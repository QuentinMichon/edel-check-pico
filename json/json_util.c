//
// Created by Quentin Michon on 03.08.2026.
//

#include "json_util.h"

#include "storage/storage_manager.h"

// Compare un token frozen (issu de %T) a une chaine litterale.
static bool json_token_eq(const struct json_token *tok, const char *s) {
    return tok->type == JSON_TYPE_STRING && tok->len == (int)strlen(s) &&
        strncmp(tok->ptr, s, (size_t)tok->len) == 0;
}

// Dé-echappe le contenu d'un token string dans un buffer fixe termine par \0.
static void json_token_copy_unescaped(const struct json_token *tok, char *out, size_t out_size) {
    if (out_size == 0) return;
    if (tok->type != JSON_TYPE_STRING) {
        out[0] = '\0';
        return;
    }
    int n = json_unescape(tok->ptr, tok->len, out, (int)out_size - 1);
    if (n < 0) n = 0;
    if ((size_t)n >= out_size) n = (int)out_size - 1;
    out[n] = '\0';
}







void handle_token(const char *body) {
    struct json_token access_token = JSON_INVALID_TOKEN;
    struct json_token token_type = JSON_INVALID_TOKEN;
    struct json_token expires_in = JSON_INVALID_TOKEN;
    struct json_token id_token = JSON_INVALID_TOKEN;

    char token[1024];

    json_scanf(body, (int)strlen(body), "{access_token: %T, token_type: %T, expires_in: %T, id_token: %T}",
        &access_token,
        &token_type,
        &expires_in,
        &id_token
    );

    if (!json_token_eq(&token_type, "Bearer")) {
        printf("[http] --- bad token ---\n");
        return;
    }

    json_token_copy_unescaped(&access_token, token, sizeof(token));

    // stock Bearer token dans le local storage
    put_bearer_token(token);

}

// dessine dans le framebuffer e-paper le QR code contenu dans le corps JSON d'une reponse de
// verification EDEL-ID (qrCodeBitMap.rows : un tableau de chaines de '0'/'1'), coin haut-gauche
// en (x, y) - meme convention que epd_fb_draw_image. scale (1, 2 ou 3, borne sinon) agrandit
// chaque module du QR code en un carre de scale x scale pixels. comme epd_fb_draw_image, ne
// declenche aucun refresh : c'est a l'appelant d'appeler epd_display_update_full/partial ensuite.
void print_qr_code(const char *body, int x, int y, int scale) {
    if (scale < 1) scale = 1;
    if (scale > 3) scale = 3;

    int len = (int) strlen(body);

    for (int row = 0; ; row++) {
        struct json_token line = JSON_INVALID_TOKEN;
        if (json_scanf_array_elem(body, len, ".qrCodeBitMap.rows", row, &line) < 0) {
            break;
        }
        for (int col = 0; col < line.len; col++) {
            bool white = (line.ptr[col] != '1');
            epd_fb_fill_rect(x + col * scale, y + row * scale, scale, scale, white);
        }
    }
}