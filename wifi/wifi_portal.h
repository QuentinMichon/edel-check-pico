#ifndef WIFI_PORTAL_H
#define WIFI_PORTAL_H

#include <stdbool.h>
#include <stdint.h>

// Ouvre un point d acces et sert une page de configuration jusqu a ce que quelqu un
// soumette un reseau. Bloquant.
//
// Au retour true, ssid et password ont ete ecrits en flash et le boitier peut se
// reconnecter. Au retour false, le delai est ecoule sans que personne n ait rien saisi.
//
// Le nom du point d acces est derive de l identifiant materiel, pour que deux boitiers
// cote a cote ne se confondent pas.
bool wifi_portal_run(const char *ap_ssid, const char *ap_password, uint32_t timeout_ms);

#endif
