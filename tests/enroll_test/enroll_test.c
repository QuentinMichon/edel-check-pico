// Banc de test : le flux complet, appairage compris.
//
//   1. le boitier demande un code d'appairage       POST /provisioning/claim
//   2. il AFFICHE le code - a toi de le saisir dans le portail operateur
//   3. il interroge toutes les 3 s                  POST /provisioning/poll
//   4. il recoit son identite et son secret         200, une seule fois
//   5. il se connecte au broker en MQTT/TLS et s'abonne a ses trois topics
//
// Rien n'est ecrit en flash : le secret ne vit qu'en RAM. Redemarrer la carte relance
// donc un appairage neuf, ce qui est exactement ce qu'on veut pour un banc - mais ce
// n'est PAS le comportement du firmware final, ou la persistance est obligatoire.
//
// Le code d'appairage sort sur la console serie, pas sur l'ecran : la police du firmware
// ne connait que A-Z, et l'alphabet des codes contient les chiffres 2-9. Les dessiner est
// l'etape suivante du plan.

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/unique_id.h"

#include "lwip/apps/mqtt.h"
#include "lwip/altcp_tls.h"
#include "lwip/ip_addr.h"

#include "wifi_setup.h"
#include "http_client.h"
#include "frozen.h"

#ifndef API_HOST
#error "API_HOST non defini (voir flash.sh)"
#endif
#ifndef BROKER_IP
#error "BROKER_IP non defini (voir flash.sh)"
#endif
#ifndef API_PORT
#define API_PORT 9080
#endif
#ifndef BROKER_PORT
#define BROKER_PORT 8883
#endif

#define WIFI_TIMEOUT_MS 30000
#define HTTP_TIMEOUT_MS 15000

// ---------------------------------------------------------------------------
// Etat de l'appairage - en RAM uniquement, cf. l'avertissement en tete de fichier.
// ---------------------------------------------------------------------------

static char hw_id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
static char pairing_code[16];
static char claim_ticket[96];

static char device_id[40];
static char device_secret[80];
static char broker_host[64];

static char http_body[4096];
static char json_req[256];

// ---------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------

static mqtt_client_t           *client;
static struct altcp_tls_config *tls_cfg;
static char topic_cfg[64], topic_cmd[64], topic_img[64], topic_evt[64], topic_status[64];

static enum { T_AUTRE, T_CFG, T_CMD, T_IMG } topic_courant;
static uint32_t rx_attendu, rx_recu;
static uint8_t  hdr[8];
static uint8_t  hdr_n;
static bool     hdr_complet;
static uint32_t nb_messages;

static void sur_abonnement(void *arg, err_t err) {
    printf("[mqtt] abonnement %-3s : %s\n", (const char *) arg,
           err == ERR_OK ? "accorde" : "REFUSE");
}

static void sur_publication(void *arg, err_t err) {
    printf("[mqtt] %s : %s\n", (const char *) arg, err == ERR_OK ? "publie":"REFUSE");
}

static void sur_topic_entrant(void *arg, const char *topic, u32_t tot_len) {
    nb_messages++;
    rx_attendu = tot_len; rx_recu = 0; hdr_n = 0; hdr_complet = false;

    if      (strcmp(topic, topic_cfg) == 0) topic_courant = T_CFG;
    else if (strcmp(topic, topic_cmd) == 0) topic_courant = T_CMD;
    else if (strcmp(topic, topic_img) == 0) topic_courant = T_IMG;
    else                                    topic_courant = T_AUTRE;

    printf("[mqtt] <- %s  %lu octets\n",
           topic_courant == T_CFG ? "cfg" : topic_courant == T_CMD ? "cmd" :
           topic_courant == T_IMG ? "img" : "?", (unsigned long) tot_len);
}

static void sur_donnees(void *arg, const u8_t *data, u16_t len, u8_t flags) {
    rx_recu += len;

    if (topic_courant == T_IMG) {
        // L'en-tete de 8 octets peut etre coupe entre deux rappels : on l'accumule.
        while (hdr_n < sizeof(hdr) && len > 0) { hdr[hdr_n++] = *data++; len--; }
        if (hdr_n == sizeof(hdr) && !hdr_complet) {
            hdr_complet = true;
            uint32_t img_id = ((uint32_t) hdr[0] << 24) | ((uint32_t) hdr[1] << 16) |
                              ((uint32_t) hdr[2] << 8)  |  (uint32_t) hdr[3];
            printf("       fragment img_id=%lu seq=%u/%u\n", (unsigned long) img_id,
                   ((uint16_t) hdr[4] << 8) | hdr[5], ((uint16_t) hdr[6] << 8) | hdr[7]);
        }
    } else if (len > 0) {
        printf("       %.*s%s\n", len > 200 ? 200 : len, (const char *) data,
               len > 200 ? "..." : "");
    }

    if (flags & MQTT_DATA_FLAG_LAST) {
        printf("       complet : %lu/%lu octets\n",
               (unsigned long) rx_recu, (unsigned long) rx_attendu);
    }
}

