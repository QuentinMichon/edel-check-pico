#include "mqtt_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "lwip/apps/mqtt.h"
#include "lwip/altcp_tls.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "hardware/watchdog.h"

#include "frozen.h"

#include "storage/storage_manager.h"
#include "profiles/profiles.h"
#include "image/image_rx.h"
#include "screen/epd_driver.h"
#include "screen/epd_text.h"

#ifndef EDEL_MQTT_PORT_DEFAUT
#define EDEL_MQTT_PORT_DEFAUT 8883
#endif

#define RECONNEXION_MS 10000
#define DNS_TIMEOUT_MS 8000

// Le testament, mot pour mot comme le contrat l'exige (§1). C'est lui qui fait remonter
// l'etat hors ligne au portail sans aucune interrogation periodique : le broker le publie
// A LA PLACE du boitier quand la connexion tombe, ce qu'un appareil debranche ne peut
// evidemment pas faire lui-meme.
static const char *TESTAMENT = "{\"state\":\"offline\"}";

static mqtt_client_t           *client;
static struct altcp_tls_config *tls_cfg;

static char topic_cfg[80], topic_cmd[80], topic_img[80], topic_evt[80], topic_status[80];
static char client_id[48];
static char presence[128];

static bool     demarre;
static uint32_t derniere_tentative_ms;

// Ecran d'erreur transitoire : au bout de ce delai, l'interface reprend la main.
// 4 s - le temps de lire trois mots, sans laisser le comptoir devant un ecran mort.
#define ERREUR_AFFICHAGE_MS 4000
static uint32_t erreur_affichee_ms;
static void (*ui_restore_cb)(void);

// ---------------------------------------------------------------------------
// Reception
// ---------------------------------------------------------------------------
//
// lwIP livre le topic UNE fois, puis la charge utile en N tranches de <= TCP_MSS octets.
// Le topic n'est plus disponible dans le rappel de donnees : il faut donc le memoriser.

static enum { T_AUTRE, T_CFG, T_CMD, T_IMG } topic_courant;
static uint32_t rx_attendu, rx_recu;

// Une image complete attend d'etre poussee sur la dalle.
//
// ⚠ Le rafraichissement ne peut PAS avoir lieu dans le rappel de reception : il dure
// 638 ms en partiel, pendant lesquelles il pilote le SPI et attend le signal BUSY du
// panneau. Bloquer aussi longtemps le fil d'execution de lwIP ferait expirer le
// keep-alive MQTT et tomber la connexion - le boitier se deconnecterait a chaque image
// affichee. On leve donc un drapeau, et la boucle principale fait le travail.
static volatile bool image_a_afficher;

// Chronometre d'abandon (contrat MQTT §3 et §4).
//
// REARME A CHAQUE FRAGMENT, pas a l'ouverture de l'image : un reseau lent qui livre huit
// fragments en douze secondes est parfaitement normal, alors qu'un silence de dix secondes
// au milieu d'une image ne l'est pas. Armer une seule fois au debut abandonnerait le
// premier cas et raterait le second.
#define IMG_ABANDON_MS 10000
static volatile uint32_t derniere_tranche_ms;

// Rassemblage des charges utiles JSON (cfg et cmd).
//
// Meme raison que pour l'en-tete d'image, avec une consequence plus grave : un JSON coupe
// en deux ne produit pas une erreur, il produit un objet incomplet que l'analyseur rejette
// en silence. Le symptome serait " le boitier ne recoit jamais sa configuration ", alors
// que le message arrive parfaitement.
//
// 2 Ko : huit profils font environ 1 Ko, l'en-tete quelques dizaines d'octets.
#define JSON_BUF_SIZE 2048
static char   json_buf[JSON_BUF_SIZE];
static size_t json_len;
static bool   json_tronque;

static void sur_abonnement(void *arg, err_t err) {
    // L'ACL du serveur refuse tout joker et repond topic par topic : on trace chaque
    // abonnement separement, sinon un seul refus ressemble a " MQTT ne marche pas ".
    printf("[mqtt] abonnement %-3s : %s\n", (const char *) arg,
           err == ERR_OK ? "accorde" : "REFUSE");
}

