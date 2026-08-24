/*
 * api_screen - banc de test autonome : le boitier affiche ce que le serveur lui envoie.
 *
 * NE FAIT PARTIE D'AUCUNE FONCTIONNALITE DU PRODUIT. Binaire separe, construit par
 * tests/api_screen/CMakeLists.txt. Il ne modifie rien du firmware existant : il reutilise
 * epd_driver, gpio_driver, wifi_setup, http_client et frozen tels quels.
 *
 * Ce qu'il prouve, en boucle, toutes les API_POLL_MS millisecondes :
 *
 *     Wi-Fi -> DNS -> TLS -> GET -> JSON -> base64 -> RLE -> framebuffer -> ecran
 *
 * C'est deliberement la meme idee que le contrat MQTT - le cloud pousse des pixels deja
 * trames, le boitier n'interprete rien - mais en HTTP et en 1 bit/pixel. Donc testable
 * sans MQTT et sans toucher au driver de l'ecran.
 *
 * Effet de bord utile : le code d'appairage contient des CHIFFRES, que
 * epd_fb_write_typo() ne sait pas dessiner. Ici c'est le serveur qui rend l'image, donc
 * le probleme n'existe pas.
 *
 * Ce banc n'ecrit RIEN en flash : il ne touche pas a storage_manager, donc la
 * configuration persistante de la carte reste intacte.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "mbedtls/base64.h"

#include "gpio/gpio_driver.h"
#include "screen/epd_driver.h"
#include "wifi_setup.h"
#include "http_client.h"
#include "json/frozen.h"

#ifndef WIFI_SSID
#error "WIFI_SSID non defini : relancer cmake avec -DWIFI_SSID=... -DWIFI_PASSWORD=..."
#endif

// --- cible ---------------------------------------------------------------
#ifndef API_HOST
#define API_HOST "edel-check-api-lab.vercel.app"
#endif
#ifndef API_PATH
#define API_PATH "/api/screen"
#endif

#define API_POLL_MS   5000
#define HTTP_TIMEOUT  20000

// Un full refresh tous les N cycles pour purger le ghosting accumule par les partiels.
#define FULL_EVERY    10

/*
 * Buffers STATIQUES, volontairement - la pile du core0 fait 4 Ko (SCRATCH_Y) sur ce chip.
 * Dimensionnement :
 *   json  : ~10,7 Ko observes en RLE, mais le serveur peut retomber sur "raw-b64"
 *           (15000 octets -> 20000 caracteres base64), d'ou 24 Ko.
 *   blob  : le binaire apres base64 ; au pire l'image brute, soit EPD_BUFFER_SIZE.
 */
static char    json_buf[24576];
static uint8_t blob[EPD_BUFFER_SIZE + 8];

/*
 * RLE par octet : suite de couples [compte, valeur], compte de 1 a 255.
 * Meme format que lib/render.js cote serveur. Retourne le nombre d'octets ecrits.
 */
static size_t rle_decode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap) {
    size_t o = 0;
    for (size_t i = 0; i + 1 < in_len; i += 2) {
        int n = in[i];
        uint8_t v = in[i + 1];
        for (int k = 0; k < n && o < out_cap; k++) {
            out[o++] = v;
        }
    }
    return o;
}

// Affiche un message court a l'ecran. ATTENTION : epd_fb_write_typo ne connait que A-Z
// et l'apostrophe - pas de chiffres, pas d'accents.
static void screen_message(char *l1, char *l2) {
    epd_fb_clear(true);
    epd_fb_write_typo(20, 120, l1);
    if (l2) epd_fb_write_typo(20, 160, l2);
    epd_display_update_full();
}

/*
 * Un cycle complet. Retourne true si l'ecran a ete rafraichi avec une image du serveur.
 */
