//
// Created by Quentin Michon on 24.07.2026.
//

#include "nav.h"

#include <stdio.h>
#include "screen/epd_driver.h"
#include "pico/stdio.h"
#include "storage/storage_manager.h"
#include "profiles/profiles.h"
#include "session/session.h"
#include "screen/epd_text.h"


static nav_page_t state = NAV_PAGE_MENU;

bool running = true;

static void go_to_menu(void) {

    printf("\n\n\n======== MENU ==========\n\n");
    printf("1) check         (%d profil(s) recu(s) du cloud)\n", profiles_count());
    printf("2) settings\n");
    printf("x) exit\n");
    printf("\n\n\n========================\n\n");

    display_menu(false, 2, "check", "settings");

    state = NAV_PAGE_MENU;
}

/*
 *      La page qui compte : les profils envoyes par le cloud sur dev/{id}/cfg.
 *
 *      Une touche = un profil = une session de verification. Le boitier ne sait pas ce que
 *      chaque profil verifie, seulement quoi afficher et quel identifiant renvoyer.
 */
static void go_to_verify(void) {

    int n = profiles_count();

    printf("\n\n\n====== VERIFIER ========\n\n");
    if (n == 0) {
        printf("aucun profil assigne a ce boitier\n");
        printf("-> en assigner un depuis le portail operateur\n");
    }
    for (int i = 0; i < n && i < 3; i++) {
        printf("%d) %s\n", i + 1, profiles_get(i)->label);
    }
    printf("4) BACK TO MENU\n");
    printf("\n\n\n========================\n\n");

    // Les libelles viennent du portail et contiennent des accents : la police de l'ecran
    // ne connait que A-Z, il faut les replier avant de dessiner.
    static char l1[32], l2[32], l3[32];
    epd_text_fold_ascii(n > 0 ? profiles_get(0)->label : "AUCUN PROFIL", l1, sizeof(l1));
    epd_text_fold_ascii(n > 1 ? profiles_get(1)->label : "", l2, sizeof(l2));
    epd_text_fold_ascii(n > 2 ? profiles_get(2)->label : "", l3, sizeof(l3));

    display_menu(false, 4, l1, l2, l3, "back to menu");

    state = NAV_PAGE_VERIFY;
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

void nav_redraw(void) {
    switch (state) {
        case NAV_PAGE_VERIFY:   go_to_verify();   break;
        case NAV_PAGE_SETTINGS: go_to_settings(); break;
        default:                go_to_menu();     break;
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
                    go_to_verify();
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

        // ========================== PAGE VERIFIER ================================
        case NAV_PAGE_VERIFY:
            switch (c) {
                case '1':
                case '2':
                case '3':
                    // Le boitier publie " le profil N a ete demande " et rend la main.
                    // Tout le reste - requete de presentation, rendu du QR, verdict -
                    // se passe dans device-service, et redescend en images.
                    session_open(c - '1');
                    break;
                case '4':
                    go_to_menu();
                    break;
                default:
                    printf("poll_usb_nav_key: NOT SUPPORTED\n");
                    break;
            }
            break;

    }
}