static void sur_publication(void *arg, err_t err) {
    if (err != ERR_OK) {
        printf("[mqtt] publication %s refusee (%d)\n", (const char *) arg, err);
    }
}

static void sur_topic_entrant(void *arg, const char *topic, u32_t tot_len) {
    rx_attendu = tot_len;
    rx_recu = 0;

    json_len = 0;
    json_tronque = false;

    if (strcmp(topic, topic_img) == 0) {
        image_rx_begin();
    }

    if      (strcmp(topic, topic_cfg) == 0) topic_courant = T_CFG;
    else if (strcmp(topic, topic_cmd) == 0) topic_courant = T_CMD;
    else if (strcmp(topic, topic_img) == 0) topic_courant = T_IMG;
    else                                    topic_courant = T_AUTRE;

    // Charge utile vide sur un topic retenu : le message conserve a ete efface, ce que le
    // serveur fait a la revocation. lwIP n'appelle alors pas forcement le rappel de
    // donnees, donc on traite le cas ici.
    if (tot_len == 0 && topic_courant == T_CFG) {
        profiles_handle_cfg(NULL, 0);
    }
}

// Les commandes du contrat (§3). session_ready et result n'ont rien a faire ici : leurs
// metadonnees servent au confort, l'en-tete binaire des fragments porte deja tout ce qu'il
// faut pour reassembler. Seule `revoked` demande une action.
static void traiter_commande(const char *json, size_t len) {
    char *type = NULL, *raison = NULL;
    json_scanf(json, (int) len, "{type: %Q, reason: %Q}", &type, &raison);

    if (type != NULL && strcmp(type, "revoked") == 0) {
        printf("[mqtt] REVOCATION recue (%s) - effacement de l'identite\n",
               raison ? raison : "sans motif");

        // Le serveur a deja efface les empreintes de son cote : cette commande sert a ce
        // qu'un boitier ENCORE connecte reagisse tout de suite, plutot que de boucler sur
        // des connexions refusees sans rien afficher.
        storage_clear_enrollment();

        epd_fb_clear(true);
        epd_fb_write_big_centered(110, "BOITIER REVOQUE", 3);
        epd_fb_write_big_centered(164, "NOUVEL APPAIRAGE", 2);
        epd_fb_write_big_centered(190, "AU REDEMARRAGE", 2);
        epd_display_update_full();

        // Redemarrage plutot qu'un retour en arriere dans le code : sans identite, le
        // chemin de demarrage normal relance l'appairage tout seul. Reproduire cette
        // sequence ici creerait un second chemin qui divergerait du premier.
        printf("[mqtt] redemarrage dans 3 s\n");
        sleep_ms(3000);
        watchdog_reboot(0, 0, 0);
    } else if (type != NULL) {
        printf("[mqtt] cmd %s\n", type);
    }

    free(type);
    free(raison);
}