static bool fetch_and_display(int cycle) {
    // ---- 1. requete -----------------------------------------------------
    // "Accept-Encoding: identity" est OBLIGATOIRE : le firmware sait de-chunker mais PAS
    // de-gzipper, et Vercel compresse des qu'on le laisse choisir. Sans cet en-tete, on
    // recoit du binaire gzip qui ressemble a du JSON corrompu.
    uint32_t t0 = to_ms_since_boot(get_absolute_time());

    bool ok = http_get(API_HOST, 443, API_PATH, true,
                       "Accept-Encoding: identity\r\n"
                       "Accept: application/json\r\n",
                       HTTP_TIMEOUT, json_buf, sizeof(json_buf));

    uint32_t t_http = to_ms_since_boot(get_absolute_time()) - t0;

    if (!ok) {
        printf("[api] echec de la requete HTTP\n");
        return false;
    }

    size_t json_len = strlen(json_buf);
    printf("[api] HTTP ok : %u octets en %u ms\n", (unsigned) json_len, t_http);

    // ---- 2. JSON --------------------------------------------------------
    // %T donne un pointeur DANS json_buf : aucune copie du base64 (10 Ko) n'a lieu.
    struct json_token enc = JSON_INVALID_TOKEN;
    struct json_token data = JSON_INVALID_TOKEN;
    struct json_token code = JSON_INVALID_TOKEN;
    int raw_len = 0;

    json_scanf(json_buf, (int) json_len,
               "{enc: %T, raw_len: %d, data: %T, code: %T}",
               &enc, &raw_len, &data, &code);

    if (data.len <= 0 || raw_len != EPD_BUFFER_SIZE) {
        printf("[api] JSON inattendu (data=%d, raw_len=%d, attendu %d)\n",
               data.len, raw_len, EPD_BUFFER_SIZE);
        return false;
    }

    printf("[api] code = %.*s | enc = %.*s | base64 = %d car.\n",
           code.len, code.ptr, enc.len, enc.ptr, data.len);

    // ---- 3. base64 ------------------------------------------------------
    size_t blob_len = 0;
    int rc = mbedtls_base64_decode(blob, sizeof(blob), &blob_len,
                                   (const unsigned char *) data.ptr, (size_t) data.len);
    if (rc != 0) {
        printf("[api] echec du decodage base64 (mbedtls %d)\n", rc);
        return false;
    }

    // ---- 4. RLE (ou copie directe) --------------------------------------
    bool is_rle = (enc.len == 7 && strncmp(enc.ptr, "rle-b64", 7) == 0);

    size_t written;
    if (is_rle) {
        written = rle_decode(blob, blob_len, epd_framebuffer, EPD_BUFFER_SIZE);
    } else {
        written = blob_len < EPD_BUFFER_SIZE ? blob_len : EPD_BUFFER_SIZE;
        memcpy(epd_framebuffer, blob, written);
    }

    printf("[api] base64 -> %u octets -> framebuffer %u/%d octets\n",
           (unsigned) blob_len, (unsigned) written, EPD_BUFFER_SIZE);

    if (written != EPD_BUFFER_SIZE) {
        printf("[api] ATTENTION : image incomplete, affichage quand meme\n");
    }

    // ---- 5. affichage ---------------------------------------------------
    // Partiel par defaut (rapide, pas de flash), full periodiquement pour purger le
    // ghosting que les partiels accumulent.
    uint32_t t1 = to_ms_since_boot(get_absolute_time());
    bool full = (cycle % FULL_EVERY) == 1;
    if (full) {
        epd_display_update_full();
    } else {
        epd_display_update_partial();
    }
    printf("[api] refresh %s : %u ms | BUSY au repos = %d\n",
           full ? "complet" : "partiel",
           to_ms_since_boot(get_absolute_time()) - t1,
           gpio_get(PIN_BUSY));

    return true;
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    printf("\n\n==========================================================\n");
    printf("   EDEL CHECK - banc de test : ecran pilote par l'API\n");
    printf("   cible   : https://%s%s\n", API_HOST, API_PATH);
    printf("   cadence : %d ms\n", API_POLL_MS);
    printf("==========================================================\n\n");

    init_gpio();
    epd_init();

    screen_message("connexion wifi", WIFI_SSID);

    if (!wifi_init()) {
        printf("[main] wifi_init a echoue\n");
        screen_message("erreur wifi", "init");
        while (true) tight_loop_contents();
    }

    if (!wifi_connect(WIFI_SSID, WIFI_PASSWORD, 30000)) {
        printf("[main] connexion wifi impossible\n");
        // Le boitier reste sur cet ecran : c'est la difference avec le firmware
        // principal, qui fait return -1 et laisse une carte muette.
        screen_message("erreur wifi", "pas de reseau");
        while (true) tight_loop_contents();
    }

    screen_message("connecte", "attente serveur");

    int cycle = 0, ok = 0, ko = 0;
    while (true) {
        cycle++;
        printf("\n--- cycle %d (%d ok / %d echecs) ---\n", cycle, ok, ko);

        if (fetch_and_display(cycle)) {
            ok++;
        } else {
            ko++;
            // On ne repeint pas l'ecran sur echec : il garde la derniere image valide,
            // ce qui evite de clignoter a chaque coupure reseau passagere.
            printf("[api] cycle en echec, on garde l'image precedente\n");
        }

        sleep_ms(API_POLL_MS);
    }
}
