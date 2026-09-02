//
// Les quatre boutons poussoirs, cables entre la broche et la masse. La tirette interne
// maintient la broche haute au repos, un appui la fait tomber.
//
// Lus dans la boucle principale, pas par interruption. Une interruption sur ces broches
// empeche l'ecran de repondre : BUSY ne monte plus jamais, le rafraichissement echoue en
// silence et la dalle garde son image precedente. Mesure par A/B sur le materiel, en
// desarmant les seules interruptions, tout le reste identique.
//
// La boucle principale passe ici toutes les 10 ms, largement sous la duree d'un appui. Le
// seul angle mort est le rafraichissement de l'ecran, pendant lequel rien n'est lu.
//

#include "boutons.h"

#include "hardware/gpio.h"
#include "pico/time.h"

// Ordre physique des boutons, de gauche a droite. Ce n'est pas l'ordre des GPIO.
static const uint broches[4] = {9, 11, 10, 8};

// Un contact mecanique rebondit pendant quelques millisecondes. Le delai est commun aux
// quatre boutons : dans un menu, deux appuis a moins de 100 ms sont un rebond, pas une
// intention.
#define ANTI_REBOND_US 100000

static bool etait_presse[4];
static uint32_t dernier_appui_us = 0;

void boutons_init(void) {
    for (int i = 0; i < 4; i++) {
        gpio_init(broches[i]);
        gpio_set_dir(broches[i], GPIO_IN);
        gpio_pull_up(broches[i]);
    }
}

char boutons_lire(void) {
    uint32_t maintenant = time_us_32();
    char touche = 0;

    for (int i = 0; i < 4; i++) {
        bool presse = !gpio_get(broches[i]);

        // Le front seul compte : un bouton maintenu ne doit pas repeter.
        if (presse && !etait_presse[i] && maintenant - dernier_appui_us > ANTI_REBOND_US) {
            dernier_appui_us = maintenant;
            touche = (char) ('1' + i);
        }
        etait_presse[i] = presse;
    }

    return touche;
}
