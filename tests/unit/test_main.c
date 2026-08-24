// Tests unitaires du firmware EdelCheck, executes sur la machine de developpement.
//
//   cd tests/unit && make
//
// Ils couvrent la logique qui ne depend d'aucun materiel, et surtout celle ou une erreur
// ne se verrait pas a l'oeil nu : un decalage d'un demi-octet dans la conversion d'image
// produit un QR que rien ne decode, mais qui ressemble a un QR.

#include <stdio.h>
#include <string.h>

#include "stub_epd.h"
#include "image/image_rx.h"
#include "profiles/profiles.h"
#include "screen/epd_text.h"

uint8_t epd_framebuffer[EPD_BUFFER_SIZE];

void epd_fb_set_pixel(int x, int y, bool white) {
    if (x < 0 || y < 0 || x >= EPD_WIDTH || y >= EPD_HEIGHT) return;
    int index = y * EPD_WIDTH_BYTES + x / 8;
    uint8_t bit = (uint8_t) (0x80 >> (x % 8));
    if (white) epd_framebuffer[index] |= bit;
    else       epd_framebuffer[index] &= (uint8_t) ~bit;
}

void epd_fb_fill_rect(int x, int y, int w, int h, bool white) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            epd_fb_set_pixel(i, j, white);
}

// ---------------------------------------------------------------------------

static int total, echecs;