static void sur_donnees(void *arg, const u8_t *data, u16_t len, u8_t flags) {
    rx_recu += len;

    if (topic_courant == T_IMG) {
        // Les octets sont convertis et ecrits dans le framebuffer a mesure qu'ils
        // arrivent : aucun tampon intermediaire de 30 Ko n'existe.
        image_rx_data(data, len);
        derniere_tranche_ms = to_ms_since_boot(get_absolute_time());

        if (flags & MQTT_DATA_FLAG_LAST) {
            if (image_rx_end()) {
                printf("[img] image %lu complete - %lu octets dans le framebuffer\n",
                       (unsigned long) image_rx_current_id(),
                       (unsigned long) EPD_BUFFER_SIZE);
                image_a_afficher = true;
            } else {
                int manquants = image_rx_missing();
                if (manquants > 0) {
                    printf("[img] image %lu : %d fragment(s) encore attendu(s)\n",
                           (unsigned long) image_rx_current_id(), manquants);
                }
            }
        }
    } else if (topic_courant == T_CFG || topic_courant == T_CMD) {
        if (json_len + len < sizeof(json_buf)) {
            memcpy(json_buf + json_len, data, len);
            json_len += len;
        } else {
            json_tronque = true;
        }

        if (flags & MQTT_DATA_FLAG_LAST) {
            if (json_tronque) {
                // Le dire fort : un JSON tronque analyse quand meme donnerait une
                // configuration partielle, donc un menu faux, sans aucune erreur visible.
                printf("[mqtt] %s TRONQUE (%lu octets recus, tampon de %u) - ignore\n",
                       topic_courant == T_CFG ? "cfg" : "cmd",
                       (unsigned long) rx_attendu, (unsigned) sizeof(json_buf));
            } else {
                json_buf[json_len] = '\0';
                if (topic_courant == T_CFG) {
                    profiles_handle_cfg(json_buf, json_len);
                } else {
                    traiter_commande(json_buf, json_len);
                }
            }
        }
    }
}

static void sur_connexion(mqtt_client_t *c, void *arg, mqtt_connection_status_t status) {
    if (status != MQTT_CONNECT_ACCEPTED) {
        // status 5 : identifiants invalides, boitier revoque, ou client_id different de
        // dev-{deviceId}. Reessayer ne changera rien tant que rien n'a bouge cote serveur.
        printf("[mqtt] connexion refusee (status %d)\n", status);
        return;
    }
    printf("[mqtt] connecte au broker\n");

    mqtt_publish(c, topic_status, presence, strlen(presence), 1, 1,
                 sur_publication, (void *) "status");

    // Trois appels distincts, jamais dev/{id}/# : l'ACL refuse tout joker, y compris en
    // position finale.
    mqtt_sub_unsub(c, topic_cfg, 1, sur_abonnement, (void *) "cfg", 1);
    mqtt_sub_unsub(c, topic_cmd, 1, sur_abonnement, (void *) "cmd", 1);
    mqtt_sub_unsub(c, topic_img, 1, sur_abonnement, (void *) "img", 1);
}

// ---------------------------------------------------------------------------

static volatile bool dns_fait;
static ip_addr_t     broker_addr;

static void sur_dns(const char *nom, const ip_addr_t *addr, void *arg) {
    if (addr != NULL) {
        broker_addr = *addr;
    }
    dns_fait = true;
}

// L'adresse du broker vient de la flash : ce peut etre une IP litterale ou un nom.
static bool resoudre(const char *hote) {
    if (ipaddr_aton(hote, &broker_addr)) {
        return true;
    }

    ip_addr_set_zero(&broker_addr);
    dns_fait = false;

    cyw43_arch_lwip_begin();
    err_t err = dns_gethostbyname(hote, &broker_addr, sur_dns, NULL);
    cyw43_arch_lwip_end();

    if (err == ERR_OK) {
        return true;                       // deja en cache
    }
    if (err != ERR_INPROGRESS) {
        return false;
    }

    absolute_time_t fin = make_timeout_time_ms(DNS_TIMEOUT_MS);
    while (!dns_fait && !time_reached(fin)) {
        sleep_ms(10);
    }
    return !ip_addr_isany(&broker_addr);
}

