//
// Created by Quentin Michon on 24.07.2026.
//

#ifndef EDEL_CHECK_PICO_NAV_H
#define EDEL_CHECK_PICO_NAV_H

#include <stdbool.h>

extern  bool running;

typedef enum NAV_PAGE {
    NAV_PAGE_MENU,
    NAV_PAGE_SETTINGS,
    NAV_PAGE_VERIFY,      // les profils recus du cloud, un par touche
} nav_page_t;

// Traite au plus une entree, venue d'un bouton ou du clavier USB. Les deux produisent le
// meme caractere : '1' a '4'.
void nav_poll_input(void);

// Redessine la page courante. Sert a reprendre la main apres un ecran transitoire.
void nav_redraw(void);

#endif //EDEL_CHECK_PICO_NAV_H
