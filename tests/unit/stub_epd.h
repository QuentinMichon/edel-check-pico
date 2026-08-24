// Bouchon du pilote d'ecran, pour compiler les modules purs sur la machine de developpement.
//
// Les tests unitaires tournent sur un PC, pas sur la carte : ils exercent la logique
// (conversion d'images, analyse du JSON, repli des accents) sans SPI, sans e-ink et sans
// materiel a brancher. Ce fichier fournit donc le strict minimum de ce que
// screen/epd_driver.h expose et que ces modules utilisent.
//
// Ce qui n'est PAS testable ici et reste du ressort des bancs materiels sous tests/ :
// le pilotage du panneau, la pile TLS, la flash, et le reseau.

#ifndef EDEL_CHECK_PICO_STUB_EPD_H
#define EDEL_CHECK_PICO_STUB_EPD_H

#include <stdbool.h>
#include <stdint.h>

#define EPD_WIDTH        400
#define EPD_HEIGHT       300
#define EPD_WIDTH_BYTES  (EPD_WIDTH / 8)
#define EPD_BUFFER_SIZE  (EPD_WIDTH_BYTES * EPD_HEIGHT)

extern uint8_t epd_framebuffer[EPD_BUFFER_SIZE];

void epd_fb_set_pixel(int x, int y, bool white);
void epd_fb_fill_rect(int x, int y, int w, int h, bool white);

#endif //EDEL_CHECK_PICO_STUB_EPD_H
