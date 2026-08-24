#include "enrollment.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/unique_id.h"

#include "http_client.h"
#include "frozen.h"
#include "storage/storage_manager.h"

#ifndef EDEL_API_HOST
#error "EDEL_API_HOST non defini : relancer cmake avec -DEDEL_API_HOST=..."
#endif
#ifndef EDEL_API_PORT
#define EDEL_API_PORT 443
#endif
#ifndef EDEL_API_USE_TLS
#define EDEL_API_USE_TLS true
#endif

#define HTTP_TIMEOUT_MS 15000
#define POLL_DEFAUT_S 3

// Le code expire en 10 minutes cote serveur. On s'arrete un peu avant pour redemander un
// code propre plutot que de se faire refuser.
#define POLL_MAX_TOURS (9 * 60 / POLL_DEFAUT_S)

// Les gros tampons sont statiques : la pile du core0 fait 4 Ko.
static char hw_id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
static char pairing_code[16];
static char claim_ticket[96];
static char corps_requete[256];
static char corps_reponse[4096];

// Le contrat MQTT §8 decrit ces corps en snake_case. C'EST FAUX, et c'est mesure :
// l'API repond 400 sur `hw_id` et 200 sur `hwId`. La regle reelle, a redire a chaque
// relecture du contrat : HTTP en camelCase, MQTT en snake_case.
static bool demander_code(int *poll_interval_s) {
    snprintf(corps_requete, sizeof(corps_requete), "{\"hwId\":\"%s\"}", hw_id);
    corps_reponse[0] = '\0';

    if (!http_post(EDEL_API_HOST, EDEL_API_PORT, "/provisioning/claim", EDEL_API_USE_TLS,
                   "application/json", corps_requete, "Accept: application/json\r\n",
                   HTTP_TIMEOUT_MS, corps_reponse, sizeof(corps_reponse))) {
        printf("[appairage] requete de claim en echec\n");
        return false;
    }

    int statut = http_client_last_status();
    if (statut != 200) {
        printf("[appairage] claim refuse (statut %d)\n", statut);
        return false;
    }

    char *code = NULL, *ticket = NULL;
    int interval = 0;
    json_scanf(corps_reponse, strlen(corps_reponse),
               "{pairingCode: %Q, claimTicket: %Q, pollIntervalS: %d}",
               &code, &ticket, &interval);

    bool ok = (code != NULL && ticket != NULL);
    if (ok) {
        snprintf(pairing_code, sizeof(pairing_code), "%s", code);
        snprintf(claim_ticket, sizeof(claim_ticket), "%s", ticket);
        *poll_interval_s = interval > 0 ? interval : POLL_DEFAUT_S;
    } else {
        printf("[appairage] reponse de claim inexploitable\n");
    }

    free(code);
    free(ticket);
    return ok;
}

typedef enum { POLL_ATTENTE, POLL_LIVRE, POLL_DEFINITIF } poll_t;

// 202 = personne n'a encore saisi le code, on continue.
// 200 = le secret est la, et il ne reviendra jamais.
// 410 = definitif : appairage expire, ou secret deja delivre a quelqu'un d'autre.
//
// Distinguer 410 d'une panne passagere est ce qui evite de marteler un appairage mort -
// et c'est pour ca que http_client expose desormais le code de statut.
static poll_t interroger(void) {
    snprintf(corps_requete, sizeof(corps_requete),
             "{\"hwId\":\"%s\",\"claimTicket\":\"%s\"}", hw_id, claim_ticket);
    corps_reponse[0] = '\0';

    if (!http_post(EDEL_API_HOST, EDEL_API_PORT, "/provisioning/poll", EDEL_API_USE_TLS,
                   "application/json", corps_requete, "Accept: application/json\r\n",
                   HTTP_TIMEOUT_MS, corps_reponse, sizeof(corps_reponse))) {
        return POLL_ATTENTE;                       // panne reseau : on retente
    }

    int statut = http_client_last_status();

    if (statut == 202) return POLL_ATTENTE;
    if (statut == 410) {
        printf("[appairage] refus definitif du serveur (410)\n");
        return POLL_DEFINITIF;
    }
    if (statut != 200) {
        printf("[appairage] statut inattendu %d, on retente\n", statut);
        return POLL_ATTENTE;
    }

    char *id = NULL, *secret = NULL, *host = NULL, *ca = NULL;
    int port = 0;
    json_scanf(corps_reponse, strlen(corps_reponse),
               "{deviceId: %Q, secret: %Q, brokerHost: %Q, brokerPort: %d, caCertPem: %Q}",
               &id, &secret, &host, &port, &ca);

    poll_t resultat = POLL_DEFINITIF;

    if (id == NULL || secret == NULL) {
        printf("[appairage] 200 sans identite exploitable - abandon\n");
    } else {
        printf("[appairage] secret recu, ecriture en flash...\n");

        // ⚠ L'ordre est impose par le contrat. Le serveur ne sert le secret qu'une fois et
        // traite un second poll reussi comme une preuve de duplication : le boitier passe
        // en SUSPECT avec alerte critique. On persiste et on RELIT avant de considerer
        // l'appairage acquis - storage_save_enrollment ne rend true qu'apres verification.
        if (storage_save_enrollment(id, secret, host, (uint16_t) port, ca)) {
            resultat = POLL_LIVRE;
        } else {
            // Le secret est perdu : le serveur ne le redonnera pas. Le boitier devra etre
            // revoque puis reappaire depuis le portail. On le dit clairement plutot que de
            // boucler sur un poll qui ne peut plus reussir.
            printf("[appairage] ECHEC D'ECRITURE - le secret est perdu.\n");
            printf("            Revoquer ce boitier dans le portail, puis reappairer.\n");
        }
    }

    free(id);
    free(secret);
    free(host);
    free(ca);
    return resultat;
}

enrollment_result_t enrollment_run(enrollment_code_cb on_code) {
    if (storage_is_enrolled()) {
        printf("[appairage] identite deja en flash - rien a faire\n");
        return ENROLLMENT_DEJA_APPAIRE;
    }

    // Grave dans la puce par le fabricant. Ce n'est PAS un secret : il ne sert que de cle
    // de correlation entre le claim et le poll.
    pico_get_unique_board_id_string(hw_id, sizeof(hw_id));
    printf("[appairage] materiel %s\n", hw_id);

    while (true) {
        int poll_interval_s = POLL_DEFAUT_S;

        while (!demander_code(&poll_interval_s)) {
            printf("[appairage] nouvelle tentative dans 10 s\n");
            sleep_ms(10000);
        }

        if (on_code != NULL) {
            on_code(pairing_code, poll_interval_s);
        }

        for (int tour = 0; tour < POLL_MAX_TOURS; tour++) {
            sleep_ms((uint32_t) poll_interval_s * 1000);

            switch (interroger()) {
                case POLL_LIVRE:
                    return ENROLLMENT_REUSSI;
                case POLL_DEFINITIF:
                    return ENROLLMENT_ECHEC;
                case POLL_ATTENTE:
                    break;
            }
        }

        // Le code a expire sans que personne ne le saisisse : on en redemande un, plutot
        // que de rester bloque sur un code que le serveur a deja oublie.
        printf("[appairage] code expire, nouvelle demande\n");
    }
}
