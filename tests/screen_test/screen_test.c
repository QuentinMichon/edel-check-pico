/*
 * screen_test - banc de test autonome de l'ecran e-ink.
 *
 * NE FAIT PARTIE D'AUCUNE FONCTIONNALITE DU PRODUIT. C'est un binaire separe,
 * construit par tests/screen_test/CMakeLists.txt, qui ne touche a rien d'autre
 * que le SPI et l'ecran :
 *
 *   - pas de Wi-Fi        -> pas besoin de -DWIFI_SSID, pas d'echec au boot
 *   - pas de flash        -> aucune ecriture dans le stockage persistant
 *   - timeout USB fini    -> la carte demarre meme sans terminal serie attache
 *                            (le firmware principal, lui, attend indefiniment)
 *
 * Objectif : prouver que le panneau repond, dans quel sens, a quelle vitesse,
 * et que les primitives de dessin font ce qu'on croit.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "gpio/gpio_driver.h"
#include "screen/epd_driver.h"

// --- Chronometrage ---------------------------------------------------------
static uint32_t chrono_start_ms;

static void chrono_start(void) {
    chrono_start_ms = to_ms_since_boot(get_absolute_time());
}

static uint32_t chrono_stop_ms(void) {
    return to_ms_since_boot(get_absolute_time()) - chrono_start_ms;
}

/*
 * Verite terrain sur la broche BUSY.
 *
 * epd_wait_busy() retourne true meme quand BUSY n'est jamais monte : un panneau
 * mort produit donc un "termine." rassurant. On lit la broche nous-memes, sans
 * rien modifier au driver.
 *
 * Au repos, BUSY doit etre BAS. Un BUSY constamment HAUT = le panneau ne rend
 * jamais la main. Un BUSY qui ne bouge jamais pendant un refresh = tres
 * probablement un probleme de cablage ou d'alimentation.
 */
static void busy_probe(const char *label) {
    printf("[test] BUSY apres %-24s = %d  (attendu : 0 au repos)\n",
           label, gpio_get(PIN_BUSY));
}

static void etape(int n, const char *titre) {
    printf("\n----------------------------------------------------------\n");
    printf("[test] ETAPE %d : %s\n", n, titre);
    printf("----------------------------------------------------------\n");
}

// Refresh complet chronometre + sonde BUSY.
static void full_refresh(const char *label) {
    chrono_start();
    epd_display_update_full();
    printf("[test] full refresh  \"%s\" : %u ms\n", label, chrono_stop_ms());
    busy_probe(label);
}

// Refresh partiel chronometre + sonde BUSY.
static void partial_refresh(const char *label) {
    chrono_start();
    epd_display_update_partial();
    printf("[test] partiel       \"%s\" : %u ms\n", label, chrono_stop_ms());
    busy_probe(label);
}

/*
 * Une passe complete du banc. Appelee en boucle par main() : la demo rejoue
 * indefiniment, sans avoir a rebrancher la carte.
 */
