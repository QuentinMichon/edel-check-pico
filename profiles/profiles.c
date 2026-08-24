#include "profiles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frozen.h"

static device_profile_t g_profiles[PROFILES_MAX];
static int g_count;
static int g_version = -1;

int profiles_count(void) {
    return g_count;
}

const device_profile_t *profiles_get(int index) {
    if (index < 0 || index >= g_count) {
        return NULL;
    }
    return &g_profiles[index];
}

int profiles_version(void) {
    return g_version;
}

void profiles_clear(void) {
    memset(g_profiles, 0, sizeof(g_profiles));
    g_count = 0;
    g_version = -1;
}

// Tri par `order`. Le contrat le dit contigu et croissant, mais l'ordre d'arrivee dans le
// tableau JSON n'est pas garanti par le contrat - et c'est cet ordre qui decide quelle
// touche declenche quel profil. Un tri de 8 elements ne coute rien, se tromper de profil
// coute une verification faite avec la mauvaise finalite.
static void trier(void) {
    for (int i = 1; i < g_count; i++) {
        device_profile_t courant = g_profiles[i];
        int j = i - 1;
        while (j >= 0 && g_profiles[j].order > courant.order) {
            g_profiles[j + 1] = g_profiles[j];
            j--;
        }
        g_profiles[j + 1] = courant;
    }
}

bool profiles_handle_cfg(const char *json, size_t len) {
    if (json == NULL || len == 0) {
        // Charge utile vide sur un topic retenu : le message conserve a ete efface. C'est
        // ce que fait le serveur a la revocation - le boitier doit donc tout oublier, et
        // non garder sa derniere configuration connue.
        printf("[cfg] message retenu efface - configuration oubliee\n");
        profiles_clear();
        return true;
    }

    int version = -1;
    if (json_scanf(json, (int) len, "{version: %d}", &version) != 1) {
        printf("[cfg] version absente - message ignore\n");
        return false;
    }

    // Le serveur republie la configuration a chaque connexion, ce qui donne au systeme sa
    // propriete de convergence. Sans ce controle, chaque reconnexion redessinerait le menu
    // pour rien.
    if (g_version >= 0 && version <= g_version) {
        return false;
    }

    int nouveau_count = 0;
    struct json_token profil;

    for (int i = 0; i < PROFILES_MAX; i++) {
        if (json_scanf_array_elem(json, (int) len, ".profiles", i, &profil) < 0) {
            break;
        }

        char *id = NULL, *label = NULL;
        int ordre = i, binding = 0;

        json_scanf(profil.ptr, profil.len,
                   "{id: %Q, order: %d, label: %Q, holder_binding: %B}",
                   &id, &ordre, &label, &binding);

        if (id != NULL) {
            device_profile_t *p = &g_profiles[nouveau_count++];
            memset(p, 0, sizeof(*p));
            snprintf(p->id, sizeof(p->id), "%s", id);
            snprintf(p->label, sizeof(p->label), "%s", label ? label : "SANS LIBELLE");
            p->order = ordre;
            p->holder_binding = binding != 0;
        }

        // json_scanf alloue les chaines %Q : ne pas les liberer fuirait un peu de tas a
        // chaque republication de configuration, et le tas de ce chip fait 520 Ko en tout.
        free(id);
        free(label);
    }

    // Au-dela de PROFILES_MAX, le boitier n'a de toute facon que quatre boutons. On le dit
    // plutot que de tronquer en silence.
    struct json_token surplus;
    if (json_scanf_array_elem(json, (int) len, ".profiles", PROFILES_MAX, &surplus) >= 0) {
        printf("[cfg] plus de %d profils recus - les suivants sont ignores\n", PROFILES_MAX);
    }

    g_count = nouveau_count;
    g_version = version;
    trier();

    printf("[cfg] version %d adoptee - %d profil(s)\n", g_version, g_count);
    for (int i = 0; i < g_count; i++) {
        printf("      %d) %s%s\n", i + 1, g_profiles[i].label,
               g_profiles[i].holder_binding ? "  (avec portrait)" : "");
    }
    return true;
}
