//
// Created by Quentin Michon on 22.07.2026.
//

#ifndef EDEL_CHECK_PICO_EPD_DRIVER_H
#define EDEL_CHECK_PICO_EPD_DRIVER_H

#include "assets/battery/epd_image_bat_80.h"
#include "assets/full_screen/epd_image_scanner.h"
#include "assets/full_screen/fullscreen-base.h"
#include "assets/full_screen/fullscreen-chargement.h"
#include "assets/typo/epd_typo_5.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

// Dimension du panneau (Adafruit 6381, 400x300)
#define EPD_WIDTH       400
#define EPD_HEIGHT      300

// Largeur en octets : 8 pixels par octet (1 bit par pixel)
#define EPD_WIDTH_BYTES  (EPD_WIDTH / 8)

// Taille totale du framebuffer en octets : 50 * 300 = 15000
#define EPD_BUFFER_SIZE  (EPD_WIDTH_BYTES * EPD_HEIGHT)

// Le framebuffer lui-même : un simple tableau statique en RAM.
// Convention : bit = 1 -> pixel blanc, bit = 0 -> pixel noir
// (cf. datasheet SSD1683, description commande 0x24).
extern uint8_t epd_framebuffer[EPD_BUFFER_SIZE];



// ====== Prototypes ======
void epd_reset(void);
void epd_send_command(const uint8_t *cmd);
void epd_send_data(const uint8_t *data);
bool epd_wait_busy(uint32_t timeout_ms);
void epd_init(void);

// manip du frame buffer
void epd_fb_clear(bool white);
void epd_fb_set_pixel(int x, int y, bool white);
void epd_fb_fill_rect(int x, int y, int w, int h, bool white);
void epd_fb_draw_image(int x, int y, const uint8_t *img, int w, int h);
void epd_fb_write_typo(int x, int y, char *text);

// manip du E-Ink
void epd_write_plane(uint8_t ram_cmd);
void epd_display_update_full(void);
void epd_display_update_partial(void);

void display_menu(bool full, int nb_lines, ...);

#endif //EDEL_CHECK_PICO_EPD_DRIVER_H
