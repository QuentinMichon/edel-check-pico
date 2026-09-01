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
#include "power/power.h"


static nav_page_t state = NAV_PAGE_MENU;

bool running = true;

// Le tout premier dessin apres la mise sous tension doit etre un full.
//
// Un rafraichissement partiel est un DIFF entre le plan 0x24 (nouvelle image) et le plan
// 0x26 (image affichee). Au demarrage, 0x26 contient encore l'ecran d'appairage ou de
// chargement, et c'est le premier diff que la dalle encaisse : l'image sort incomplete.
// Les suivants partent d'un 0x26 resynchronise en fin de partiel, donc sains.
//
// Le symptome etait exactement celui-la : le premier changement de configuration semblait
// ignore, tous les suivants passaient sans accroc. Coute 2,3 s une seule fois.
static bool premier_dessin = true;

static void go_to_menu(void) {

    printf("\n\n\n======== MENU ==========\n\n");
    printf("1) check         (%d profil(s) recu(s) du cloud)\n", profiles_count());
    printf("2) settings\n");
    printf("x) exit\n");
    printf("\n\n\n========================\n\n");

    display_menu(premier_dessin, 2, "check", "settings");
    premier_dessin = false;

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

// Page d'information, en lecture seule. Elle affichait auparavant WIFI et PAIRING, deux
// entrees dont aucune touche ne faisait quoi que ce soit. Montrer ce que le boitier sait
// de lui-meme est plus utile qu'un menu qui ne repond pas.
//
// display_menu ne convient pas ici : sa police ne connait que A-Z, or il faut des chiffres.
// D'ou epd_text, qui trace une matrice 5x7 complete.
static void go_to_settings(void) {

    persistent_storage_t *cfg = get_local_storage();

    printf("\n\n\n====== INFOS ===========\n\n");
    printf("boitier  %s\n", cfg->device_id[0] ? cfg->device_id : "non appaire");
    printf("broker   %s:%u\n", cfg->broker_host, cfg->broker_port);
    printf("wifi     %s\n", cfg->wifi_1_ssid);
    printf("batterie %d%% (%.2f V, %s)\n", power_niveau_pourcent(), power_vsys_volts(),
           power_sur_usb() ? "usb" : "batterie");
    printf("4) BACK TO MENU\n");
    printf("\n\n\n========================\n\n");

    static char ligne[64];

    epd_fb_clear(true);
    epd_fb_write_big_centered(20, "INFOS", 3);

    snprintf(ligne, sizeof(ligne), "ID %s", cfg->device_id[0] ? cfg->device_id : "NON APPAIRE");
    epd_fb_write_big(20, 80, ligne, 2);

    snprintf(ligne, sizeof(ligne), "BROKER %s", cfg->broker_host);
    epd_fb_write_big(20, 120, ligne, 2);

    snprintf(ligne, sizeof(ligne), "WIFI %s", cfg->wifi_1_ssid);
    epd_fb_write_big(20, 160, ligne, 2);

    snprintf(ligne, sizeof(ligne), "BATTERIE %d%% %s", power_niveau_pourcent(),
             power_sur_usb() ? "USB" : "");
    epd_fb_write_big(20, 200, ligne, 2);

    epd_fb_write_big(20, 260, "4 RETOUR", 2);
    epd_display_update_partial();

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
                    // Le boitier publie "le profil N a ete demande" et rend la main.
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