#include <stdio.h>
#include <stdbool.h>
#include <string.h>


#include "pico/stdlib.h"
#include "wifi_setup.h"
#include "http_client.h"
#include "navigation/nav.h"
#include "screen/epd_driver.h"
#include "gpio/gpio_driver.h"


#ifndef WIFI_SSID
#error "WIFI_SSID non defini : relancer cmake avec -DWIFI_SSID=... -DWIFI_PASSWORD=..."
#endif
#ifndef WIFI_PASSWORD
#error "WIFI_PASSWORD non defini : relancer cmake avec -DWIFI_SSID=... -DWIFI_PASSWORD=..."
#endif



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
    printf("   -   POST token\n");

    // init GPIO
    init_gpio();

    // init screen
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
    if (!wifi_connect(WIFI_SSID, WIFI_PASSWORD, 30000)) {
        printf("impossible de continuer sans wifi\n");
        return -1;
    }

    sleep_ms(500);

    memcpy(epd_framebuffer, fullscreen_base, EPD_BUFFER_SIZE);
    epd_fb_draw_image(342, 270, epd_image_bat_80, EPD_IMAGE_BAT_W, EPD_IMAGE_BAT_H);
    epd_fb_write_typo(10, 270, "edel id");
    epd_fb_write_typo(60, 45, "check");
    epd_fb_write_typo(60, 97, "settings");

    epd_display_update_full();

    while (running) {
        poll_usb_nav_key();
        sleep_ms(10);
    }

    epd_fb_clear(false);
    epd_display_update_full();

    return 0;
}
