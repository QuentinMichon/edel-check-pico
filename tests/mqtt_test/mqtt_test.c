// Banc de test : le boitier parle MQTT sur TLS au broker EdelCheck.
//
// Ce banc ne fait qu'UNE chose et l'imprime : prouver que le Pico sait tenir une
// connexion MQTT/TLS authentifiee et s'abonner a ses trois topics. Aucun appairage,
// aucun ecran, aucune ecriture en flash - les identifiants du boitier arrivent par
// -D au moment de la compilation.
//
// Ce qu'on attend a l'ecran serie, dans l'ordre :
//
//   [wifi]  connected, ip = ...
//   [mqtt]  CONNACK : accepte
//   [mqtt]  status/online publie (retenu)
//   [mqtt]  abonnement cfg / cmd / img : accorde        <- TROIS lignes distinctes
//   [mqtt]  <- cfg  N octets                            <- le cloud repond tout seul
//
// La derniere ligne est le vrai signal de reussite : `cfg` est retenu et republie par
// device-service a la reception du message de presence. La voir arriver prouve que la
// boucle complete boitier -> cloud -> boitier fonctionne.

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "lwip/apps/mqtt.h"
#include "lwip/altcp_tls.h"
#include "lwip/ip_addr.h"

#include "wifi_setup.h"

#ifndef DEVICE_ID
#error "DEVICE_ID non defini : passer -DDEVICE_ID=... (voir flash.sh)"
#endif
#ifndef DEVICE_SECRET
#error "DEVICE_SECRET non defini : passer -DDEVICE_SECRET=... (voir flash.sh)"
#endif
#ifndef BROKER_IP
#error "BROKER_IP non defini : passer -DBROKER_IP=... (voir flash.sh)"
#endif
#ifndef BROKER_PORT
#define BROKER_PORT 8883
#endif

#define WIFI_TIMEOUT_MS 30000

// Le contrat impose ce testament, mot pour mot. C'est lui qui fait remonter l'etat hors
// ligne au portail sans aucune interrogation periodique : le broker le publie A NOTRE
// PLACE quand la connexion tombe, ce qu'un boitier debranche ne peut evidemment pas faire.
static const char *WILL_PAYLOAD = "{\"state\":\"offline\"}";

static mqtt_client_t          *client;
static struct altcp_tls_config *tls_cfg;

static char topic_cfg[64], topic_cmd[64], topic_img[64], topic_status[64];

// Quel topic est en cours de reception.
//
// lwIP livre le topic UNE fois (publish_cb) puis la charge utile en N tranches
// (data_cb) de <= TCP_MSS octets. Le topic n'est plus disponible dans data_cb : il faut
// donc le memoriser ici. C'est le "brancher img avant tout parsing" du simulateur,
// transpose en C.
static enum { T_AUTRE, T_CFG, T_CMD, T_IMG } topic_courant;
static uint32_t rx_attendu, rx_recu;

// L'en-tete binaire d'un fragment d'image fait 8 octets et peut etre COUPE EN DEUX
// entre deux rappels. On l'accumule donc au lieu de le lire d'un bloc.
static uint8_t  hdr[8];
static uint8_t  hdr_n;
static bool     hdr_complet;

static volatile bool connecte;
static uint32_t nb_connexions, nb_messages;

// ---------------------------------------------------------------------------
// Rappels
// ---------------------------------------------------------------------------

static void sur_abonnement(void *arg, err_t err) {
    const char *nom = (const char *) arg;
    if (err == ERR_OK) {
        printf("[mqtt] abonnement %-3s : accorde\n", nom);
    } else {
        // L'ACL du serveur refuse TOUT joker et repond topic par topic. Un seul refus
        // ne veut donc pas dire "MQTT ne marche pas" - d'ou les trois lignes separees.
        printf("[mqtt] abonnement %-3s : REFUSE (err %d)\n", nom, err);
    }
}

static void sur_publication(void *arg, err_t err) {
    printf("[mqtt] status/online %s\n",
           err == ERR_OK ? "publie (retenu)" : "REFUSE");
}

