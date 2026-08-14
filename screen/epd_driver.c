//
// Created by Quentin Michon on 22.07.2026.
//
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "epd_driver.h"
#include "pico/stdlib.h"
#include "gpio/gpio_driver.h"
#include "hardware/spi.h"

#include "assets/battery/epd_image_bat_80.h"
#include "assets/typo/epd_typo_5.h"
#include "assets/full_screen/epd_image_scanner.h"

uint8_t epd_framebuffer[EPD_BUFFER_SIZE];

/*
 *      EPD RESET
 */
void epd_reset(void) {
    // Timings alignés sur le driver officiel Adafruit (10 ms par phase).
    // Notre impulsion basse de 2 ms était courte, d'autant que le signal
    // traverse un level shifter 74LCX245 sur le breakout avant d'atteindre
    // le panneau.
    gpio_put(PIN_RST, 1);
    sleep_ms(10);

    gpio_put(PIN_RST, 0);
    sleep_ms(10);

    gpio_put(PIN_RST, 1);
    sleep_ms(20);

    epd_wait_busy(1000);
}

/*
 *      EPD SEND CMD
 */
void epd_send_command(const uint8_t *cmd) {
    gpio_put(PIN_DC, 0);   // LOW = commande

    gpio_put(PIN_ECS, 0);  // sélectionne l'écran
    spi_write_blocking(EPD_SPI_PORT, cmd, 1);
    gpio_put(PIN_ECS, 1);  // désélectionne
}

/*
 *      EPD SEND DATA (1 Octet)
 */
void epd_send_data(const uint8_t *data) {
    gpio_put(PIN_DC, 1);

    gpio_put(PIN_ECS, 0);
    spi_write_blocking(EPD_SPI_PORT, data, 1);
    gpio_put(PIN_ECS, 1);
}

/*
 *      EPD BUSY WAIT
 */
// TODO CLEAN DEBUG - enlever printf + var
bool epd_wait_busy(uint32_t timeout_ms) {
    absolute_time_t start = get_absolute_time();

    // Phase 1 : attend que BUSY MONTE (fenêtre max 100 ms)
    bool seen_high = false;
    while (absolute_time_diff_us(start, get_absolute_time()) < 100 * 1000) {
        if (gpio_get(PIN_BUSY)) {
            seen_high = true;
            break;
        }
    }

    // Phase 2 : attend que BUSY REDESCENDE
    while (gpio_get(PIN_BUSY)) {
        if ( absolute_time_diff_us(start, get_absolute_time()) / 1000 > timeout_ms) {
#ifdef DEBUG_PRINT
            printf("[screen] epd_wait_busy: timeout apres %u ms\n", timeout_ms);
#endif
            return false;
        }
        sleep_ms(5);
    }

    uint32_t elapsed_ms = (uint32_t)(absolute_time_diff_us(start, get_absolute_time()) / 1000);
#ifdef DEBUG_PRINT
    printf("[screen] epd_wait_busy: BUSY vu HIGH = %s, duree totale = %u ms\n",
           seen_high ? "OUI" : "NON (jamais monte !)", elapsed_ms);
#endif

    return true;
}

/*
 *      EPD INIT SEQ
 *
 *      Séquence d'initialisation du SSD1683, suivant le flow recommandé par le
 *      fabricant (datasheet section 9.1) : reset, config gate/RAM/border, puis
 *      chargement de la LUT noir/blanc depuis l'OTP interne.
 */
