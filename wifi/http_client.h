#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stdint.h>
#include <stdbool.h>


// =================================================================
// cible utilisee pour la demo de requete sortante (partie 1)
// =================================================================

#define HTTP_TEST_HOST "example.com"
#define HTTP_TEST_PORT 443 // 0 => port par defaut (80 en http, 443 en https)
#define HTTP_TEST_PATH "/"
#define HTTP_TEST_USE_TLS true
// =================================================================


// effectue une requete GET vers host:port/path (en http simple, ou en https si use_tls est vrai)
// et affiche la reponse brute (entetes + corps, tels que recus) sur la sortie standard. bloque
// jusqu'a la fin de la reponse ou expiration de timeout_ms. port = 0 => port par defaut (80/443).
//
// extra_headers permet d'ajouter nos propres lignes d'entete (ex: "Accept: application/json\r\n"),
// chaque ligne devant se terminer par \r\n. peut valoir NULL si aucun entete supplementaire n'est
// necessaire.
bool http_get(const char *host, uint16_t port, const char *path, bool use_tls,
              const char *extra_headers, uint32_t timeout_ms);

#endif
