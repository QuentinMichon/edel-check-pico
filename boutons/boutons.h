#ifndef EDEL_CHECK_PICO_BOUTONS_H
#define EDEL_CHECK_PICO_BOUTONS_H

void boutons_init(void);

// Renvoie '1' a '4' pour le dernier bouton presse, 0 si aucun. La case est videe a la
// lecture, de sorte qu'un appui ne soit traite qu'une fois.
char boutons_lire(void);

#endif //EDEL_CHECK_PICO_BOUTONS_H