void epd_init(void) {
    uint8_t cmd = 0x00;
    uint8_t data = 0x00;

    epd_reset();

    // -- SW reset --
    cmd = 0x12;
    epd_send_command(&cmd);
    epd_wait_busy(1000);

    // Note : Display Update Control 1 (0x21) n'est PAS configuré ici.
    // Le driver de référence de ce panneau (GxEPD2, GDEY042T81) l'envoie
    // avant CHAQUE refresh, avec une valeur différente selon le mode :
    // 0x40 (bypass du plan 0x26) pour le full, 0x00 (plan 0x26 actif,
    // nécessaire au diff) pour le partiel.

    // -- Driver output Control --
    // configure le nombre de lignes = hauteur du panneau.
    // (300 - 1) = 0x12B
    // 2 Bytes =>
    //                  MUX[7:0] = (HEIGH - 1) & 0xFF
    //                  MUX[8]   = (HEIGH - 1) & 0x01
    cmd = 0x01;
    epd_send_command(&cmd);
    data = (EPD_HEIGHT - 1) & 0xFF;
    epd_send_data(&data);
    data = ((EPD_HEIGHT - 1) >> 8) & 0x01;
    epd_send_data(&data);
    data = 0x00;
    epd_send_data(&data);

    // --- Data Entry Mode Setting (0x11) ---
    // ID[1:0] = 11 : X et Y s'incrémentent tous les deux après chaque
    // octet écrit en RAM (comportement le plus naturel pour remplir une
    // image ligne par ligne, dans l'ordre habituel de lecture).
    // AM = 0 : l'auto-incrément se fait en direction X d'abord.
    cmd = 0x11;
    epd_send_command(&cmd);
    data = 0x03;
    epd_send_data(&data);

    // --- Set RAM X Address Start/End (0x44) ---
    // Unité = 8 pixels par "adresse" (la RAM est organisée en octets, donc
    // 400 pixels de large = 400/8 = 50 octets = adresses 0 à 49).
    cmd = 0x44;
    epd_send_command(&cmd);
    data = 0x00;                        // XStart = 0
    epd_send_data(&data);
    data = (EPD_WIDTH / 8) - 1;         // XEnd = 49
    epd_send_data(&data);

    // --- Set RAM Y Address Start/End (0x45) ---
    // Unité = 1 ligne. 300 lignes = adresses 0 à 299 (0x12B).
    cmd = 0x45;
    epd_send_command(&cmd);
    data = 0x00;
    epd_send_data(&data);
    epd_send_data(&data);
    data = (EPD_HEIGHT - 1) & 0xFF;
    epd_send_data(&data);
    data = ((EPD_HEIGHT - 1) >> 8) & 0x01;
    epd_send_data(&data);

    // --- Border Waveform Control (0x3C) ---
    // 0x01 : la valeur du driver de référence GxEPD2 pour ce panneau
    // (GDEY042T81). Réglé une fois pour toutes ici, valable pour les
    // deux modes de refresh.
    cmd = 0x3C;
    epd_send_command(&cmd);
    data = 0x01;
    epd_send_data(&data);

#ifdef DEBUG_PRINT
    printf("[screen] epd_init: sequence de config : OK.\n");
#endif

    // --- CHARGEMENT DU LUT ---

    // --- Temperature Sensor Control (0x18) ---
    // 0x80 : sélectionne le capteur de température interne du chip
    // (plutôt qu'un capteur externe I2C, qu'on n'a pas câblé). Le chip
    // s'en sert pour choisir la bonne LUT (waveform) selon la
    // température ambiante actuelle.
    cmd = 0x18;
    epd_send_command(&cmd);
    data = 0x80;
    epd_send_data(&data);

    // Note : pas de chargement de LUT séparé ici. Le driver officiel
    // Adafruit n'en fait pas : la séquence 0x22 = 0xF7 envoyée au moment
    // du refresh inclut déjà "load temperature" et "load LUT" depuis
    // l'OTP, juste avant le driving du panneau.

    printf("[screen] e-paper driver (epd) ready!\n");
}

// ========= FRAMEBUFFER ========
void epd_fb_clear(bool white) {
    uint8_t fill_value = white ? 0xFF : 0x00;

    for (int i = 0; i < EPD_BUFFER_SIZE; i++) {
        epd_framebuffer[i] = fill_value;
    }
}

// Positionne un pixel unique à (x, y) dans le framebuffer.
// x va de 0 à EPD_WIDTH-1, y de 0 à EPD_HEIGHT-1.
// white = true -> pixel blanc (bit à 1), white = false -> pixel noir (bit à 0)
void epd_fb_set_pixel(int x, int y, bool white) {

    if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) {
        return;
    }

    int byte_index = y * EPD_WIDTH_BYTES + (x / 8);
    int bit_index = x % 8;

    // Le bit 7 (MSB) correspond au pixel le plus à gauche du groupe de 8,
    // le bit 0 (LSB) au plus à droite -> d'où "7 - bit_index".
    uint8_t mask = 1 << (7 - bit_index);

    if (white) {
        epd_framebuffer[byte_index] |= mask;   // force le bit à 1
    } else {
        epd_framebuffer[byte_index] &= ~mask;  // force le bit à 0
    }

}