static void sur_topic_entrant(void *arg, const char *topic, u32_t tot_len) {
    nb_messages++;
    rx_attendu  = tot_len;
    rx_recu     = 0;
    hdr_n       = 0;
    hdr_complet = false;

    if      (strcmp(topic, topic_cfg) == 0) topic_courant = T_CFG;
    else if (strcmp(topic, topic_cmd) == 0) topic_courant = T_CMD;
    else if (strcmp(topic, topic_img) == 0) topic_courant = T_IMG;
    else                                    topic_courant = T_AUTRE;

    printf("[mqtt] <- %s  %lu octets\n",
           topic_courant == T_CFG ? "cfg" :
           topic_courant == T_CMD ? "cmd" :
           topic_courant == T_IMG ? "img" : "?",
           (unsigned long) tot_len);
}

static void sur_donnees(void *arg, const u8_t *data, u16_t len, u8_t flags) {
    rx_recu += len;

    if (topic_courant == T_IMG) {
        // Accumuler l'en-tete tant qu'il est incomplet, sans supposer qu'il tient
        // entierement dans cette tranche.
        while (hdr_n < sizeof(hdr) && len > 0) {
            hdr[hdr_n++] = *data++;
            len--;
        }
        if (hdr_n == sizeof(hdr) && !hdr_complet) {
            hdr_complet = true;
            uint32_t img_id = ((uint32_t) hdr[0] << 24) | ((uint32_t) hdr[1] << 16) |
                              ((uint32_t) hdr[2] << 8)  |  (uint32_t) hdr[3];
            uint16_t seq    = ((uint16_t) hdr[4] << 8) | hdr[5];
            uint16_t total  = ((uint16_t) hdr[6] << 8) | hdr[7];
            printf("       fragment img_id=%lu seq=%u/%u\n",
                   (unsigned long) img_id, seq, total);
        }
    } else if (len > 0) {
        // cfg et cmd sont du JSON : on imprime la premiere tranche telle quelle, c'est
        // suffisant pour ce banc - le parsing viendra avec le vrai client.
        printf("       %.*s%s\n", len > 180 ? 180 : len, (const char *) data,
               len > 180 ? "..." : "");
    }

    if (flags & MQTT_DATA_FLAG_LAST) {
        printf("       complet : %lu/%lu octets\n",
               (unsigned long) rx_recu, (unsigned long) rx_attendu);
    }
}

static void sur_connexion(mqtt_client_t *c, void *arg, mqtt_connection_status_t status) {
    if (status != MQTT_CONNECT_ACCEPTED) {
        connecte = false;
        // 5 = refuse par le serveur : identifiants invalides, boitier revoque, ou
        // client_id different de dev-{deviceId}. Reessayer n'y changera rien.
        printf("[mqtt] CONNACK : REFUSE (status %d)%s\n", status,
               status == MQTT_CONNECT_REFUSED_NOT_AUTHORIZED_
                   ? " - identifiants, revocation, ou client_id" : "");
        return;
    }

    connecte = true;
    nb_connexions++;
    printf("[mqtt] CONNACK : accepte\n");

    // Presence. Retenu, pour que le portail retrouve l'etat sans interroger personne.
    static const char *online =
        "{\"state\":\"online\",\"fw\":\"banc-1.0\",\"batt\":78,\"boot_nonce\":\"deadbeef\"}";
    mqtt_publish(c, topic_status, online, strlen(online), 1, 1, sur_publication, NULL);

    // TROIS appels distincts, et pas dev/{id}/# : l'ACL du serveur refuse tout joker,
    // y compris en position finale.
    mqtt_sub_unsub(c, topic_cfg, 1, sur_abonnement, (void *) "cfg", 1);
    mqtt_sub_unsub(c, topic_cmd, 1, sur_abonnement, (void *) "cmd", 1);
    mqtt_sub_unsub(c, topic_img, 1, sur_abonnement, (void *) "img", 1);
}

// ---------------------------------------------------------------------------

