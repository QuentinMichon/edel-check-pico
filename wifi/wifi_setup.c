#include <stdio.h>

#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

#include "pico/time.h"

#include "wifi_setup.h"

// Une adresse seule ne prouve rien : sans passerelle, la pile n'a aucune route et toute
// connexion sortante echoue en ERR_RTE. Les deux doivent etre posees.
static bool adresse_utilisable(void) {
    const ip4_addr_t *ip = netif_ip4_addr(netif_default);
    const ip4_addr_t *gw = netif_ip4_gw(netif_default);
    return ip && gw && !ip4_addr_isany_val(*ip) && !ip4_addr_isany_val(*gw);
}

bool wifi_init(void) {

    if (cyw43_arch_init()) {
        printf("[wifi] echec de l'initialisation du chip cyw43\n");
        return false;
    }

    // MODE station
    cyw43_arch_enable_sta_mode();

    printf("[wifi] chip cyw43 init en mode station : ok\n");

    return true;
}

bool wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms) {

    printf("[wifi] connexion a \"%s\"...\n", ssid);
    int err = cyw43_arch_wifi_connect_timeout_ms(ssid, password, CYW43_AUTH_WPA2_AES_PSK, timeout_ms);
    if (err) {
        printf("[wifi] echec de connexion (code %d)\n", err);
        return false;
    }

    // L'association ne suffit pas. Un reseau peut accepter le boitier puis ne jamais
    // repondre au DHCP : lwIP garde alors une adresse inutilisable, la connexion au
    // courtier echoue en boucle sur ERR_RTE, et comme l'association a reussi le boitier
    // n'ouvre jamais son portail. Il reste bloque sans que rien ne le dise.
    // Constate le 03.09.2026, une heure perdue.
    for (int i = 0; i < 100 && !adresse_utilisable(); i++) {
        sleep_ms(100);
    }

    if (!adresse_utilisable()) {
        printf("[wifi] associe a \"%s\" mais aucune adresse utilisable, DHCP muet\n", ssid);
        return false;
    }

    printf("[wifi] connected, ip = %s\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));
    return true;
}