static void run_demo(int cycle) {
    printf("\n##########################################################\n");
    printf("[test] CYCLE %d\n", cycle);
    printf("##########################################################\n");

    // ================================================================
    etape(1, "BLANC complet - l'ecran doit devenir uniformement blanc");
    // ================================================================
    epd_fb_clear(true);
    full_refresh("blanc");
    sleep_ms(2000);

    // ================================================================
    etape(2, "NOIR complet - la preuve la plus nette que le panneau vit");
    // ================================================================
    epd_fb_clear(false);
    full_refresh("noir");
    sleep_ms(2000);

    // ================================================================
    etape(3, "MIRE - cadre, origine, damier : verifie sens et geometrie");
    // ================================================================
    epd_fb_clear(true);

    // Cadre de 3 px sur tout le pourtour : si un bord manque, la fenetre RAM
    // (commandes 0x44 / 0x45) est mal dimensionnee.
    epd_fb_fill_rect(0,   0,   EPD_WIDTH, 3,          false);  // haut
    epd_fb_fill_rect(0,   EPD_HEIGHT - 3, EPD_WIDTH, 3, false); // bas
    epd_fb_fill_rect(0,   0,   3, EPD_HEIGHT,         false);  // gauche
    epd_fb_fill_rect(EPD_WIDTH - 3, 0, 3, EPD_HEIGHT, false);  // droite

    // Repere d'origine asymetrique en (0,0) : un "L" epais. Il leve toute
    // ambiguite sur le sens de balayage et sur un eventuel miroir.
    epd_fb_fill_rect(10, 10, 60, 16, false);
    epd_fb_fill_rect(10, 10, 16, 60, false);

    // Petit carre en bas a droite : marque le pixel (399, 299).
    epd_fb_fill_rect(EPD_WIDTH - 30, EPD_HEIGHT - 30, 20, 20, false);

    // Damier 8 x 6 cases de 50 px, dans la zone centrale.
    for (int by = 1; by < 5; by++) {
        for (int bx = 1; bx < 7; bx++) {
            if ((bx + by) % 2 == 0) {
                epd_fb_fill_rect(bx * 50, by * 50, 50, 50, false);
            }
        }
    }

    full_refresh("mire");
    printf("[test] a verifier a l'oeil :\n");
    printf("       - le cadre fait bien le tour complet\n");
    printf("       - le \"L\" est en HAUT A GAUCHE (sinon : image miroir ou pivotee)\n");
    printf("       - le petit carre est en BAS A DROITE\n");
    sleep_ms(2500);

    // ================================================================
    etape(4, "TEXTE - police 16x15, A-Z et apostrophe uniquement");
    // ================================================================
    epd_fb_clear(true);
    epd_fb_write_typo(20,  30, "edel check");
    epd_fb_write_typo(20,  70, "test ecran");
    epd_fb_write_typo(20, 120, "abcdefghijklm");
    epd_fb_write_typo(20, 160, "nopqrstuvwxyz");
    epd_fb_write_typo(20, 210, "l'ecran repond");
    full_refresh("texte");
    printf("[test] rappel : ni chiffres, ni accents, ni ponctuation dans cette police.\n");
    sleep_ms(2500);

    // ================================================================
    etape(5, "IMAGE - asset plein ecran 400x300 pose par memcpy");
    // ================================================================
    memcpy(epd_framebuffer, fullscreen_base, EPD_BUFFER_SIZE);
    full_refresh("fullscreen_base");
    sleep_ms(2000);

    // ================================================================
    etape(6, "PARTIEL - 4 rafraichissements courts, un carre qui se deplace");
    // ================================================================
    printf("[test] le partiel doit etre nettement plus rapide et sans flash.\n");
    printf("       un leger ghosting apres plusieurs passages est NORMAL.\n");

    epd_fb_clear(true);
    epd_fb_write_typo(20, 20, "refresh partiel");
    full_refresh("fond du test partiel");
    sleep_ms(1500);

    for (int i = 0; i < 4; i++) {
        // efface la bande, puis redessine le carre a sa nouvelle position
        epd_fb_fill_rect(0, 120, EPD_WIDTH, 60, true);
        epd_fb_fill_rect(20 + i * 90, 130, 40, 40, false);

        char label[32];
        snprintf(label, sizeof(label), "position %d", i);
        partial_refresh(label);
        sleep_ms(800);
    }

    // ================================================================
    etape(7, "BALAYAGE - barre qui se remplit en rafraichissements partiels");
    // ================================================================
    epd_fb_clear(true);
    epd_fb_write_typo(20, 30, "balayage");
    epd_fb_fill_rect(38, 138, 324, 44, false);   // cadre de la barre
    epd_fb_fill_rect(42, 142, 316, 36, true);    // interieur blanc
    full_refresh("cadre de la barre");

    for (int i = 1; i <= 8; i++) {
        epd_fb_fill_rect(42, 142, (316 * i) / 8, 36, false);

        char label[32];
        snprintf(label, sizeof(label), "barre %d/8", i);
        partial_refresh(label);
    }
    printf("[test] 8 partiels enchaines : c'est la que le ghosting apparait.\n");
    sleep_ms(1500);

    // ================================================================
    etape(8, "MENU - la fonction reellement utilisee par le firmware");
    // ================================================================
    chrono_start();
    display_menu(true, 4, "check", "settings", "post token", "reglages");
    printf("[test] display_menu (full) : %u ms\n", chrono_stop_ms());
    busy_probe("display_menu");

    printf("\n[test] fin du cycle %d.\n", cycle);
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);   // laisse le temps au terminal serie de s'attacher

    printf("\n\n==========================================================\n");
    printf("   EDEL CHECK - banc de test ecran e-ink (autonome)\n");
    printf("   panneau %d x %d, framebuffer %d octets, 1 bit/pixel\n",
           EPD_WIDTH, EPD_HEIGHT, EPD_BUFFER_SIZE);
    printf("   la demo tourne EN BOUCLE - debrancher pour arreter\n");
    printf("==========================================================\n\n");

    printf("[test] init GPIO + SPI0 a %d Hz...\n", EPD_SPI_BAUDRATE);
    init_gpio();

    printf("[test] init du controleur SSD1683...\n");
    epd_init();
    busy_probe("epd_init()");

    /*
     * Boucle infinie : la demo rejoue sans fin. Un ecran e-ink ne consomme
     * rien entre deux rafraichissements, mais chaque rafraichissement use la
     * dalle - ne pas laisser tourner des heures pour rien.
     *
     * Si la console affiche des "termine." alors que l'ecran ne bouge pas :
     * voir docs/ARCHITECTURE.md section 0.3, epd_wait_busy() ment. Ce sont les
     * valeurs BUSY imprimees a chaque etape qui font foi, pas ces messages.
     */
    int cycle = 0;
    while (true) {
        run_demo(++cycle);

        // ecran d'attente entre deux passes
        epd_fb_clear(true);
        epd_fb_write_typo(60, 120, "on recommence");
        epd_fb_write_typo(60, 160, "dans trois secondes");
        epd_display_update_full();
        sleep_ms(3000);
    }
}
