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
#include "session/session.h"
#include "screen/epd_driver.h"
#include "screen/epd_text.h"
#include "power/power.h"

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

// Ecrans transitoires : au bout de leur delai, l'interface reprend la main.
//
// Deux cas, deux durees. Une erreur tient en trois mots et se lit vite ; un verdict est
// montre a une personne qui vient de scanner, et le lui retirer au bout de quatre secondes
// serait brusque. Le QR, lui, n'a PAS de delai : c'est le serveur qui borne l'attente a
// 120 s et pousse ensuite un verdict "Delai depasse", lequel arme ce chronometre.
#define ERREUR_AFFICHAGE_MS  4000
#define VERDICT_AFFICHAGE_MS 8000

// Instant d'armement (0 = desarme) et delai a respecter.
static uint32_t retour_menu_ms;
static uint32_t retour_menu_delai_ms;
static void (*ui_restore_cb)(void);

// Une configuration vient d'etre adoptee : le menu affiche est perime.
//
// Un DRAPEAU, pas un appel direct. profiles_handle_cfg() tourne dans le rappel de lwIP, et
// redessiner sur place bloquerait ce fil 638 ms - le keep-alive MQTT expirerait et le
// boitier se deconnecterait a chaque changement de configuration. La boucle principale
// s'en charge, comme pour les images.
static volatile bool cfg_a_redessiner;

// Un QR est affiche et on attend le verdict. Redessiner le menu maintenant l'effacerait
// sous les yeux du client en train de scanner. On attend : le verdict rendra la main au
// menu de toute facon, et le menu redessine sera le bon.
static volatile bool qr_affiche;

// Les images annoncees par les metadonnees recues sur `cmd` AVANT leurs fragments.
// 0 = aucune.
//
// On retient l'identifiant plutot qu'un simple drapeau : deux images peuvent se succeder
// vite - le QR puis le verdict - et un booleen consomme au mauvais moment renverrait au
// menu un boitier qui vient d'afficher un QR, juste avant que le client ne le scanne.
static volatile uint32_t img_id_verdict;
static volatile uint32_t img_id_qr;

static void armer_retour_menu(uint32_t delai_ms) {
    retour_menu_ms = to_ms_since_boot(get_absolute_time());
    retour_menu_delai_ms = delai_ms;
}

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
// en silence. Le symptome serait "le boitier ne recoit jamais sa configuration", alors
// que le message arrive parfaitement.
//
// 2 Ko : huit profils font environ 1 Ko, l'en-tete quelques dizaines d'octets.
#define JSON_BUF_SIZE 2048
static char   json_buf[JSON_BUF_SIZE];
static size_t json_len;
static bool   json_tronque;

