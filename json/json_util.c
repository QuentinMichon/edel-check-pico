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

