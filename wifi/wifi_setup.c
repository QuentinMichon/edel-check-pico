#include <stdio.h>

#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

#include "wifi_setup.h"

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

    printf("[wifi] connected, ip = %s\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));
    return true;
}