// Copie une image 1 bit/pixel (format exporté par epd_paint.html) dans le
// framebuffer, coin haut-gauche en (x, y).
//   img : le tableau exporté (bit 1 = blanc, MSB = pixel de gauche)
//   w   : largeur en PIXELS (la constante <NOM>_W du .h généré)
//   h   : hauteur en pixels (la constante <NOM>_H)
// Le stride source est recalculé ici : chaque ligne de l'image occupe
// (w + 7) / 8 octets, alors qu'une ligne du framebuffer en fait 50 — c'est
// précisément ce désalignement qui interdit un memcpy direct.
// Les pixels qui débordent de l'écran sont ignorés (le clipping est déjà
// assuré par les bornes de epd_fb_set_pixel).
void epd_fb_draw_image(int x, int y, const uint8_t *img, int w, int h) {
    int w_bytes = (w + 7) / 8;    // largeur d'une ligne source, en octets

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint8_t byte = img[row * w_bytes + (col / 8)];
            bool white = byte & (0x80 >> (col % 8));
            epd_fb_set_pixel(x + col, y + row, white);
        }
    }
}

void epd_fb_write_typo(int x, int y, char *text) {
    size_t cursor = x;
    size_t nb_char = 0;

    nb_char = strlen(text);

    // write
    for (int i = 0; i < nb_char; i++) {
        switch (toupper(text[i])) {
            case 'A':
                epd_fb_draw_image(cursor, y, epd_typo_5_a, EPD_TYPO_5_A_W, EPD_TYPO_5_A_H);
                cursor += EPD_TYPO_5_A_W;
                break;
            case 'B':
                epd_fb_draw_image(cursor, y, epd_typo_5_b, EPD_TYPO_5_B_W, EPD_TYPO_5_B_H);
                cursor += EPD_TYPO_5_B_W;
                break;
            case 'C':
                epd_fb_draw_image(cursor, y, epd_typo_5_c, EPD_TYPO_5_C_W, EPD_TYPO_5_C_H);
                cursor += EPD_TYPO_5_C_W;
                break;
            case 'D':
                epd_fb_draw_image(cursor, y, epd_typo_5_d, EPD_TYPO_5_D_W, EPD_TYPO_5_D_H);
                cursor += EPD_TYPO_5_D_W;
                break;
            case 'E':
                epd_fb_draw_image(cursor, y, epd_typo_5_e, EPD_TYPO_5_E_W, EPD_TYPO_5_E_H);
                cursor += EPD_TYPO_5_E_W;
                break;
            case 'F':
                epd_fb_draw_image(cursor, y, epd_typo_5_f, EPD_TYPO_5_F_W, EPD_TYPO_5_F_H);
                cursor += EPD_TYPO_5_F_W;
                break;
            case 'G':
                epd_fb_draw_image(cursor, y, epd_typo_5_g, EPD_TYPO_5_G_W, EPD_TYPO_5_G_H);
                cursor += EPD_TYPO_5_G_W;
                break;
            case 'H':
                epd_fb_draw_image(cursor, y, epd_typo_5_h, EPD_TYPO_5_H_W, EPD_TYPO_5_H_H);
                cursor += EPD_TYPO_5_H_W;
                break;
            case 'I':
                epd_fb_draw_image(cursor, y, epd_typo_5_i, EPD_TYPO_5_I_W, EPD_TYPO_5_I_H);
                cursor += EPD_TYPO_5_I_W;
                break;
            case 'J':
                epd_fb_draw_image(cursor, y, epd_typo_5_j, EPD_TYPO_5_J_W, EPD_TYPO_5_J_H);
                cursor += EPD_TYPO_5_J_W;
                break;
            case 'K':
                epd_fb_draw_image(cursor, y, epd_typo_5_k, EPD_TYPO_5_K_W, EPD_TYPO_5_K_H);
                cursor += EPD_TYPO_5_K_W;
                break;
            case 'L':
                epd_fb_draw_image(cursor, y, epd_typo_5_l, EPD_TYPO_5_L_W, EPD_TYPO_5_L_H);
                cursor += EPD_TYPO_5_L_W;
                break;
            case 'M':
                epd_fb_draw_image(cursor, y, epd_typo_5_m, EPD_TYPO_5_M_W, EPD_TYPO_5_M_H);
                cursor += EPD_TYPO_5_M_W;
                break;
            case 'N':
                epd_fb_draw_image(cursor, y, epd_typo_5_n, EPD_TYPO_5_N_W, EPD_TYPO_5_N_H);
                cursor += EPD_TYPO_5_N_W;
                break;
            case 'O':
                epd_fb_draw_image(cursor, y, epd_typo_5_o, EPD_TYPO_5_O_W, EPD_TYPO_5_O_H);
                cursor += EPD_TYPO_5_O_W;
                break;
            case 'P':
                epd_fb_draw_image(cursor, y, epd_typo_5_p, EPD_TYPO_5_P_W, EPD_TYPO_5_P_H);
                cursor += EPD_TYPO_5_P_W;
                break;
            case 'Q':
                epd_fb_draw_image(cursor, y, epd_typo_5_q, EPD_TYPO_5_Q_W, EPD_TYPO_5_Q_H);
                cursor += EPD_TYPO_5_Q_W;
                break;
            case 'R':
                epd_fb_draw_image(cursor, y, epd_typo_5_r, EPD_TYPO_5_R_W, EPD_TYPO_5_R_H);
                cursor += EPD_TYPO_5_R_W;
                break;
            case 'S':
                epd_fb_draw_image(cursor, y, epd_typo_5_s, EPD_TYPO_5_S_W, EPD_TYPO_5_S_H);
                cursor += EPD_TYPO_5_S_W;
                break;
            case 'T':
                epd_fb_draw_image(cursor, y, epd_typo_5_t, EPD_TYPO_5_T_W, EPD_TYPO_5_T_H);
                cursor += EPD_TYPO_5_T_W;
                break;
            case 'U':
                epd_fb_draw_image(cursor, y, epd_typo_5_u, EPD_TYPO_5_U_W, EPD_TYPO_5_U_H);
                cursor += EPD_TYPO_5_U_W;
                break;
            case 'V':
                epd_fb_draw_image(cursor, y, epd_typo_5_v, EPD_TYPO_5_V_W, EPD_TYPO_5_V_H);
                cursor += EPD_TYPO_5_V_W;
                break;
            case 'W':
                epd_fb_draw_image(cursor, y, epd_typo_5_w, EPD_TYPO_5_W_W, EPD_TYPO_5_W_H);
                cursor += EPD_TYPO_5_W_W;
                break;
            case 'X':
                epd_fb_draw_image(cursor, y, epd_typo_5_x, EPD_TYPO_5_X_W, EPD_TYPO_5_X_H);
                cursor += EPD_TYPO_5_X_W;
                break;
            case 'Y':
                epd_fb_draw_image(cursor, y, epd_typo_5_y, EPD_TYPO_5_Y_W, EPD_TYPO_5_Y_H);
                cursor += EPD_TYPO_5_Y_W;
                break;
            case 'Z':
                epd_fb_draw_image(cursor, y, epd_typo_5_z, EPD_TYPO_5_Z_W, EPD_TYPO_5_Z_H);
                cursor += EPD_TYPO_5_Z_W;
                break;
            case ' ':
                cursor += 8;
                break;
            case '\'':
                epd_fb_draw_image(cursor, y, epd_typo_5_apos, EPD_TYPO_5_APOS_W, EPD_TYPO_5_APOS_H);
                cursor += EPD_TYPO_5_APOS_W;
                break;
            default:
                printf("char not supported\n");
                cursor += 8;
                break;
        }
    }
}

