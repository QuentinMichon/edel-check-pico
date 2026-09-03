// Sortie du framebuffer sur la console serie.
//
// La dalle du boitier ne repond plus : sa ligne de reset est en court-circuit avec le 3V3
// et le panneau ne s'initialise donc jamais (voir docs/ARCHITECTURE.md). L'image, elle,
// arrive bien : le cloud la pousse en huit fragments et le boitier la reassemble dans son
// framebuffer. Elle est simplement invisible.
//
// Ce module la sort en texte. Projetee sur un ecran, un QR code ainsi rendu reste lisible
// par un telephone : la demonstration se fait donc avec le vrai boitier, pas avec un
// simulateur.
//
// Convention du framebuffer : 1 bit par pixel, bit a 1 = BLANC, MSB = pixel de gauche.
#include <stdio.h>

#include "screen/epd_driver.h"
#include "screen/epd_console.h"

// Deux caracteres par bloc horizontal : une cellule de terminal est environ deux fois plus
// haute que large, sans cela l'image sort ecrasee et le QR devient illisible.
static bool bloc_noir(int bx, int by, int pas) {
    int noirs = 0, total = 0;
    for (int y = by * pas; y < (by + 1) * pas && y < EPD_HEIGHT; y++) {
        for (int x = bx * pas; x < (bx + 1) * pas && x < EPD_WIDTH; x++) {
            int octet = y * EPD_WIDTH_BYTES + (x >> 3);
            int bit = 7 - (x & 7);
            if (((epd_framebuffer[octet] >> bit) & 1) == 0) noirs++;   // 0 = noir
            total++;
        }
    }
    return total && noirs * 2 >= total;
}

void epd_console_dump(int pas) {
    if (pas < 1) pas = 1;
    int cols = (EPD_WIDTH  + pas - 1) / pas;
    int rows = (EPD_HEIGHT + pas - 1) / pas;

    printf("\n[ecran] rendu texte du framebuffer, %d x %d blocs de %d px\n", cols, rows, pas);
    for (int by = 0; by < rows; by++) {
        for (int bx = 0; bx < cols; bx++) {
            bool noir = bloc_noir(bx, by, pas);
            putchar(noir ? '#' : ' ');
            putchar(noir ? '#' : ' ');
        }
        putchar('\n');
    }
    printf("[ecran] fin du rendu\n\n");
}