static void connecter(void) {
    persistent_storage_t *cfg = get_local_storage();

    const char *hote = cfg->broker_host[0] ? cfg->broker_host : "";
    uint16_t port = cfg->broker_port ? cfg->broker_port : EDEL_MQTT_PORT_DEFAUT;

#ifdef EDEL_BROKER_HOST_OVERRIDE
    // Bequille de developpement : tant que MQTT_PUBLIC_HOST vaut `localhost` cote
    // edelcheck, le serveur annonce une adresse que le boitier ne peut pas joindre.
    // A retirer des que la variable est corrigee - en production, l'adresse du broker
    // DOIT venir de l'appairage, sinon demenager l'infrastructure imposerait de reflasher
    // tout le parc.
    hote = EDEL_BROKER_HOST_OVERRIDE;
    printf("[mqtt] adresse forcee a la compilation : %s\n", hote);
#endif

    if (hote[0] == '\0') {
        printf("[mqtt] aucune adresse de broker connue\n");
        return;
    }
    if (!resoudre(hote)) {
        printf("[mqtt] impossible de resoudre %s\n", hote);
        return;
    }

    static struct mqtt_connect_client_info_t ci;
    memset(&ci, 0, sizeof(ci));
    ci.client_id   = client_id;          // dev-{deviceId}, impose par le serveur
    ci.client_user = cfg->device_id;
    ci.client_pass = cfg->device_secret;
    ci.keep_alive  = 60;
    ci.will_topic  = topic_status;
    ci.will_msg    = TESTAMENT;
    ci.will_qos    = 1;
    ci.will_retain = 1;
    ci.tls_config  = tls_cfg;
    // clean_session est toujours pose par lwIP (mqtt.c:1376), ce que le contrat exige :
    // sans lui, les images d'un boitier hors ligne s'empilent dans le broker et sont
    // rejouees a la reconnexion - au risque de reafficher un verdict devant quelqu'un
    // d'autre.

    printf("[mqtt] connexion a %s:%u (TLS)...\n", ipaddr_ntoa(&broker_addr), port);

    cyw43_arch_lwip_begin();
    err_t err = mqtt_client_connect(client, &broker_addr, port, sur_connexion, NULL, &ci);
    cyw43_arch_lwip_end();

    if (err != ERR_OK) {
        printf("[mqtt] connexion impossible (%d)\n", err);
    }
}

bool edel_mqtt_start(void) {
    if (!storage_is_enrolled()) {
        printf("[mqtt] boitier non appaire - connexion impossible\n");
        return false;
    }

    persistent_storage_t *cfg = get_local_storage();

    snprintf(client_id,    sizeof(client_id),    "dev-%s",        cfg->device_id);
    snprintf(topic_cfg,    sizeof(topic_cfg),    "dev/%s/cfg",    cfg->device_id);
    snprintf(topic_cmd,    sizeof(topic_cmd),    "dev/%s/cmd",    cfg->device_id);
    snprintf(topic_img,    sizeof(topic_img),    "dev/%s/img",    cfg->device_id);
    snprintf(topic_evt,    sizeof(topic_evt),    "dev/%s/evt",    cfg->device_id);
    snprintf(topic_status, sizeof(topic_status), "dev/%s/status", cfg->device_id);

    // boot_nonce : tire au demarrage et fige ensuite. Le serveur s'en sert pour distinguer
    // deux appareils portant la meme identite - deux `online` avec des valeurs differentes
    // sans `offline` intercale signalent un clone.
    snprintf(presence, sizeof(presence),
             "{\"state\":\"online\",\"fw\":\"1.0.6\",\"batt\":100,\"boot_nonce\":\"%08lx\"}",
             (unsigned long) to_us_since_boot(get_absolute_time()));

    // ⚠ Certificat NON verifie tant que la CA n'est pas epinglee.
    //
    // cfg->ca_cert_pem contient pourtant la bonne autorite, recue a l'appairage. Deux
    // choses manquent avant de pouvoir s'en servir : le certificat du broker de
    // developpement porte CN=localhost et ne couvre pas l'adresse reelle (son
    // subjectAltName est ecrit en dur dans edelcheck/scripts/dev-up.sh), et il faut
    // epingler la CA RACINE, jamais la feuille - sinon un renouvellement de certificat
    // fait tomber tout le parc, sans recours apres flashage.
    tls_cfg = altcp_tls_create_config_client(NULL, 0);
    client  = mqtt_client_new();
    if (tls_cfg == NULL || client == NULL) {
        printf("[mqtt] allocation impossible\n");
        return false;
    }

    // A poser AVANT la connexion : sans ces rappels, lwIP jette chaque message entrant en
    // silence, et le symptome ressemble a " le cloud ne repond jamais ".
    mqtt_set_inpub_callback(client, sur_topic_entrant, sur_donnees, NULL);

    demarre = true;
    connecter();
    derniere_tentative_ms = to_ms_since_boot(get_absolute_time());
    return true;
}

