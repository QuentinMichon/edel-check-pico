#ifndef EPD_CONSOLE_H
#define EPD_CONSOLE_H

// Sort le framebuffer sur la console serie, en texte.
//
// `pas` est la taille du bloc en pixels : 4 donne 100 x 75 blocs, soit 200 colonnes, et
// conserve assez de definition pour qu'un QR code reste scannable une fois projete.
// 8 donne une vignette de 50 x 38 blocs, lisible mais trop grossiere pour un QR.
void epd_console_dump(int pas);

#endif
