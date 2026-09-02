#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <pico/time.h>


#include "pico/stdlib.h"
#include "wifi_setup.h"
#include "http_client.h"
#include "navigation/nav.h"
#include "screen/epd_driver.h"
#include "screen/epd_text.h"

// Le point d acces est protege : sans mot de passe, n importe qui a portee pourrait
// reconfigurer le reseau du boitier. Il est imprime sur la notice du materiel.
#include "pico/unique_id.h"
#include "wifi/wifi_portal.h"
#include "boutons/boutons.h"

// Le point d acces est protege : sans mot de passe, n importe qui a portee pourrait
// reconfigurer le reseau du boitier. Il est imprime sur la notice du materiel.
#define PORTAIL_MOT_DE_PASSE "edelcheck"
#include "gpio/gpio_driver.h"
#include "storage/storage_manager.h"
#include "enrollment/enrollment.h"
#include "mqtt/mqtt_client.h"
#include "power/power.h"


#ifndef WIFI_SSID
#error "WIFI_SSID non defini : relancer cmake avec -DWIFI_SSID=... -DWIFI_PASSWORD=..."
#endif
#ifndef WIFI_PASSWORD
#error "WIFI_PASSWORD non defini : relancer cmake avec -DWIFI_SSID=... -DWIFI_PASSWORD=..."
#endif

// flags pour savoir quand faire du polling sur batterie ou wifi
static uint8_t flags_irq = 0b0;

// L'ecran d'appairage.
//
// C'est le seul moment de la vie du boitier ou quelqu'un doit LIRE quelque chose dessus et
// le recopier ailleurs. Le code est donc trace en grand - a l'echelle 7, un glyphe fait
// 35x49 pixels, lisible a un metre d'un comptoir - et rien d'autre ne se dispute
// l'attention.
//
// Les glyphes viennent de epd_text : epd_fb_write_typo ne saurait pas dessiner les
// chiffres, et l'alphabet des codes en contient (ABCDEFGHJKLMNPQRSTUVWXYZ23456789).
static void afficher_code_appairage(const char *code, int poll_interval_s) {
    epd_fb_clear(true);

    epd_fb_write_big_centered(38, "CODE D APPAIRAGE", 3);

    // Un cadre autour du code : il separe ce qu'il faut recopier du reste de l'ecran.
    epd_fb_fill_rect(20, 108, EPD_WIDTH - 40, 4, false);
    epd_fb_fill_rect(20, 196, EPD_WIDTH - 40, 4, false);

    epd_fb_write_big_centered(130, code, 7);

    epd_fb_write_big_centered(232, "A SAISIR DANS LE PORTAIL", 2);
    epd_fb_write_big_centered(258, "EXPIRE DANS 10 MINUTES", 2);

    epd_display_update_full();

    printf("\n[appairage] code affiche a l'ecran : %s\n", code);
    printf("            interrogation du serveur toutes les %d s\n\n", poll_interval_s);
}

// Une fois l'identite obtenue, l'ecran doit cesser d'afficher un code que plus personne ne
// doit saisir - sinon un passant le recopierait dans son propre portail.
static void afficher_appaire(void) {
    epd_fb_clear(true);
    epd_fb_write_big_centered(120, "BOITIER APPAIRE", 4);
    epd_fb_write_big_centered(180, "CONNEXION AU SERVEUR", 2);
    epd_display_update_full();
}

bool periodic_check_callback(struct repeating_timer *t) {
    flags_irq = 0b1;
    return true;
}

struct repeating_timer timer;

/*=================================================================
 *  Main
 *=================================================================*/
