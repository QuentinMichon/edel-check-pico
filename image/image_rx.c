#include "image_rx.h"

#include <stdio.h>
#include <string.h>

#include "screen/epd_driver.h"

#define ENTETE 8
#define CHARGE_2BPP 4088          // octets utiles d'un fragment plein
#define CHARGE_1BPP (CHARGE_2BPP / 2)

// Le contrat plafonne a 8 fragments pour un plein ecran. On garde de la marge sans
// exagerer : le masque tient sur un u32.
#define FRAGMENTS_MAX 32

static uint32_t img_id_courant;
static uint16_t total_attendu;
static uint32_t recus_masque;      // un bit par seq
static int      recus_nombre;

// Etat de la tranche en cours de reception
static uint8_t  entete[ENTETE];
static uint8_t  entete_n;
static uint16_t seq_courant;
static bool     fragment_valide;   // faux = fragment perime ou hors bornes, on jette
static size_t   ecrit_offset;      // prochain octet a ecrire dans le framebuffer

// L'octet 2 bpp en attente de son voisin.
//
// Deux octets 2 bpp donnent un octet 1 bpp. Une tranche lwIP peut se terminer sur un
// nombre impair d'octets : sans ce report, la conversion se decalerait d'un demi-octet et
// tout le reste de l'image serait brouille - un defaut qui ne ressemblerait pas du tout a
// un probleme de decoupage.
static uint8_t report;
static bool    report_present;

uint32_t image_rx_current_id(void) {
    return img_id_courant;
}

int image_rx_missing(void) {
    if (total_attendu == 0) {
        return -1;
    }
    return total_attendu - recus_nombre;
}

void image_rx_missing_json(char *out, size_t out_len) {
    size_t n = 0;
    n += (size_t) snprintf(out + n, out_len - n, "[");
    bool premier = true;
    for (uint16_t s = 0; s < total_attendu && n + 8 < out_len; s++) {
        if (!(recus_masque & (1u << s))) {
            n += (size_t) snprintf(out + n, out_len - n, "%s%u", premier ? "" : ",", s);
            premier = false;
        }
    }
    snprintf(out + n, out_len - n, "]");
}

void image_rx_reset(void) {
    img_id_courant = 0;
    total_attendu = 0;
    recus_masque = 0;
    recus_nombre = 0;
    entete_n = 0;
    fragment_valide = false;
    report_present = false;
}

void image_rx_begin(void) {
    entete_n = 0;
    fragment_valide = false;
    report_present = false;
    ecrit_offset = 0;
}

// Convertit un octet 2 bpp (4 pixels, MSB a gauche) en un demi-octet 1 bpp.
static inline uint8_t quatre_pixels(uint8_t octet2bpp) {
    uint8_t quartet = 0;
    for (int k = 0; k < 4; k++) {
        // niveau du pixel k : bits (6-2k) et (7-2k)
        uint8_t niveau = (uint8_t) ((octet2bpp >> (6 - 2 * k)) & 0x3);
        if (niveau >= 0x2) {                 // LIGHT ou WHITE -> bit a 1 (blanc)
            quartet |= (uint8_t) (0x8 >> k);
        }
    }
    return quartet;
}

static void ecrire_octet_2bpp(uint8_t octet) {
    if (!report_present) {
        report = octet;
        report_present = true;
        return;
    }

    // Deux octets 2 bpp -> un octet 1 bpp : le premier donne les 4 bits de poids fort.
    uint8_t sortie = (uint8_t) ((quatre_pixels(report) << 4) | quatre_pixels(octet));
    report_present = false;

    if (ecrit_offset < EPD_BUFFER_SIZE) {
        epd_framebuffer[ecrit_offset++] = sortie;
    }
}

static void traiter_entete(void) {
    uint32_t img_id = ((uint32_t) entete[0] << 24) | ((uint32_t) entete[1] << 16) |
                      ((uint32_t) entete[2] << 8)  |  (uint32_t) entete[3];
    uint16_t seq    = (uint16_t) (((uint16_t) entete[4] << 8) | entete[5]);
    uint16_t total  = (uint16_t) (((uint16_t) entete[6] << 8) | entete[7]);

    // img_id est un compteur croissant par boitier, alloue par le serveur : tout fragment
    // portant un identifiant inferieur au courant est PROUVABLEMENT perime. La comparaison
    // est donc decidable, sans ambiguite ni fenetre de tolerance.
    if (img_id < img_id_courant) {
        fragment_valide = false;
        return;
    }

    if (img_id > img_id_courant) {
        // Nouvelle image : on repart de zero. Le serveur garantit que deux images ne
        // s'entrelacent jamais sur ce topic.
        img_id_courant = img_id;
        total_attendu = total;
        recus_masque = 0;
        recus_nombre = 0;
    }

    if (total > FRAGMENTS_MAX || seq >= total) {
        printf("[img] fragment hors bornes (seq %u/%u) - ignore\n", seq, total);
        fragment_valide = false;
        return;
    }

    seq_courant = seq;
    fragment_valide = true;
    ecrit_offset = (size_t) seq * CHARGE_1BPP;
}

void image_rx_data(const uint8_t *data, uint16_t len) {
    // L'en-tete de 8 octets peut etre coupe entre deux tranches : on l'accumule.
    while (entete_n < ENTETE && len > 0) {
        entete[entete_n++] = *data++;
        len--;
        if (entete_n == ENTETE) {
            traiter_entete();
        }
    }

    if (!fragment_valide) {
        return;
    }
    for (uint16_t i = 0; i < len; i++) {
        ecrire_octet_2bpp(data[i]);
    }
}

bool image_rx_end(void) {
    if (!fragment_valide) {
        return false;
    }

    // QoS 1 garantit " au moins une fois " : un fragment peut arriver deux fois. Le couple
    // (img_id, seq) rend donc l'operation idempotente - on ne recompte pas un seq deja vu.
    if (!(recus_masque & (1u << seq_courant))) {
        recus_masque |= (1u << seq_courant);
        recus_nombre++;
    }

    return total_attendu > 0 && recus_nombre >= total_attendu;
}