static void sur_abonnement(void *arg, err_t err) {
    // L'ACL du serveur refuse tout joker et repond topic par topic : on trace chaque
    // abonnement separement, sinon un seul refus ressemble a "MQTT ne marche pas".
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
    int img_id = 0;
    json_scanf(json, (int) len, "{type: %Q, reason: %Q, img_id: %d}", &type, &raison, &img_id);

    // Un verdict doit rendre la main au menu une fois lu ; un QR doit rester affiche le
    // temps que le client le scanne. Seules ces metadonnees permettent de les distinguer -
    // l'en-tete binaire des fragments ne porte que de quoi reassembler.
    if (type != NULL && img_id > 0) {
        if (strcmp(type, "result") == 0) {
            img_id_verdict = (uint32_t) img_id;
        } else if (strcmp(type, "session_ready") == 0) {
            img_id_qr = (uint32_t) img_id;
        }
    }

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
                    // Le menu ne se redessinait pas : l'operateur assignait un profil depuis
                    // le portail, le boitier l'adoptait aussitot, et l'ecran continuait
                    // d'afficher l'ancienne liste jusqu'a ce que quelqu'un appuie sur une
                    // touche. Vu du comptoir, on croit a une minute de latence reseau.
                    if (profiles_handle_cfg(json_buf, json_len)) {
                        cfg_a_redessiner = true;
                    }
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
    // 15 s et non 60.
    //
    // Le keep-alive ne sert pas qu'a detecter une connexion morte : c'est le SEUL trafic
    // montant d'un boitier au repos, et c'est lui qui maintient ouverte la traduction
    // d'adresse du routeur. Mesure sur un partage de connexion telephone : une
    // configuration poussee vers un boitier inactif mettait environ soixante secondes a
    // arriver - exactement l'ancien intervalle - alors qu'une image publiee juste apres un
    // message montant du boitier arrivait en cinquante millisecondes. Le passage etait
    // referme, et seul le PINGREQ suivant le rouvrait.
    //
    // Le cout est negligeable : deux octets toutes les quinze secondes.
    ci.keep_alive  = 15;
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
             // `batt` etait ecrit EN DUR a 100 : le boitier annoncait une batterie pleine
             // qu'il n'avait jamais mesuree. Il la lit maintenant sur VSYS.
             "{\"state\":\"online\",\"fw\":\"1.0.6\",\"batt\":%d,\"boot_nonce\":\"%08lx\"}",
             power_niveau_pourcent(),
             (unsigned long) to_us_since_boot(get_absolute_time()));

    // Certificat du broker VERIFIE contre l'autorite recue a l'appairage.
    //
    // C'est la CA RACINE qui est epinglee, jamais la feuille : epingler le certificat du
    // broker rendrait tout renouvellement impossible sans reflasher le parc entier.
    //
    // La longueur INCLUT le zero terminal. mbedtls_x509_crt_parse distingue le PEM du DER a
    // ce detail pres : sans lui, il tente une lecture binaire, echoue, et la connexion tombe
    // avec une erreur qui ne parle pas du certificat.
    //
    // Le certificat du broker doit couvrir l'adresse que le boitier compose reellement. Le
    // subjectAltName se regle par MQTT_EXTRA_SAN dans edelcheck/scripts/dev-up.sh ; une
    // adresse absente donne un refus de connexion, muet cote serveur.
    size_t ca_len = strlen(cfg->ca_cert_pem);
    if (ca_len == 0) {
        printf("[mqtt] aucune autorite en flash - connexion refusee\n");
        return false;
    }
    printf("[tls] verification du certificat activee (autorite de %u octets)\n",
           (unsigned) ca_len);
    tls_cfg = altcp_tls_create_config_client((const u8_t *) cfg->ca_cert_pem, ca_len + 1);
    client  = mqtt_client_new();
    if (tls_cfg == NULL || client == NULL) {
        printf("[mqtt] allocation impossible\n");
        return false;
    }

    // A poser AVANT la connexion : sans ces rappels, lwIP jette chaque message entrant en
    // silence, et le symptome ressemble a "le cloud ne repond jamais".
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

        uint32_t affichee = image_rx_current_id();

        // L'operateur a quitte la page avant le verdict. Le serveur, lui, n'en sait rien :
        // le contrat n'a pas de message d'abandon, et il publiera quand meme. Afficher
        // reprendrait la dalle sous les yeux de quelqu'un passe a autre chose.
        if (session_est_abandonnee()) {
            printf("[session] image d'une session abandonnee - ignoree\n");
            img_id_verdict = 0;
            img_id_qr = 0;
            qr_affiche = false;
        } else {
            epd_display_update_partial();
            printf("[img] affichee\n");

            // Sans ceci, la dalle restait sur le verdict jusqu'a la prochaine touche : le
            // comptoir se retrouvait devant un ecran qui n'indique plus quoi appuyer.
            if (img_id_verdict != 0 && affichee == img_id_verdict) {
                img_id_verdict = 0;
                qr_affiche = false;
                armer_retour_menu(VERDICT_AFFICHAGE_MS);
            } else if (img_id_qr != 0 && affichee == img_id_qr) {
                img_id_qr = 0;
                qr_affiche = true;
            }
        }
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
        armer_retour_menu(ERREUR_AFFICHAGE_MS);
    }

    // Adopter une configuration change le menu : il faut le redessiner. Mais pas tant qu'un
    // ecran transitoire ou un QR occupe la dalle - dans ces deux cas le retour au menu est
    // deja programme, et il affichera la nouvelle liste.
    if (cfg_a_redessiner && !qr_affiche && retour_menu_ms == 0) {
        cfg_a_redessiner = false;
        if (ui_restore_cb != NULL) {
            printf("[cfg] menu redessine\n");
            ui_restore_cb();
        }
    }

    // Rendre la main a l'interface : le message a ete lu, on reaffiche le menu pour que
    // l'operateur voie de nouveau quoi appuyer.
    if (retour_menu_ms != 0
        && to_ms_since_boot(get_absolute_time()) - retour_menu_ms > retour_menu_delai_ms) {
        retour_menu_ms = 0;
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
