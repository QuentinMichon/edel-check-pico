#ifndef EDEL_CHECK_PICO_IMAGE_RX_H
#define EDEL_CHECK_PICO_IMAGE_RX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Reassemblage des images poussees par le cloud sur dev/{id}/img.
//
// Le boitier ne dessine rien : il recoit un bitmap deja trame et le pousse sur la dalle.
// C'est ce qui permet de changer l'apparence d'un ecran sans reflasher le parc, et ce qui
// fait qu'un boitier vole ne contient aucune logique metier a retro-conceler.
//
// ┌─────────────────────────────────────────────────────────────────────────┐
// │  CADRAGE (contrat MQTT §5) - chaque message sur img, en gros-boutiste :  │
// │    octet 0-3  img_id  u32      compteur croissant, alloue par le serveur │
// │    octet 4-5  seq     u16      0-based                                   │
// │    octet 6-7  total   u16      ceil(taille / 4088)                       │
// │    octet 8+   donnees          <= 4088 octets, 2 bits/pixel              │
// └─────────────────────────────────────────────────────────────────────────┘
//
// CONVERSION. Le cloud envoie 2 bits par pixel (4 gris), le framebuffer en tient 1. La
// conversion se fait A LA VOLEE, pendant la reception :
//
//   fragment plein   4088 o = 16352 px  ->  2044 o,  ecrit a seq * 2044
//   dernier fragment 1384 o =  5536 px  ->   692 o
//   total           30000 o             -> 15000 o  = exactement le framebuffer
//
// Tous les offsets sont entiers, pour tout seq. Aucun tampon de 30 Ko n'est donc
// necessaire - on ecrit directement dans epd_framebuffer a mesure que les octets
// arrivent.
//
// Les deux conventions sont MSB a gauche, et 0b11 = blanc / bit 1 = blanc pointent dans le
// meme sens : pas d'inversion, seulement un seuil (niveau >= 0b10 -> blanc).

// Debut d'un message sur le topic img.
void image_rx_begin(void);

// Une tranche du message, dans l'ordre. lwIP en livre plusieurs par fragment.
void image_rx_data(const uint8_t *data, uint16_t len);

// Fin du message. Retourne true si l'IMAGE ENTIERE est desormais complete - c'est le
// moment de rafraichir la dalle.
bool image_rx_end(void);

// Identifiant de l'image en cours d'assemblage, 0 si aucune.
uint32_t image_rx_current_id(void);

// Nombre de fragments encore attendus, -1 si aucune image en cours.
int image_rx_missing(void);

// Ecrit dans `out` la liste des seq manquants, au format JSON `[3,5]`. Sert a construire
// l'evenement img_abort du contrat.
void image_rx_missing_json(char *out, size_t out_len);

void image_rx_reset(void);

#endif //EDEL_CHECK_PICO_IMAGE_RX_H
