#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <pico/time.h>


#include "pico/stdlib.h"
#include "wifi_setup.h"
#include "http_client.h"
#include "navigation/nav.h"
#include "screen/epd_driver.h"
#include "gpio/gpio_driver.h"
#include "storage/storage_manager.h"


#ifndef WIFI_SSID
#error "WIFI_SSID non defini : relancer cmake avec -DWIFI_SSID=... -DWIFI_PASSWORD=..."
#endif
#ifndef WIFI_PASSWORD
#error "WIFI_PASSWORD non defini : relancer cmake avec -DWIFI_SSID=... -DWIFI_PASSWORD=..."
#endif

// flags pour savoir quand faire du polling sur batterie ou wifi
static uint8_t flags_irq = 0b0;

bool periodic_check_callback(struct repeating_timer *t) {
    flags_irq = 0b1;
    return true;
}

struct repeating_timer timer;

/*=================================================================
 *  Main
 *=================================================================*/
int main(int argc, char *argv[]) {
    stdio_init_all();   // init USB
    sleep_ms(1000);     // time for picocom to print initial log
    // --- BLOC until USB connected ---

    printf("\n\n\n======== EDEL CHECK ==========\n\n");
    printf("version 1.0.6\n");
    printf("   -   wifi\n");
    printf("   -   Screen\n");
    printf("   -   POST token\n\n\n");

    // --- init LOCAL STORAGE ---
    if (!init_local_storage()) {
        return -1;
    }

    persistent_storage_t *local_storage = get_local_storage();

    printf("[storage] values\n");
    printf("%s\n", local_storage->wifi_1_ssid);
    printf("%s\n", local_storage->wifi_1_password);
    printf("%s\n", local_storage->bearer_token);

    // --- init GPIO ---
    init_gpio();

    // --- init screen ---
    epd_init();

    epd_fb_clear(true);
    epd_display_update_full();
    memcpy(epd_framebuffer, fullscreen_chargement, EPD_BUFFER_SIZE);
    epd_display_update_partial();

    // init wifi
    if (!wifi_init()) {
        printf("\nwifi_init() failed\n");
        return -1;
    }

    // TODO MOVE dans settings
    if (!wifi_connect(local_storage->wifi_1_ssid, local_storage->wifi_1_password, 30000)) {
        printf("impossible de continuer sans wifi\n");
        return -1;
    }

    sleep_ms(500);

    // --- END OF INIT PART

    display_menu(true, 4, "check", "settings", "post token", "mcquenty");
    printf("\n\n\n======== MENU ==========\n\n");
    printf("1) check\n");
    printf("2) settings\n");
    printf("\n\n\n========================\n\n");

    add_repeating_timer_ms(-5000, periodic_check_callback, NULL, &timer);
    
    while (running) {
        poll_usb_nav_key();
        sleep_ms(10);
    }

    epd_fb_clear(true);
    epd_display_update_full();

    return 0;
}