static void sur_connexion(mqtt_client_t *c, void *arg, mqtt_connection_status_t status) {
    if (status != MQTT_CONNECT_ACCEPTED) {
        printf("[mqtt] CONNACK : REFUSE (status %d)\n", status);
        return;
    }
    printf("[mqtt] CONNACK : accepte\n");

    static const char *online =
        "{\"state\":\"online\",\"fw\":\"banc-enrol-1.0\",\"batt\":78,\"boot_nonce\":\"c0ffee01\"}";
    mqtt_publish(c, topic_status, online, strlen(online), 1, 1, sur_publication,
                 (void *) "status/online");

    // Trois appels distincts : l'ACL refuse tout joker, y compris en position finale.
    mqtt_sub_unsub(c, topic_cfg, 1, sur_abonnement, (void *) "cfg", 1);
    mqtt_sub_unsub(c, topic_cmd, 1, sur_abonnement, (void *) "cmd", 1);
    mqtt_sub_unsub(c, topic_img, 1, sur_abonnement, (void *) "img", 1);
}

static void mqtt_demarrer(void) {
    snprintf(topic_cfg,    sizeof(topic_cfg),    "dev/%s/cfg",    device_id);
    snprintf(topic_cmd,    sizeof(topic_cmd),    "dev/%s/cmd",    device_id);
    snprintf(topic_img,    sizeof(topic_img),    "dev/%s/img",    device_id);
    snprintf(topic_evt,    sizeof(topic_evt),    "dev/%s/evt",    device_id);
    snprintf(topic_status, sizeof(topic_status), "dev/%s/status", device_id);

    // Certificat NON verifie : le certificat du broker porte CN=localhost et on compose
    // une adresse IP. L'epinglage viendra quand le SAN aura ete corrige cote edelcheck.
    tls_cfg = altcp_tls_create_config_client(NULL, 0);
    client  = mqtt_client_new();
    if (!tls_cfg || !client) { printf("[mqtt] allocation impossible\n"); return; }

    mqtt_set_inpub_callback(client, sur_topic_entrant, sur_donnees, NULL);

    static struct mqtt_connect_client_info_t ci;
    static char client_id[48];
    snprintf(client_id, sizeof(client_id), "dev-%s", device_id);

    memset(&ci, 0, sizeof(ci));
    ci.client_id   = client_id;          // impose par le serveur
    ci.client_user = device_id;
    ci.client_pass = device_secret;
    ci.keep_alive  = 60;
    ci.will_topic  = topic_status;
    ci.will_msg    = "{\"state\":\"offline\"}";
    ci.will_qos    = 1;
    ci.will_retain = 1;
    ci.tls_config  = tls_cfg;

    ip_addr_t addr;
    ipaddr_aton(BROKER_IP, &addr);
    printf("\n[mqtt] connexion a %s:%d (TLS)...\n", BROKER_IP, BROKER_PORT);

    cyw43_arch_lwip_begin();
    err_t err = mqtt_client_connect(client, &addr, BROKER_PORT, sur_connexion, NULL, &ci);
    cyw43_arch_lwip_end();
    if (err != ERR_OK) printf("[mqtt] connexion impossible : %d\n", err);
}

// ---------------------------------------------------------------------------
// Appairage
// ---------------------------------------------------------------------------

static void afficher_code(const char *code) {
    printf("\n");
    printf("    +------------------------------+\n");
    printf("    |                              |\n");
    printf("    |   CODE D'APPAIRAGE           |\n");
    printf("    |                              |\n");
    printf("    |        %-10s            |\n", code);
    printf("    |                              |\n");
    printf("    +------------------------------+\n");
    printf("\n");
    printf("    -> saisis-le dans le portail : http://localhost:4200\n");
    printf("       Boitiers > Appairer un boitier\n");
    printf("       Il expire dans 10 minutes.\n\n");
}

// Le corps en camelCase, PAS en snake_case : le contrat MQTT §8 dit l'inverse et il a
// tort - l'API repond 400 sur hw_id. Regle reelle : HTTP camelCase, MQTT snake_case.
static bool demander_code(void) {
    snprintf(json_req, sizeof(json_req), "{\"hwId\":\"%s\"}", hw_id);
    http_body[0] = '\0';

    if (!http_post(API_HOST, API_PORT, "/provisioning/claim", false,
                   "application/json", json_req,
                   "Accept: application/json\r\n", HTTP_TIMEOUT_MS,
                   http_body, sizeof(http_body))) {
        printf("[appairage] la requete de claim a echoue\n");
        return false;
    }

    char *code = NULL, *ticket = NULL;
    json_scanf(http_body, strlen(http_body),
               "{pairingCode: %Q, claimTicket: %Q}", &code, &ticket);
    if (!code || !ticket) {
        printf("[appairage] reponse inattendue : %.200s\n", http_body);
        free(code); free(ticket);
        return false;
    }

    snprintf(pairing_code, sizeof(pairing_code), "%s", code);
    snprintf(claim_ticket, sizeof(claim_ticket), "%s", ticket);
    free(code); free(ticket);
    return true;
}

