#ifndef EDEL_CHECK_PICO_EPD_TEXT_H
#define EDEL_CHECK_PICO_EPD_TEXT_H

#include <stdbool.h>
#include <stddef.h>

// Texte dessine, a taille libre - chiffres compris.
//
// Pourquoi ce module existe : epd_fb_write_typo ne connait que A-Z et l'apostrophe, parce
// que ses glyphes sont des bitmaps generes par epd_paint.html, absent des deux depots. Or
// l'alphabet des codes d'appairage est ABCDEFGHJKLMNPQRSTUVWXYZ23456789 : sans les
// chiffres, neuf codes sur dix s'affichent avec des trous, et un operateur ne peut pas
// saisir ce qu'il ne voit pas.
//
// Les glyphes sont donc traces ici en rectangles depuis une matrice 5x7, sans aucun asset.
// Deux consequences utiles : la taille devient un parametre (un code lu a un metre d'un
// comptoir n'a pas la meme taille qu'un libelle de menu), et plus rien ne depend d'un
// outil perdu.
//
// La police couvre 0-9, A-Z et l'espace. Tout autre caractere est dessine comme un espace.

// Dessine `text` en noir sur le framebuffer. `scale` est le cote, en pixels, d'un point de
// la matrice : un glyphe occupe donc 5*scale par 7*scale.
void epd_fb_write_big(int x, int y, const char *text, int scale);

// Largeur totale qu'occuperait `text`, pour pouvoir le centrer.
int epd_fb_big_width(const char *text, int scale);

// Dessine `text` centre horizontalement sur toute la largeur du panneau.
void epd_fb_write_big_centered(int y, const char *text, int scale);

// Replie une chaine UTF-8 sur l'alphabet que la police sait tracer : accents retires,
// tout en majuscules, caracteres inconnus remplaces par un espace.
//
// Les libelles de profils viennent du portail, saisis par un operateur - " Majorite
// verifiee " y arrive avec ses accents. Sans ce repli, chaque accentue s'afficherait comme
// un trou, et un libelle troue sur un comptoir a l'air d'un bug, pas d'une limite de
// police.
void epd_text_fold_ascii(const char *in, char *out, size_t out_len);

#endif //EDEL_CHECK_PICO_EPD_TEXT_H
