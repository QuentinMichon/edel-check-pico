#include "epd_text.h"

#include <ctype.h>
#include <string.h>

#include "epd_driver.h"

#define GLYPHE_COLONNES 5
#define GLYPHE_LIGNES   7
#define ESPACEMENT      1   // en points de matrice, mis a l'echelle comme le reste

// Matrice 5x7, une colonne par octet, bit 0 = ligne du haut.
//
// C'est la disposition classique des petits afficheurs a matrice de points : elle tient en
// 5 octets par glyphe et se dessine en deux boucles, sans table de correspondance ni
// decodage. Ordre : 0-9 puis A-Z.
static const unsigned char police_5x7[36][GLYPHE_COLONNES] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E},  // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00},  // 1
    {0x42, 0x61, 0x51, 0x49, 0x46},  // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31},  // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10},  // 4
    {0x27, 0x45, 0x45, 0x45, 0x39},  // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30},  // 6
    {0x01, 0x71, 0x09, 0x05, 0x03},  // 7
    {0x36, 0x49, 0x49, 0x49, 0x36},  // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E},  // 9
    {0x7E, 0x11, 0x11, 0x11, 0x7E},  // A
    {0x7F, 0x49, 0x49, 0x49, 0x36},  // B
    {0x3E, 0x41, 0x41, 0x41, 0x22},  // C
    {0x7F, 0x41, 0x41, 0x22, 0x1C},  // D
    {0x7F, 0x49, 0x49, 0x49, 0x41},  // E
    {0x7F, 0x09, 0x09, 0x01, 0x01},  // F
    {0x3E, 0x41, 0x41, 0x51, 0x32},  // G
    {0x7F, 0x08, 0x08, 0x08, 0x7F},  // H
    {0x00, 0x41, 0x7F, 0x41, 0x00},  // I
    {0x20, 0x40, 0x41, 0x3F, 0x01},  // J
    {0x7F, 0x08, 0x14, 0x22, 0x41},  // K
    {0x7F, 0x40, 0x40, 0x40, 0x40},  // L
    {0x7F, 0x02, 0x04, 0x02, 0x7F},  // M
    {0x7F, 0x04, 0x08, 0x10, 0x7F},  // N
    {0x3E, 0x41, 0x41, 0x41, 0x3E},  // O
    {0x7F, 0x09, 0x09, 0x09, 0x06},  // P
    {0x3E, 0x41, 0x51, 0x21, 0x5E},  // Q
    {0x7F, 0x09, 0x19, 0x29, 0x46},  // R
    {0x46, 0x49, 0x49, 0x49, 0x31},  // S
    {0x01, 0x01, 0x7F, 0x01, 0x01},  // T
    {0x3F, 0x40, 0x40, 0x40, 0x3F},  // U
    {0x1F, 0x20, 0x40, 0x20, 0x1F},  // V
    {0x7F, 0x20, 0x18, 0x20, 0x7F},  // W
    {0x63, 0x14, 0x08, 0x14, 0x63},  // X
    {0x03, 0x04, 0x78, 0x04, 0x03},  // Y
    {0x61, 0x51, 0x49, 0x45, 0x43},  // Z
};

// -1 pour tout ce que la police ne couvre pas : on dessine alors un blanc, plutot que de
// laisser passer un glyphe faux qui donnerait un code impossible a saisir.
static int index_glyphe(char c) {
    c = (char) toupper((unsigned char) c);
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
    return -1;
}

void epd_fb_write_big(int x, int y, const char *text, int scale) {
    if (text == NULL || scale < 1) {
        return;
    }

    int curseur = x;
    for (size_t i = 0; text[i] != '\0'; i++) {
        int g = index_glyphe(text[i]);

        if (g >= 0) {
            for (int col = 0; col < GLYPHE_COLONNES; col++) {
                unsigned char colonne = police_5x7[g][col];
                for (int ligne = 0; ligne < GLYPHE_LIGNES; ligne++) {
                    if (colonne & (1u << ligne)) {
                        // false = noir : le framebuffer suit la convention bit 1 = blanc.
                        epd_fb_fill_rect(curseur + col * scale, y + ligne * scale,
                                         scale, scale, false);
                    }
                }
            }
        }
        curseur += (GLYPHE_COLONNES + ESPACEMENT) * scale;
    }
}

int epd_fb_big_width(const char *text, int scale) {
    if (text == NULL || scale < 1) {
        return 0;
    }
    size_t n = strlen(text);
    if (n == 0) {
        return 0;
    }
    // n glyphes et n-1 espacements : le dernier espacement ne compte pas dans la largeur.
    return (int) n * (GLYPHE_COLONNES + ESPACEMENT) * scale - ESPACEMENT * scale;
}

void epd_fb_write_big_centered(int y, const char *text, int scale) {
    int largeur = epd_fb_big_width(text, scale);
    int x = (EPD_WIDTH - largeur) / 2;
    epd_fb_write_big(x < 0 ? 0 : x, y, text, scale);
}

// Les accentues francais en UTF-8 tiennent tous sur deux octets commencant par 0xC3.
// On ne decode pas l'UTF-8 en general : on traite le seul cas qui se presente, les
// libelles saisis dans le portail.
void epd_text_fold_ascii(const char *in, char *out, size_t out_len) {
    if (out == NULL || out_len == 0) {
        return;
    }
    if (in == NULL) {
        out[0] = '\0';
        return;
    }

    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 1 < out_len; i++) {
        unsigned char c = (unsigned char) in[i];

        if (c == 0xC3 && in[i + 1] != '\0') {
            unsigned char suite = (unsigned char) in[++i];
            char base;
            switch (suite) {
                case 0xA0: case 0xA2: case 0xA4:
                case 0x80: case 0x82: case 0x84: base = 'A'; break;
                case 0xA7: case 0x87:           base = 'C'; break;
                case 0xA8: case 0xA9: case 0xAA: case 0xAB:
                case 0x88: case 0x89: case 0x8A: case 0x8B: base = 'E'; break;
                case 0xAE: case 0xAF: case 0x8E: case 0x8F: base = 'I'; break;
                case 0xB4: case 0xB6: case 0x94: case 0x96: base = 'O'; break;
                case 0xB9: case 0xBB: case 0xBC:
                case 0x99: case 0x9B: case 0x9C: base = 'U'; break;
                default:                        base = ' '; break;
            }
            out[j++] = base;
            continue;
        }

        if (c >= 0x80) {
            continue;              // tout autre octet multi-octets : ignore
        }
        if (c >= 'a' && c <= 'z') {
            out[j++] = (char) (c - 'a' + 'A');
        } else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            out[j++] = (char) c;
        } else {
            out[j++] = ' ';        // ponctuation et espaces : un blanc
        }
    }
    out[j] = '\0';
}