// 202 revient SANS corps, 200 avec le secret, 410 avec un corps d'erreur. Le client HTTP
// ne remonte pas le code de statut : on distingue donc sur le contenu du corps, ce qui
// suffit ici mais devra etre repris proprement dans le firmware final.
typedef enum { POLL_ATTENTE, POLL_OK, POLL_ERREUR } poll_r;

static poll_r interroger(void) {
    snprintf(json_req, sizeof(json_req),
             "{\"hwId\":\"%s\",\"claimTicket\":\"%s\"}", hw_id, claim_ticket);
    http_body[0] = '\0';

    if (!http_post(API_HOST, API_PORT, "/provisioning/poll", false,
                   "application/json", json_req,
                   "Accept: application/json\r\n", HTTP_TIMEOUT_MS,
                   http_body, sizeof(http_body))) {
        return POLL_ATTENTE;                       // panne reseau : on retente
    }

    if (http_body[0] == '\0') return POLL_ATTENTE; // 202 : personne n'a encore saisi

    char *id = NULL, *secret = NULL, *host = NULL;
    int port = 0;
    json_scanf(http_body, strlen(http_body),
               "{deviceId: %Q, secret: %Q, brokerHost: %Q, brokerPort: %d}",
               &id, &secret, &host, &port);

    if (!id || !secret) {
        printf("[appairage] refus du serveur : %.200s\n", http_body);
        free(id); free(secret); free(host);
        return POLL_ERREUR;
    }

    snprintf(device_id,     sizeof(device_id),     "%s", id);
    snprintf(device_secret, sizeof(device_secret), "%s", secret);
    snprintf(broker_host,   sizeof(broker_host),   "%s", host ? host : "");
    free(id); free(secret); free(host);

    printf("\n[appairage] SECRET RECU - le boitier a une identite\n");
    printf("            deviceId   = %s\n", device_id);
    printf("            secret     = %.8s... (%u caracteres)\n",
           device_secret, (unsigned) strlen(device_secret));
    printf("            broker     = %s:%d (annonce par le serveur)\n", broker_host, port);
    printf("            -> on se connecte a %s a la place : le serveur annonce encore\n"
           "               localhost, cf. MQTT_PUBLIC_HOST dans edelcheck/.env\n", BROKER_IP);
    return POLL_OK;
}

// ---------------------------------------------------------------------------

int main(void) {
    stdio_init_all();
    sleep_ms(2500);

    pico_get_unique_board_id_string(hw_id, sizeof(hw_id));

    printf("\n=== banc APPAIRAGE + MQTT EdelCheck ===\n");
    printf("materiel : %s   (grave dans la puce, ce n'est pas un secret)\n", hw_id);
    printf("api      : http://%s:%d\n", API_HOST, API_PORT);
    printf("broker   : %s:%d\n\n", BROKER_IP, BROKER_PORT);

    if (!wifi_init()) return -1;
    while (!wifi_connect(WIFI_SSID, WIFI_PASSWORD, WIFI_TIMEOUT_MS)) {
        printf("[wifi] nouvelle tentative dans 5 s\n");
        sleep_ms(5000);
    }

    printf("\n[appairage] demande d'un code...\n");
    while (!demander_code()) sleep_ms(5000);
    afficher_code(pairing_code);

    printf("[appairage] interrogation du serveur toutes les 3 s\n");
    int tours = 0;
    while (true) {
        poll_r r = interroger();
        if (r == POLL_OK) break;
        if (r == POLL_ERREUR) {
            printf("[appairage] abandon - redemarre la carte pour un nouveau code\n");
            while (true) sleep_ms(10000);
        }
        if (++tours % 10 == 0) {
            printf("[appairage] toujours en attente (%d tentatives) - code : %s\n",
                   tours, pairing_code);
        }
        sleep_ms(3000);
    }

    mqtt_demarrer();

    uint32_t tick = 0;
    while (true) {
        sleep_ms(1000);
        if (++tick % 30 == 0) {
            cyw43_arch_lwip_begin();
            bool vivant = mqtt_client_is_connected(client) != 0;
            cyw43_arch_lwip_end();
            printf("[banc] %s · %lu message(s) recu(s)\n",
                   vivant ? "en ligne" : "DECONNECTE", (unsigned long) nb_messages);
        }
    }
}
