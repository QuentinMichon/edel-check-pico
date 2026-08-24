#include "session.h"

#include <stdio.h>

#include "pico/stdlib.h"

#include "mqtt/mqtt_client.h"
#include "profiles/profiles.h"

static char req_id[16];
static uint32_t compteur;

const char *session_last_req_id(void) {
    return req_id;
}

bool session_open(int profile_index) {
    const device_profile_t *p = profiles_get(profile_index);
    if (p == NULL) {
        printf("[session] profil %d inconnu - le boitier n'a que %d profil(s)\n",
               profile_index + 1, profiles_count());
        return false;
    }

    if (!edel_mqtt_is_connected()) {
        // Pas de file d'attente : une session ouverte hors ligne serait servie plus tard,
        // devant quelqu'un d'autre. Mieux vaut ne rien faire et le dire.
        printf("[session] hors ligne - session non ouverte\n");
        return false;
    }

    // Un compteur suffit, combine a l'instant du demarrage : le req_id ne sert qu'a
    // apparier session_open et session_ready sur une meme connexion, il n'a besoin d'etre
    // ni imprevisible ni unique dans le temps. Ensuite c'est le session_id du serveur qui
    // prend le relais, le resultat pouvant arriver bien plus tard.
    snprintf(req_id, sizeof(req_id), "%04lx%03lx",
             (unsigned long) (to_ms_since_boot(get_absolute_time()) & 0xFFFF),
             (unsigned long) (++compteur & 0xFFF));

    char json[128];
    snprintf(json, sizeof(json),
             "{\"type\":\"session_open\",\"req_id\":\"%s\",\"profile_id\":\"%s\"}",
             req_id, p->id);

    if (!edel_mqtt_publish_evt(json)) {
        printf("[session] publication refusee\n");
        return false;
    }

    printf("[session] ouverte - req_id=%s profil=%s\n", req_id, p->label);
    printf("           en attente du QR rendu par le cloud...\n");
    return true;
}
