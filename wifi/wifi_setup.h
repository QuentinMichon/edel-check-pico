#ifndef WIFI_SETUP_H
#define WIFI_SETUP_H

#include <stdint.h>
#include <stdbool.h>

// init du chip wifi cyw_43
bool wifi_init(void);

// se connecte au reseau local en mode station.
// bloque jusqu'a connexion ou expiration de timeout_ms. retourne false en cas d'echec.
bool wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms);

#endif