// Envoie le framebuffer dans UN plan de RAM du chip :
//   0x24 = plan "nouvelle image" (celui que le refresh affiche)
//   0x26 = plan "ancienne image" (la référence du diff en refresh partiel)
void epd_write_plane(uint8_t ram_cmd) {
    uint8_t cmd = 0x00;
    uint8_t data = 0x00;

    // --- RAM X/Y Address Counter (0x4E / 0x4F) ---
    // Repositionne le "curseur" d'écriture au début de la fenêtre RAM
    // (compteur partagé par les deux plans). Indispensable avant CHAQUE
    // envoi : le compteur reste là où la dernière écriture s'est arrêtée
    // (l'analogie POSIX : un write() sans lseek() préalable — on écrirait
    // depuis une position résiduelle et une partie de l'écran garderait
    // son ancien contenu).
    cmd = 0x4E;
    epd_send_command(&cmd);
    data = 0x00;                        // X counter = 0
    epd_send_data(&data);

    cmd = 0x4F;
    epd_send_command(&cmd);
    data = 0x00;                        // Y counter = 0 (2 octets, little endian)
    epd_send_data(&data);
    epd_send_data(&data);

    epd_send_command(&ram_cmd);

    for (int i = 0; i < EPD_BUFFER_SIZE; i++) {
        epd_send_data(&epd_framebuffer[i]);
    }
}

