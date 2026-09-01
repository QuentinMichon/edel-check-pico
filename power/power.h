#ifndef EDEL_CHECK_PICO_POWER_H
#define EDEL_CHECK_PICO_POWER_H

#include <stdbool.h>

// Mesure de l'alimentation du boitier.
//
// Le champ `batt` du contrat MQTT (§3) etait ecrit EN DUR a 100 : le boitier annoncait une
// batterie pleine qu'il n'avait jamais mesuree. Ici il la lit.

// Initialise l'ADC. A appeler une fois, apres cyw43_arch_init().
void power_init(void);

// Vrai si le boitier est alimente par l'USB.
//
// Sur Pico 2 W, VBUS n'est pas cable sur un GPIO du RP2350 mais sur une broche de la puce
// WiFi : impossible a lire avant cyw43_arch_init().
bool power_sur_usb(void);

// Tension de VSYS en volts, ou -1 si la lecture a echoue.
float power_vsys_volts(void);

// Niveau a publier dans `status`, en pourcentage 0-100.
//
// Sous USB, rend 100 : la question « combien reste-t-il ? » n'a pas de sens quand la source
// est illimitee. Sur batterie, interpole entre 3,0 V (vide) et 4,2 V (pleine), les bornes
// usuelles d'un accumulateur lithium a element unique.
int power_niveau_pourcent(void);

#endif
