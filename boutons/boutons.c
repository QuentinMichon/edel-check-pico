//
// Les quatre boutons poussoirs, cables entre la broche et la masse. La tirette interne
// maintient la broche haute au repos, un appui la fait tomber : d'ou le front descendant.
//
// L'interruption ne fait que deposer un caractere, jamais dessiner. Un rafraichissement
// d'ecran dure jusqu'a 2,3 s et pilote le SPI ; le faire dans une interruption bloquerait
// aussi le fil de lwIP et ferait expirer le keep-alive MQTT. C'est la meme regle que pour
// les rappels du client MQTT : lever un drapeau, laisser la boucle principale agir.
//

#include "boutons.h"

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "pico/time.h"

// Ordre physique des boutons, de gauche a droite. Ce n'est pas l'ordre des GPIO.
static const uint broches[4] = {9, 11, 10, 8};

// Un contact mecanique rebondit pendant quelques millisecondes. Le delai est commun aux
// quatre boutons : dans un menu, deux appuis a moins de 100 ms sont un rebond, pas une
// intention.
#define ANTI_REBOND_US 100000

static volatile uint32_t dernier_appui_us = 0;
static volatile char en_attente = 0;

static void sur_front(uint gpio, uint32_t evenements) {
    if (!(evenements & GPIO_IRQ_EDGE_FALL)) {
        return;
    }

    uint32_t maintenant = time_us_32();
    if (maintenant - dernier_appui_us < ANTI_REBOND_US) {
        return;
    }
    dernier_appui_us = maintenant;

    for (int i = 0; i < 4; i++) {
        if (broches[i] == gpio) {
            en_attente = (char) ('1' + i);
            return;
        }
    }
}

void boutons_init(void) {
    for (int i = 0; i < 4; i++) {
        gpio_init(broches[i]);
        gpio_set_dir(broches[i], GPIO_IN);
        gpio_pull_up(broches[i]);
        gpio_set_irq_enabled(broches[i], GPIO_IRQ_EDGE_FALL, true);
    }

    gpio_set_irq_callback(sur_front);
    irq_set_enabled(IO_IRQ_BANK0, true);
}

char boutons_lire(void) {
    uint32_t etat = save_and_disable_interrupts();
    char c = en_attente;
    en_attente = 0;
    restore_interrupts(etat);
    return c;
}
