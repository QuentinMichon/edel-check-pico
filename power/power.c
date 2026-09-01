#include "power.h"

#include "hardware/adc.h"
#include "pico/cyw43_arch.h"

// VSYS arrive sur GPIO29 a travers un pont diviseur par 3. Les deux constantes viennent du
// fichier de carte du SDK (boards/pico2_w.h) plutot que d'etre recopiees a la main.
#define VSYS_ADC_CANAL   (PICO_VSYS_PIN - 26)
#define DIVISEUR         3.0f
#define PLEINE_ECHELLE   (3.3f / (1 << 12))

// Trois lectures moyennees. L'ADC du RP2350 est bruite de quelques millivolts ; une lecture
// unique fait sauter l'affichage d'un pourcent a l'autre sans que rien n'ait bouge.
#define ECHANTILLONS 3

#define VIDE_V   3.0f
#define PLEINE_V 4.2f

static bool pret;

void power_init(void) {
    // Seulement le bloc ADC ici. La broche, elle, est reconfiguree A CHAQUE LECTURE : voir
    // power_vsys_volts().
    adc_init();
    pret = true;
}

bool power_sur_usb(void) {
    // Broche de la puce WiFi, pas du RP2350 : cyw43_arch_init() doit avoir eu lieu.
    return cyw43_arch_gpio_get(CYW43_WL_GPIO_VBUS_PIN);
}

float power_vsys_volts(void) {
    if (!pret) {
        return -1.0f;
    }

    // La puce WiFi PARTAGE cette broche (CYW43_USES_VSYS_PIN dans le fichier de carte).
    // Lire sans prendre son verrou revient a la lui arracher au milieu d'un echange SPI :
    // le symptome serait une deconnexion reseau apparemment sans cause, quelques secondes
    // apres une lecture de batterie.
    cyw43_thread_enter();

    // Reveiller la puce avant de lui prendre la broche. Sans cet acces prealable, elle peut
    // etre en veille et la laisser dans un etat indefini.
    (void) cyw43_arch_gpio_get(CYW43_WL_GPIO_VBUS_PIN);

    // adc_gpio_init A CHAQUE LECTURE, et non une fois pour toutes au demarrage.
    //
    // C'est le piege de cette broche : la puce WiFi la RECONFIGURE pour son propre usage
    // entre deux lectures. L'initialiser une seule fois donnait une mesure de 0,02 V au lieu
    // de 5 V - mesure sur la carte, pas deduite. Le pilote avait repris la main entre-temps.
    adc_gpio_init(PICO_VSYS_PIN);
    adc_select_input(VSYS_ADC_CANAL);

    // La premiere conversion apres un changement d'entree porte encore la charge de la
    // precedente. On la jette.
    (void) adc_read();

    uint32_t somme = 0;
    for (int i = 0; i < ECHANTILLONS; i++) {
        somme += adc_read();
    }

    cyw43_thread_exit();

    return ((float) somme / ECHANTILLONS) * PLEINE_ECHELLE * DIVISEUR;
}

int power_niveau_pourcent(void) {
    if (power_sur_usb()) {
        return 100;
    }

    float v = power_vsys_volts();
    if (v < 0.0f) {
        return 0;
    }

    float part = (v - VIDE_V) / (PLEINE_V - VIDE_V);
    if (part < 0.0f) part = 0.0f;
    if (part > 1.0f) part = 1.0f;
    return (int) (part * 100.0f + 0.5f);
}
