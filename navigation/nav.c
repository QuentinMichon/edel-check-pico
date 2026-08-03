//
// Created by Quentin Michon on 24.07.2026.
//

#include "nav.h"

#include <stdio.h>
#include "pico/stdio.h"
#include "http_client.h"


static nav_page_t state = NAV_PAGE_START;

bool running = true;

/*
 *      GESTION DES BOUTON + MENU
 */
void poll_usb_nav_key(void) {
    int c = getchar_timeout_us(0);
    if (c == PICO_ERROR_TIMEOUT) {
        return;
    }

    switch (state) {
        // ========================== PAGE START =================================
        case NAV_PAGE_START:
            switch (c) {
                case '1':
                    printf("go to check: 1\n");
                    // decouverte du client https (requete sortante)
                    // extra_headers = NULL ici ; pour ajouter un entete custom, ex: "Accept: application/json\r\n"
                    http_get(HTTP_TEST_HOST,
                        HTTP_TEST_PORT,
                        HTTP_TEST_PATH,
                        HTTP_TEST_USE_TLS,
                        NULL,
                        10000
                    );

                    break;
                case '2':
                    printf("go to settings: 2\n");
                    printf("B1:back\n");
                    state = NAV_PAGE_SETTINGS;
                    break;
                case '3':
                    printf("NA: 3\n");
                    break;
                case '4':
                    printf("NA: 4\n");
                    break;
                case 'x':
                    running = false;
                    printf("EXIT\n");
                default:
                    printf("poll_usb_nav_key: NOT SUPPORTED\n");
                    break;
            }
            break;
        // ========================== PAGE SETTINGS =================================
        case NAV_PAGE_SETTINGS:
            switch (c) {
                case '1':
                    printf("go to start: 1\n");
                    printf("B1:check B2:settings\n");
                    state = NAV_PAGE_START;
                    break;
                case '2':
                    printf("NA: 2\n");
                    break;
                case '3':
                    printf("NA: 3\n");
                    break;
                case '4':
                    printf("NA: 4\n");
                    break;
                default:
                    printf("poll_usb_nav_key: NOT SUPPORTED\n");
                    break;
            }
            break;
    }
}


// switch (c) {
//     case '1':
//         printf("poll_usb_nav_key: 1\n");
//         break;
//     case '2':
//         printf("poll_usb_nav_key: 2\n");
//         break;
//     case '3':
//         printf("poll_usb_nav_key: 3\n");
//         break;
//     case '4':
//         printf("poll_usb_nav_key: 4\n");
//         break;
//     default:
//         printf("poll_usb_nav_key: NOT SUPPORTED\n");
//         break;
// }