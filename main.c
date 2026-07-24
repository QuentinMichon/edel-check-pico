#include <stdio.h>
#include <stdbool.h>


#include "pico/stdlib.h"
#include "wifi_setup.h"
#include "http_client.h"


#ifndef WIFI_SSID
#error "WIFI_SSID non defini : relancer cmake avec -DWIFI_SSID=... -DWIFI_PASSWORD=..."
#endif
#ifndef WIFI_PASSWORD
#error "WIFI_PASSWORD non defini : relancer cmake avec -DWIFI_SSID=... -DWIFI_PASSWORD=..."
#endif


// =================================================================
// cible utilisee pour la demo de requete sortante (partie 1)
// =================================================================

#define HTTP_TEST_HOST "example.com"
#define HTTP_TEST_PORT 443 // 0 => port par defaut (80 en http, 443 en https)
#define HTTP_TEST_PATH "/"
#define HTTP_TEST_USE_TLS true
// =================================================================





/*=================================================================
 *  Main
 *=================================================================*/
int main(int argc, char *argv[]) {
    stdio_init_all();   // init USB
    sleep_ms(1000);     // time for picocom to print initial log
    // --- BLOC until USB connected ---

    printf("\n\n\n======== EDEL CHECK ==========\n\n");
    printf("version 1.0.2\n");
    printf("   -   wifi\n\n");

    // init wifi
    if (!wifi_init()) {
        printf("\nwifi_init() failed\n");
        return -1;
    }

    // TODO MOVE dans settings
    if (!wifi_connect(WIFI_SSID, WIFI_PASSWORD, 30000)) {
        printf("impossible de continuer sans wifi\n");
        return -1;
    }

    // decouverte du client https (requete sortante)
    // extra_headers = NULL ici ; pour ajouter un entete custom, ex: "Accept: application/json\r\n"
    http_get(HTTP_TEST_HOST, HTTP_TEST_PORT, HTTP_TEST_PATH, HTTP_TEST_USE_TLS, NULL, 10000);

    while (true) {
        printf("ok\n");
        sleep_ms(1000);
    }

    return 0;
}