#define VERIFIE(cond, ...)                                                    \
    do {                                                                      \
        total++;                                                              \
        if (!(cond)) {                                                        \
            echecs++;                                                         \
            printf("  ECHEC  " __VA_ARGS__);                                  \
            printf("\n         %s:%d  (%s)\n", __FILE__, __LINE__, #cond);    \
        }                                                                     \
    } while (0)

static void titre(const char *nom) {
    printf("\n== %s\n", nom);
}

// ---------------------------------------------------------------------------
// Conversion d'images 2 bpp -> 1 bpp
// ---------------------------------------------------------------------------

#define CHARGE 4088
#define DERNIER 1384

// Construit un fragment complet : en-tete de 8 octets en gros-boutiste, puis la charge.
static int fragment(uint8_t *out, uint32_t img_id, uint16_t seq, uint16_t total_frag,
                    uint8_t motif, int charge) {
    out[0] = (uint8_t) (img_id >> 24); out[1] = (uint8_t) (img_id >> 16);
    out[2] = (uint8_t) (img_id >> 8);  out[3] = (uint8_t) img_id;
    out[4] = (uint8_t) (seq >> 8);     out[5] = (uint8_t) seq;
    out[6] = (uint8_t) (total_frag >> 8); out[7] = (uint8_t) total_frag;
    memset(out + 8, motif, (size_t) charge);
    return 8 + charge;
}

static bool envoyer(const uint8_t *frag, int len, int tranche) {
    image_rx_begin();
    for (int i = 0; i < len; i += tranche) {
        int n = (len - i) < tranche ? (len - i) : tranche;
        image_rx_data(frag + i, (uint16_t) n);
    }
    return image_rx_end();
}

static void test_conversion_niveaux(void) {
    titre("Conversion 2 bpp -> 1 bpp : le seuil");

    static uint8_t f[8 + CHARGE];
    image_rx_reset();
    memset(epd_framebuffer, 0xAA, sizeof(epd_framebuffer));

    // 0b11111111 = quatre pixels WHITE -> quatre bits a 1
    fragment(f, 1, 0, 8, 0xFF, CHARGE);
    envoyer(f, 8 + CHARGE, 4096);
    VERIFIE(epd_framebuffer[0] == 0xFF, "WHITE (0b11) doit donner des bits a 1");

    // 0b00000000 = quatre pixels BLACK -> quatre bits a 0
    image_rx_reset();
    fragment(f, 2, 0, 8, 0x00, CHARGE);
    envoyer(f, 8 + CHARGE, 4096);
    VERIFIE(epd_framebuffer[0] == 0x00, "BLACK (0b00) doit donner des bits a 0");

    // 0b10101010 = quatre pixels LIGHT -> au-dessus du seuil, bits a 1
    image_rx_reset();
    fragment(f, 3, 0, 8, 0xAA, CHARGE);
    envoyer(f, 8 + CHARGE, 4096);
    VERIFIE(epd_framebuffer[0] == 0xFF, "LIGHT (0b10) est au-dessus du seuil");

    // 0b01010101 = quatre pixels DARK -> en dessous du seuil, bits a 0
    image_rx_reset();
    fragment(f, 4, 0, 8, 0x55, CHARGE);
    envoyer(f, 8 + CHARGE, 4096);
    VERIFIE(epd_framebuffer[0] == 0x00, "DARK (0b01) est en dessous du seuil");

    // Motif mixte : 0b11000110 = WHITE BLACK DARK LIGHT -> 1 0 0 1
    image_rx_reset();
    fragment(f, 5, 0, 8, 0xC6, CHARGE);
    envoyer(f, 8 + CHARGE, 4096);
    VERIFIE((epd_framebuffer[0] >> 4) == 0x9,
            "0b11000110 doit donner 1001, obtenu %X", epd_framebuffer[0] >> 4);
}

static void test_offsets(void) {
    titre("Conversion : les offsets de destination");

    static uint8_t f[8 + CHARGE];
    image_rx_reset();
    memset(epd_framebuffer, 0x00, sizeof(epd_framebuffer));

    // Le fragment 3 doit atterrir a 3 * 2044, et nulle part ailleurs.
    fragment(f, 10, 3, 8, 0xFF, CHARGE);
    envoyer(f, 8 + CHARGE, 1460);

    VERIFIE(epd_framebuffer[3 * 2044 - 1] == 0x00, "rien avant l'offset du fragment 3");
    VERIFIE(epd_framebuffer[3 * 2044] == 0xFF,     "le fragment 3 commence a 3 * 2044");
    VERIFIE(epd_framebuffer[4 * 2044 - 1] == 0xFF, "et occupe exactement 2044 octets");
    VERIFIE(epd_framebuffer[4 * 2044] == 0x00,     "rien apres");
}

static void test_image_complete(void) {
    titre("Conversion : les 8 fragments couvrent exactement le framebuffer");

    static uint8_t f[8 + CHARGE];
    image_rx_reset();
    memset(epd_framebuffer, 0x00, sizeof(epd_framebuffer));

    bool complete = false;
    for (int seq = 0; seq < 8; seq++) {
        int charge = (seq < 7) ? CHARGE : DERNIER;
        int len = fragment(f, 20, (uint16_t) seq, 8, 0xFF, charge);
        complete = envoyer(f, len, 1460);
    }

    VERIFIE(complete, "l'image est signalee complete au huitieme fragment");

    int ecrits = 0;
    for (int i = 0; i < EPD_BUFFER_SIZE; i++) if (epd_framebuffer[i] == 0xFF) ecrits++;
    VERIFIE(ecrits == EPD_BUFFER_SIZE,
            "les 15000 octets sont ecrits, obtenu %d", ecrits);
}

static void test_tranches_impaires(void) {
    titre("Conversion : le report d'octet impair");

    static uint8_t f[8 + CHARGE];

    // La meme image envoyee en tranches de tailles differentes doit produire des octets
    // IDENTIQUES. C'est le test qui attrape le decalage d'un demi-octet : lwIP livre des
    // tranches de taille arbitraire, et deux octets 2 bpp donnent un octet 1 bpp.
    static uint8_t reference[2044];

    image_rx_reset();
    memset(epd_framebuffer, 0, sizeof(epd_framebuffer));
    fragment(f, 30, 0, 8, 0xC6, CHARGE);
    envoyer(f, 8 + CHARGE, 4096);
    memcpy(reference, epd_framebuffer, sizeof(reference));

    const int tailles[] = {1, 3, 7, 13, 1460, 4095};
    for (unsigned t = 0; t < sizeof(tailles) / sizeof(tailles[0]); t++) {
        image_rx_reset();
        memset(epd_framebuffer, 0, sizeof(epd_framebuffer));
        fragment(f, 31 + t, 0, 8, 0xC6, CHARGE);
        envoyer(f, 8 + CHARGE, tailles[t]);
        VERIFIE(memcmp(epd_framebuffer, reference, sizeof(reference)) == 0,
                "tranches de %d octets : resultat identique au decoupage plein",
                tailles[t]);
    }
}

static void test_perime_et_doublon(void) {
    titre("Conversion : fragments perimes et doublons");

    static uint8_t f[8 + CHARGE];
    image_rx_reset();
    memset(epd_framebuffer, 0x00, sizeof(epd_framebuffer));

    // Image 50 en cours
    fragment(f, 50, 0, 8, 0xFF, CHARGE);
    envoyer(f, 8 + CHARGE, 4096);
    VERIFIE(image_rx_current_id() == 50, "l'image courante est la 50");
    VERIFIE(image_rx_missing() == 7, "il manque 7 fragments");

    // Un fragment d'une image PLUS ANCIENNE doit etre jete sans rien ecrire
    memset(epd_framebuffer + 2044, 0x00, 2044);
    fragment(f, 49, 1, 8, 0xFF, CHARGE);
    envoyer(f, 8 + CHARGE, 4096);
    VERIFIE(epd_framebuffer[2044] == 0x00, "un img_id inferieur est jete");
    VERIFIE(image_rx_missing() == 7, "et ne compte pas comme recu");

    // Le MEME fragment recu deux fois ne compte qu'une fois (QoS 1 = au moins une fois)
    fragment(f, 50, 1, 8, 0xFF, CHARGE);
    envoyer(f, 8 + CHARGE, 4096);
    VERIFIE(image_rx_missing() == 6, "le fragment 1 est compte");
    envoyer(f, 8 + CHARGE, 4096);
    VERIFIE(image_rx_missing() == 6, "un doublon ne recompte pas");
}

static void test_manquants_json(void) {
    titre("Conversion : la liste des fragments manquants");

    static uint8_t f[8 + CHARGE];
    char liste[96];

    image_rx_reset();
    for (int seq = 0; seq < 8; seq++) {
        if (seq == 1 || seq == 4) continue;
        int charge = (seq < 7) ? CHARGE : DERNIER;
        int len = fragment(f, 60, (uint16_t) seq, 8, 0xFF, charge);
        envoyer(f, len, 4096);
    }
    image_rx_missing_json(liste, sizeof(liste));
    VERIFIE(strcmp(liste, "[1,4]") == 0,
            "img_abort doit nommer les manquants, obtenu %s", liste);
}

// ---------------------------------------------------------------------------
// Analyse de la configuration
// ---------------------------------------------------------------------------

static void test_cfg(void) {
    titre("Configuration : analyse du message cfg");

    profiles_clear();

    const char *un = "{\"type\":\"cfg\",\"version\":3,\"profiles\":["
                     "{\"id\":\"aaa\",\"order\":0,\"label\":\"Majorite\",\"holder_binding\":false}]}";
    VERIFIE(profiles_handle_cfg(un, strlen(un)), "un cfg valide est adopte");
    VERIFIE(profiles_count() == 1, "un profil");
    VERIFIE(profiles_version() == 3, "version 3");
    VERIFIE(strcmp(profiles_get(0)->label, "Majorite") == 0, "libelle lu");
    VERIFIE(profiles_get(0)->holder_binding == false, "holder_binding lu");

    // Une version deja connue est ignoree : le serveur republie a chaque connexion.
    const char *vieux = "{\"type\":\"cfg\",\"version\":3,\"profiles\":[]}";
    VERIFIE(!profiles_handle_cfg(vieux, strlen(vieux)), "version <= courante ignoree");
    VERIFIE(profiles_count() == 1, "la configuration precedente survit");

    // Le tri par `order`, meme si le tableau arrive dans le desordre.
    const char *desordre = "{\"type\":\"cfg\",\"version\":4,\"profiles\":["
        "{\"id\":\"c\",\"order\":2,\"label\":\"TROIS\"},"
        "{\"id\":\"a\",\"order\":0,\"label\":\"UN\"},"
        "{\"id\":\"b\",\"order\":1,\"label\":\"DEUX\"}]}";
    VERIFIE(profiles_handle_cfg(desordre, strlen(desordre)), "cfg desordonne adopte");
    VERIFIE(profiles_count() == 3, "trois profils");
    VERIFIE(strcmp(profiles_get(0)->label, "UN") == 0,   "premier = order 0");
    VERIFIE(strcmp(profiles_get(1)->label, "DEUX") == 0, "deuxieme = order 1");
    VERIFIE(strcmp(profiles_get(2)->label, "TROIS") == 0, "troisieme = order 2");

    // Charge utile vide = message retenu efface = revocation : tout oublier.
    VERIFIE(profiles_handle_cfg(NULL, 0), "un message vide est traite");
    VERIFIE(profiles_count() == 0, "plus aucun profil");
    VERIFIE(profiles_version() == -1, "version remise a zero");

    // Un JSON sans version ne doit rien casser.
    const char *sans = "{\"type\":\"cfg\",\"profiles\":[]}";
    VERIFIE(!profiles_handle_cfg(sans, strlen(sans)), "cfg sans version rejete");
}

// ---------------------------------------------------------------------------
// Repli des accents
// ---------------------------------------------------------------------------

static void test_repli_ascii(void) {
    titre("Ecran : repli des libelles sur l'alphabet de la police");

    char out[64];

    epd_text_fold_ascii("Majorite verifiee", out, sizeof(out));
    VERIFIE(strcmp(out, "MAJORITE VERIFIEE") == 0, "mise en majuscules, obtenu %s", out);

    epd_text_fold_ascii("\xC3\xA9\xC3\xA8\xC3\xAA\xC3\xA0\xC3\xA7", out, sizeof(out));
    VERIFIE(strcmp(out, "EEEAC") == 0, "accents replies, obtenu %s", out);

    epd_text_fold_ascii("Age 18+", out, sizeof(out));
    VERIFIE(strcmp(out, "AGE 18 ") == 0, "chiffres gardes, ponctuation blanchie, obtenu %s", out);

    // Troncature : la sortie doit rester terminee, jamais deborder.
    char court[6];
    epd_text_fold_ascii("ABCDEFGHIJ", court, sizeof(court));
    VERIFIE(strlen(court) == 5, "tronque a la capacite, obtenu %zu", strlen(court));

    epd_text_fold_ascii(NULL, out, sizeof(out));
    VERIFIE(out[0] == '\0', "une entree nulle donne une chaine vide");
}

static void test_largeur_texte(void) {
    titre("Ecran : largeur du texte trace");

    // 5 colonnes + 1 d'espacement par glyphe, moins l'espacement final.
    VERIFIE(epd_fb_big_width("A", 1) == 5, "un glyphe a l'echelle 1 fait 5 px");
    VERIFIE(epd_fb_big_width("AB", 1) == 11, "deux glyphes : 5 + 1 + 5");
    VERIFIE(epd_fb_big_width("K7F2M9QX", 7) == 329, "un code de 8 caracteres a l'echelle 7");
    VERIFIE(epd_fb_big_width("K7F2M9QX", 7) < EPD_WIDTH, "et il tient sur la dalle");
    VERIFIE(epd_fb_big_width("", 7) == 0, "chaine vide : largeur nulle");
}

// ---------------------------------------------------------------------------

int main(void) {
    printf("Tests unitaires du firmware EdelCheck\n");

    test_conversion_niveaux();
    test_offsets();
    test_image_complete();
    test_tranches_impaires();
    test_perime_et_doublon();
    test_manquants_json();
    test_cfg();
    test_repli_ascii();
    test_largeur_texte();

    printf("\n%s  %d verification(s), %d echec(s)\n",
           echecs == 0 ? "OK    " : "ECHOUE", total, echecs);
    return echecs == 0 ? 0 : 1;
}
