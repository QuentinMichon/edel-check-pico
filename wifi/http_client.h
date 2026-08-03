#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stddef.h>
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


// =================================================================
// cible utilisee pour la demo de recuperation de token OAuth2 (client_credentials)
// =================================================================

#define OAUTH_TOKEN_HOST "auth.edel-id.app"
#define OAUTH_TOKEN_PORT 443
#define OAUTH_TOKEN_PATH "/oauth/v2/token"
#define OAUTH_TOKEN_USE_TLS true

// A REMPLACER par les vraies valeurs avant utilisation - ne pas commiter de vrais secrets.
#define OAUTH_CLIENT_ID "ak-378149985188839430-3e17286f"
#define OAUTH_CLIENT_SECRET "RRO67PBH1dFlogl05FhskGiDddiMhJH24eZ8xWp2wmcuK0VhcGuesFzrmNhyfUnr"
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

// effectue une requete POST vers host:port/path et affiche la reponse brute sur la sortie
// standard, meme convention que http_get (port = 0 => port par defaut, extra_headers optionnel,
// bloque jusqu'a la fin de la reponse ou expiration de timeout_ms).
//
// body est envoye tel quel comme corps de la requete (peut valoir NULL, corps vide). content_type
// peut valoir NULL, auquel cas "application/x-www-form-urlencoded" est utilise par defaut (comme
// curl -d). le "Content-Length" est calcule automatiquement a partir de body.
//
// nommee "_oauth2" car pensee pour le flux client_credentials (ex: POST vers /oauth/v2/token
// avec un entete "Authorization: Basic ..." construit via http_client_basic_auth), mais reste
// generique dans ses parametres.
bool http_post_oauth2(const char *host, uint16_t port, const char *path, bool use_tls,
                       const char *content_type, const char *body,
                       const char *extra_headers, uint32_t timeout_ms);

// construit dans out (NUL-terminee) la valeur base64 de "username:password", a utiliser par
// l'appelant pour composer un entete d'authentification HTTP Basic, ex :
//
//   char auth_value[256]; // >= 4*ceil((strlen(id)+1+strlen(secret))/3) + 1 ; toujours verifier le retour
//   if (!http_client_basic_auth(OAUTH_CLIENT_ID, OAUTH_CLIENT_SECRET, auth_value, sizeof(auth_value))) { ... }
//   char headers[320];
//   snprintf(headers, sizeof(headers), "Authorization: Basic %s\r\n", auth_value);
//   http_post_oauth2(..., headers, ...);
//
// retourne false si out_len est insuffisant pour contenir le resultat encode (dans ce cas out
// n'est PAS modifie : ne pas l'utiliser sans avoir verifie la valeur de retour).
bool http_client_basic_auth(const char *username, const char *password, char *out, size_t out_len);

#endif