static void connecter(void) {
    ip_addr_t addr;
    if (!ipaddr_aton(BROKER_IP, &addr)) {
        printf("[mqtt] BROKER_IP invalide : %s\n", BROKER_IP);
        return;
    }

    struct mqtt_connect_client_info_t ci = {0};
    ci.client_id   = "dev-" DEVICE_ID;   // impose par le serveur, sinon refus
    ci.client_user = DEVICE_ID;
    ci.client_pass = DEVICE_SECRET;
    ci.keep_alive  = 60;
    ci.will_topic  = topic_status;
    ci.will_msg    = WILL_PAYLOAD;
    ci.will_qos    = 1;
    ci.will_retain = 1;
    ci.tls_config  = tls_cfg;
    // clean_session est TOUJOURS pose par lwIP (mqtt.c:1376), ce qui tombe juste :
    // le contrat l'exige, sinon les images d'un boitier hors ligne s'empilent dans le
    // broker et sont rejouees a la reconnexion.

    printf("[mqtt] connexion a %s:%d (TLS)...\n", BROKER_IP, BROKER_PORT);

    cyw43_arch_lwip_begin();
    err_t err = mqtt_client_connect(client, &addr, BROKER_PORT, sur_connexion, NULL, &ci);
    cyw43_arch_lwip_end();

    if (err != ERR_OK) {
        printf("[mqtt] mqtt_client_connect a echoue : %d\n", err);
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    printf("\n=== banc MQTT/TLS EdelCheck ===\n");
    printf("boitier : %s\n", DEVICE_ID);
    printf("broker  : %s:%d\n\n", BROKER_IP, BROKER_PORT);

    snprintf(topic_cfg,    sizeof(topic_cfg),    "dev/%s/cfg",    DEVICE_ID);
    snprintf(topic_cmd,    sizeof(topic_cmd),    "dev/%s/cmd",    DEVICE_ID);
    snprintf(topic_img,    sizeof(topic_img),    "dev/%s/img",    DEVICE_ID);
    snprintf(topic_status, sizeof(topic_status), "dev/%s/status", DEVICE_ID);

    if (!wifi_init()) {
        printf("[wifi] init impossible - arret\n");
        return -1;
    }
    while (!wifi_connect(WIFI_SSID, WIFI_PASSWORD, WIFI_TIMEOUT_MS)) {
        printf("[wifi] nouvelle tentative dans 5 s\n");
        sleep_ms(5000);
    }

    // CA a NULL : on ne verifie PAS le certificat, et c'est delibere pour ce banc.
    //
    // Le certificat du broker porte CN=localhost avec SAN localhost/mosquitto/127.0.0.1,
    // alors qu'on compose une adresse IP du reseau local. Epingler la CA ferait donc
    // echouer la verification du nom d'hote - un second echec en forme de -15 empile sur
    // celui qu'on vient d'eliminer, et on deboguerait la mauvaise couche.
    //
    // L'epinglage viendra quand le SAN du certificat aura ete corrige cote edelcheck.
    tls_cfg = altcp_tls_create_config_client(NULL, 0);
    if (!tls_cfg) {
        printf("[tls] configuration client impossible - arret\n");
        return -1;
    }

    client = mqtt_client_new();
    if (!client) {
        printf("[mqtt] allocation du client impossible - arret\n");
        return -1;
    }

    // A poser AVANT la connexion. Sans ces deux rappels, lwIP jette chaque message
    // entrant en silence - et le symptome ressemble a "le cloud ne repond jamais".
    mqtt_set_inpub_callback(client, sur_topic_entrant, sur_donnees, NULL);

    connecter();

    uint32_t tick = 0;
    while (true) {
        sleep_ms(1000);
        tick++;

        cyw43_arch_lwip_begin();
        bool vivant = mqtt_client_is_connected(client) != 0;
        cyw43_arch_lwip_end();

        if (!vivant && connecte) {
            connecte = false;
            printf("[mqtt] connexion perdue\n");
        }
        if (!vivant && (tick % 10) == 0) {
            connecter();
        }
        if (vivant && (tick % 30) == 0) {
            printf("[banc] en ligne · %lu connexion(s) · %lu message(s) recu(s)\n",
                   (unsigned long) nb_connexions, (unsigned long) nb_messages);
        }
    }
}
