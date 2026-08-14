//
// Created by Quentin Michon on 03.08.2026.
//

#include "json_util.h"

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

    printf("[http] --- access_token ---\n");
    printf("[http] %s\n", token);
}