void edel_mqtt_poll(void) {
    // L'affichage se fait ICI, hors du rappel reseau : cf. le commentaire de
    // `image_a_afficher`. Rafraichissement PARTIEL - 638 ms mesurees contre 2,3 s pour un
    // plein, et l'ecart se voit devant un client qui attend.
    if (image_a_afficher) {
        image_a_afficher = false;
        derniere_tranche_ms = 0;
        epd_display_update_partial();
        printf("[img] affichee\n");
    }

    // Abandon d'une image restee incomplete.
    //
    // Le serveur NE RETRANSMET PAS : l'evenement img_abort sert au diagnostic, pas a la
    // reparation. Mais il faut l'envoyer - une image perdue en silence laisse la dalle
    // figee sur l'ecran precedent, sans que personne, ni au comptoir ni au portail, ne
    // sache pourquoi.
    int manquants = image_rx_missing();
    if (manquants > 0 && derniere_tranche_ms != 0
        && to_ms_since_boot(get_absolute_time()) - derniere_tranche_ms > IMG_ABANDON_MS) {

        char liste[96], evenement[160];
        image_rx_missing_json(liste, sizeof(liste));   // avant le reset : il lit l'etat
        snprintf(evenement, sizeof(evenement),
                 "{\"type\":\"img_abort\",\"img_id\":%lu,\"missing\":%s}",
                 (unsigned long) image_rx_current_id(), liste);

        printf("[img] image %lu ABANDONNEE - fragments manquants : %s\n",
               (unsigned long) image_rx_current_id(), liste);
        edel_mqtt_publish_evt(evenement);

        image_rx_reset();
        derniere_tranche_ms = 0;

        // Le dire aussi a l'ecran : le framebuffer contient une image a moitie ecrite,
        // l'afficher telle quelle serait pire que de ne rien montrer.
        epd_fb_clear(true);
        epd_fb_write_big_centered(112, "IMAGE INCOMPLETE", 3);
        epd_fb_write_big_centered(168, "RELANCER LA VERIFICATION", 2);
        epd_display_update_partial();
        erreur_affichee_ms = to_ms_since_boot(get_absolute_time());
    }

    // Rendre la main a l'interface : le message a ete lu, on reaffiche le menu pour que
    // l'operateur voie de nouveau quoi appuyer.
    if (erreur_affichee_ms != 0
        && to_ms_since_boot(get_absolute_time()) - erreur_affichee_ms > ERREUR_AFFICHAGE_MS) {
        erreur_affichee_ms = 0;
        if (ui_restore_cb != NULL) {
            ui_restore_cb();
        }
    }

    if (!demarre || edel_mqtt_is_connected()) {
        return;
    }
    uint32_t maintenant = to_ms_since_boot(get_absolute_time());
    if (maintenant - derniere_tentative_ms < RECONNEXION_MS) {
        return;
    }
    derniere_tentative_ms = maintenant;
    connecter();
}

bool edel_mqtt_is_connected(void) {
    if (client == NULL) {
        return false;
    }
    cyw43_arch_lwip_begin();
    bool vivant = mqtt_client_is_connected(client) != 0;
    cyw43_arch_lwip_end();
    return vivant;
}

void edel_mqtt_set_ui_restore_cb(void (*cb)(void)) {
    ui_restore_cb = cb;
}

bool edel_mqtt_publish_evt(const char *json) {
    if (json == NULL || !edel_mqtt_is_connected()) {
        return false;
    }
    cyw43_arch_lwip_begin();
    err_t err = mqtt_publish(client, topic_evt, json, strlen(json), 1, 0,
                             sur_publication, (void *) "evt");
    cyw43_arch_lwip_end();
    return err == ERR_OK;
}