/*
 *      FULL REFRESH (display mode 1)
 *
 *      Réécrit tout l'écran avec la séquence de nettoyage complète (les
 *      flashs noir/blanc, ~2.3 s). C'est le refresh de référence : il
 *      efface tout ghosting accumulé. On écrit l'image dans les DEUX
 *      plans : 0x24 pour l'affichage, 0x26 pour que le plan "ancien"
 *      reflète l'écran réel — condition de départ saine du prochain
 *      refresh partiel.
 */
void epd_display_update_full(void) {
    uint8_t cmd = 0x00;
    uint8_t data = 0x00;

    epd_write_plane(0x24);
    epd_write_plane(0x26);

    // Display Update Control 1 : 0x40 = bypass du plan 0x26 ("RED") avec
    // la valeur 0. En full refresh, seul le plan 0x24 compte — le driver
    // de référence envoie ce réglage avant chaque full.
    cmd = 0x21;
    epd_send_command(&cmd);
    data = 0x40;
    epd_send_data(&data);
    data = 0x00;
    epd_send_data(&data);

    // 0x22 = 0xF7 : enable clock -> enable analog -> load temperature ->
    // load LUT (mode 1) -> display -> disable analog -> disable clock.
    cmd = 0x22;
    epd_send_command(&cmd);
    data = 0xF7;
    epd_send_data(&data);

    cmd = 0x20;
    epd_send_command(&cmd);

    bool ok = epd_wait_busy(15000);
    printf("[screen] epd_display_update_full: %s\n", ok ? "termine." : "TIMEOUT.");
}

/*
 *      PARTIAL REFRESH (display mode 2)
 *
 *      Le chip compare pixel par pixel le plan 0x24 (nouvelle image) au
 *      plan 0x26 (image actuellement affichée) et ne pilote QUE les
 *      pixels qui changent, avec une forme d'onde courte : pas de flash
 *      global, ~0.3-0.7 s. En contrepartie, chaque passage laisse un
 *      léger ghosting : prévoir un full refresh périodique (géré par
 *      l'appelant).
 *
 *      Protocole à respecter : 0x26 doit contenir l'image réellement
 *      affichée AVANT le refresh, et être resynchronisé APRÈS pour
 *      devenir la référence du prochain diff.
 */
void epd_display_update_partial(void) {
    uint8_t cmd = 0x00;
    uint8_t data = 0x00;

    // Seule la nouvelle image part dans 0x24
    epd_write_plane(0x24);

    // Display Update Control 1 : 0x00 = plan 0x26 actif
    cmd = 0x21;
    epd_send_command(&cmd);
    data = 0x00;
    epd_send_data(&data);
    data = 0x00;
    epd_send_data(&data);

    // 0x22 = 0xFC : enable clock -> enable analog -> load temperature ->
    // load LUT (mode 2, waveform différentielle) -> display. Contrairement
    // au full (0xF7), l'analogique n'est PAS coupé à la fin : les partiels
    // consécutifs s'enchaînent plus vite. (Valeur du driver de référence.)
    cmd = 0x22;
    epd_send_command(&cmd);
    data = 0xFC;
    epd_send_data(&data);

    cmd = 0x20;
    epd_send_command(&cmd);

    bool ok = epd_wait_busy(5000);

    // Resynchronise les DEUX plans avec l'image affichée
    epd_write_plane(0x26);
    epd_write_plane(0x24);

    printf("[screen] epd_display_update_partial: %s\n", ok ? "termine." : "TIMEOUT.");
}

#include <stdarg.h>

void display_menu(bool full, int nb_lines, ...) {
    if (nb_lines > 4 || nb_lines < 1) {
        return;
    }

    int h = 46;
    va_list args;
    va_start(args, nb_lines);

    memcpy(epd_framebuffer, fullscreen_base, EPD_BUFFER_SIZE);
    epd_fb_draw_image(342, 270, epd_image_bat_80, EPD_IMAGE_BAT_W, EPD_IMAGE_BAT_H);
    epd_fb_write_typo(10, 270, "edel id");

    for (int i = 0; i < nb_lines; i++) {
        epd_fb_write_typo(60, h, va_arg(args, char*));
        h += 52;
    }

    if (full) {
        epd_display_update_full();
    } else {
        epd_display_update_partial();
    }
}