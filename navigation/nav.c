//
// Created by Quentin Michon on 24.07.2026.
//

#include "nav.h"

#include <stdio.h>
#include "screen/epd_driver.h"
#include "pico/stdio.h"
#include "http_client.h"
#include "assets/full_screen/check_fullscreen.h"
#include "json/json_util.h"
#include "storage/storage_manager.h"


static nav_page_t state = NAV_PAGE_MENU;

bool running = true;

static void go_to_menu(void) {

    printf("\n\n\n======== MENU ==========\n\n");
    printf("1) check\n");
    printf("2) settings\n");
    printf("3)\n");
    printf("4)\n");
    printf("x) exit\n");
    printf("\n\n\n========================\n\n");

    display_menu(false, 2, "check", "settings");

    state = NAV_PAGE_MENU;
}

static void go_to_profile(void) {

    printf("\n\n\n====== PROFILES ========\n\n");
    printf("1) TOKEN\n");
    printf("2) QR_CH\n");
    printf("3)\n");
    printf("4) BACK TO MENU\n");
    printf("\n\n\n========================\n\n");

    display_menu(false, 4, "token", "qr ch", "", "back to menu");

    state = NAV_PAGE_PROFILE;
}

static void go_to_settings(void) {

    printf("\n\n\n====== SETTINGS ========\n\n");
    printf("1) WIFI\n");
    printf("2) PAIRING\n");
    printf("3) \n");
    printf("4) BACK TO MENU\n");
    printf("\n\n\n========================\n\n");

    display_menu(false, 4, "wifi", "pairing", "", "back to menu");

    state = NAV_PAGE_SETTINGS;
}

static void go_to_check(void) {

    printf("\n\n\n====== CHECK ===========\n\n");
    printf("1) BACK TO PROFILES\n");
    printf("2) \n");
    printf("3) \n");
    printf("4) \n");
    printf("\n\n\n========================\n\n");

    // EPD : déjà affiché dans verify_ch()

    state = NAV_PAGE_CHECK;
}

/*
 *          1) POST pour avoir le bareer token EDEL-ID
 *              POST + parse le JSON + save le token
 */
static bool post_token(void) {

    // static : la pile du core0 ne fait que 4 Ko (SCRATCH_Y) sur ce chip, ces buffers y sont
    // beaucoup trop gros. pas de probleme de reentrance ici (boucle nav mono-thread, un seul
    // appel a la fois).
    static char auth_value[256];
    static char headers[320];
    static char body[2048];

    if (!http_client_basic_auth(OAUTH_CLIENT_ID, OAUTH_CLIENT_SECRET, auth_value, sizeof(auth_value))) {
        printf("[nav] echec de construction de l'entete Basic Auth (buffer trop petit ?)\n");
        return false;
    }

    snprintf(headers, sizeof(headers), "Authorization: Basic %s\r\n", auth_value);

    bool ok = http_post(OAUTH_TOKEN_HOST,
        OAUTH_TOKEN_PORT,
        OAUTH_TOKEN_PATH,
        OAUTH_TOKEN_USE_TLS,
        "application/x-www-form-urlencoded",
        "grant_type=client_credentials&scope=openid",
        headers,
        10000,
        body,
        sizeof(body)
    );

    if (ok) {
        handle_token(body);
        return true;
    }
    return false;
}

/*
 *          2) POST verification EDEL-ID (claims), authentifie par le Bearer token stocke
 *              en local storage (voir post_token / handle_token)
 */
static void verify_ch(void) {

    // TODO boucle pour resend verify_ch si le token est invalide après avoir fait un post_token() à nouveau

    persistent_storage_t *local_storage = get_local_storage();

    if (local_storage->bearer_token[0] == '\0') {
        if (!post_token()) {
            return;
        }
    }

    // static, memes raisons que dans post_token(). body est dimensionne large : la reponse de
    // verification embarque un bitmap de QR code en JSON et pese couramment 7-8 Ko.
    static char headers[MEM_BEARER_TOKEN_SIZE + 64];
    static char body[10240];

    snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", local_storage->bearer_token);

    bool ok = http_post(VERIFY_CH_HOST,
        VERIFY_CH_PORT,
        VERIFY_CH_PATH,
        VERIFY_CH_USE_TLS,
        "application/json",
        "{\"verificationClaims\": [\"$.given_name\", \"$.family_name\", \"$.birth_date\"]}",
        headers,
        10000,
        body,
        sizeof(body)
    );

    if (ok) {
        printf("[nav] verification reponse: %s\n", body);

        epd_fb_clear(true);
        memcpy(epd_framebuffer, check_fullscreen, EPD_BUFFER_SIZE);
        print_qr_code(body, 78, 0, 3);
        epd_fb_write_typo(35, 240, "back to profiles");
        epd_display_update_full();
    }
}

/*
 *      GESTION DES BOUTON + MENU
 */
void poll_usb_nav_key(void) {
    int c = getchar_timeout_us(0);
    if (c == PICO_ERROR_TIMEOUT) {
        return;
    }

    switch (state) {
        // ========================== MENU =================================
        case NAV_PAGE_MENU:
            switch (c) {
                case '1':
                    go_to_profile();
                    break;
                case '2':
                    go_to_settings();
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
                    printf("NA: 1\n");
                    break;
                case '2':
                    printf("NA: 2\n");
                    break;
                case '3':
                    printf("NA: 3\n");
                    break;
                case '4':
                    go_to_menu();
                    break;
                default:
                    printf("poll_usb_nav_key: NOT SUPPORTED\n");
                    break;
            }
            break;

        // ========================== PAGE PROFILE =================================
        case NAV_PAGE_PROFILE:
            switch (c) {
                case '1':
                    post_token();
                    go_to_profile();
                    break;
                case '2':
                    // TODO image de chargement sur EPD
                    verify_ch();
                    go_to_check();
                    break;
                case '3':
                    printf("NA: 3\n");
                    break;
                case '4':
                    go_to_menu();
                    break;
                default:
                    printf("poll_usb_nav_key: NOT SUPPORTED\n");
                    break;
            }
            break;

        // ========================== PAGE CHECK ===================================
        case NAV_PAGE_CHECK:
            switch (c) {
                case '1':
                    go_to_profile();
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