// Ecran d attente quand le reseau manque. Avec ap != NULL, il indique aussi comment
// reconfigurer le boitier depuis un telephone.
static void afficher_pas_de_reseau(const char *ssid, const char *ap) {
    static char ligne[64];

    epd_fb_clear(true);
    epd_fb_write_big_centered(60, "PAS DE RESEAU", 3);
    snprintf(ligne, sizeof(ligne), "%s", ssid && ssid[0] ? ssid : "AUCUN CONFIGURE");
    epd_fb_write_big_centered(110, ligne, 2);

    if (ap) {
        epd_fb_write_big_centered(170, "CONNECTEZ VOUS AU RESEAU", 2);
        epd_fb_write_big_centered(205, ap, 2);
        epd_fb_write_big_centered(245, "PUIS 192.168.4.1", 2);
    } else {
        epd_fb_write_big_centered(200, "NOUVELLE TENTATIVE", 2);
    }
    epd_display_update_partial();
}

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

    // Le mot de passe Wi-Fi et le jeton ne sont plus imprimes : la console serie est
    // lisible par quiconque branche un cable sur un boitier pose sur un comptoir.
    printf("[storage] reseau configure : %s\n", local_storage->wifi_1_ssid);

    // --- init GPIO ---
    init_gpio();
    boutons_init();
    // Les boutons restent lus pendant les 638 ms a 2,3 s d'un rafraichissement.
    epd_set_attente_cb(boutons_scruter);

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

    // Les identifiants wifi viennent de la flash. Ils y sont semes a la compilation au tout
    // premier demarrage, puis remplaces par ce que le portail captif enregistre.
    //
    // On reessaie au lieu de rendre la main. Un return ici terminait main() : l'USB restait
    // enumere mais plus rien ne tournait, et seule une coupure d'alimentation relancait le
    // boitier. Sur un comptoir, un boitier allume avant le routeur restait eteint pour la
    // journee, sans que l'ecran ne dise pourquoi.
    // Le reseau enregistre passe TOUJOURS en premier. Le portail n est qu un recours :
    // tant que le boitier sait se connecter, rien ne change pour lui.
    //
    // Deux essais avant d ouvrir le point d acces, pas un seul : un routeur qui redemarre
    // en meme temps que le boitier ne doit pas declencher une reconfiguration.
    for (int essai = 1; !wifi_connect(local_storage->wifi_1_ssid,
                                      local_storage->wifi_1_password, 30000); essai++) {
        printf("[wifi] echec (essai %d)\n", essai);

        if (essai < 2) {
            afficher_pas_de_reseau(local_storage->wifi_1_ssid, NULL);
            sleep_ms(5000);
            continue;
        }

        // Nom du point d acces derive de l identifiant materiel : deux boitiers cote a
        // cote ne doivent pas porter le meme.
        char board[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        pico_get_unique_board_id_string(board, sizeof(board));
        static char ap[32];
        snprintf(ap, sizeof(ap), "EdelCheck-%s", board + strlen(board) - 4);

        afficher_pas_de_reseau(local_storage->wifi_1_ssid, ap);
        if (wifi_portal_run(ap, PORTAIL_MOT_DE_PASSE, 300000)) {
            local_storage = get_local_storage();
            printf("[wifi] nouveau reseau configure, tentative de connexion\n");
        }
    }

    sleep_ms(500);

    // La mesure d'alimentation vient APRES l'initialisation de la puce WiFi : VBUS est cable
    // sur une de ses broches et non sur un GPIO du RP2350, et VSYS partage la sienne avec
    // elle. Avant cet appel, les deux lectures seraient fausses.
    power_init();
    printf("[power] %s, VSYS %.2f V, niveau %d%%\n",
           power_sur_usb() ? "sur USB" : "sur batterie",
           power_vsys_volts(), power_niveau_pourcent());

    // L'interface reprend la main apres un ecran d'erreur transitoire.
    edel_mqtt_set_ui_restore_cb(nav_redraw);

    // --- appairage -------------------------------------------------------------
    //
    // Ne fait rien si une identite valide est deja en flash, ce qui est le cas nominal a
    // chaque demarrage. Sinon le boitier demande un code, l'affiche, et interroge le
    // serveur jusqu'a ce qu'un operateur l'ait saisi dans son portail.
    switch (enrollment_run(afficher_code_appairage)) {
        case ENROLLMENT_REUSSI:
            afficher_appaire();
            edel_mqtt_start();
            break;
        case ENROLLMENT_DEJA_APPAIRE:
            edel_mqtt_start();
            break;
        case ENROLLMENT_ECHEC:
            // Pas de mode degrade : sans identite, le boitier ne peut rien verifier. On le
            // dit a l'ecran plutot que de laisser une dalle figee sur l'ecran de chargement.
            printf("[main] appairage impossible - le boitier reste inutilisable\n");
            epd_fb_clear(true);
            epd_fb_write_big_centered(130, "APPAIRAGE IMPOSSIBLE", 3);
            epd_fb_write_big_centered(180, "REDEMARRER LE BOITIER", 2);
            epd_display_update_full();
            break;
    }

    // --- END OF INIT PART

    // Le menu est dessine par nav_redraw() et par personne d'autre.
    //
    // Il etait auparavant ecrit une SECONDE fois ici, en dur, avec quatre entrees dont
    // deux qui n'existaient plus dans nav.c. L'ecran affichait donc un menu different de
    // celui auquel les touches repondaient - et supprimer une entree dans nav.c laissait
    // celle de main.c intacte.
    nav_redraw();

    add_repeating_timer_ms(-5000, periodic_check_callback, NULL, &timer);
    
    while (running) {
        nav_poll_input();
        edel_mqtt_poll();   // reconnecte le broker si la connexion est tombee
        sleep_ms(10);
    }

    epd_fb_clear(true);
    epd_display_update_full();

    return 0;
}
