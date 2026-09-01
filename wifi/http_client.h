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


// Les constantes OAUTH_* et VERIFY_CH_* ont ete retirees le 24.08.2026.
//
// Elles servaient a un chemin ou le BOITIER appelait directement l'API Edel-ID : il devait
// donc detenir le CLIENT_SECRET de l'organisation, qui etait ecrit en clair ici, sur un
// depot avec un remote GitHub. Un boitier vole sur un comptoir donnait alors acces a l'API
// de verification du client entier.
//
// Le boitier ne parle plus qu'a device-service, en MQTT, avec un secret qui lui est propre
// et revocable d'un UPDATE. Le secret d'organisation vit desormais dans
// edelcheck/.env, hors depot, lu par device-service seul.
//
// ⚠ L'ancienne valeur reste dans l'historique git : elle DOIT etre revoquee et regeneree.


// effectue une requete GET vers host:port/path (en http simple, ou en https si use_tls est vrai)
// et affiche la reponse brute (entetes + corps, tels que recus) sur la sortie standard. bloque
// jusqu'a la fin de la reponse ou expiration de timeout_ms. port = 0 => port par defaut (80/443).
//
// extra_headers permet d'ajouter nos propres lignes d'entete (ex: "Accept: application/json\r\n"),
// chaque ligne devant se terminer par \r\n. peut valoir NULL si aucun entete supplementaire n'est
// necessaire.
//
// out_body/out_body_len : buffer optionnel qui recoit le corps de la reponse (sans les entetes
// HTTP), NUL-termine. peut valoir NULL/0 pour ne pas capturer (seul l'affichage sur stdout a
// alors lieu, comme avant). si out_body_len est insuffisant, le corps est tronque et un warning
// est affiche sur stdout - la requete elle-meme n'est pas consideree en echec pour autant.
// limitation : suppose un corps delimite par Content-Length ou par la fermeture de connexion (pas
// de support de Transfer-Encoding: chunked).
bool http_get(const char *host, uint16_t port, const char *path, bool use_tls,
              const char *extra_headers, uint32_t timeout_ms,
              char *out_body, size_t out_body_len);

// effectue une requete POST vers host:port/path et affiche la reponse brute sur la sortie
// standard, meme convention que http_get (port = 0 => port par defaut, extra_headers optionnel,
// bloque jusqu'a la fin de la reponse ou expiration de timeout_ms).
//
// body est envoye tel quel comme corps de la requete (peut valoir NULL, corps vide). content_type
// peut valoir NULL, auquel cas "application/x-www-form-urlencoded" est utilise par defaut (comme
// curl -d). le "Content-Length" est calcule automatiquement a partir de body.
//
// out_body/out_body_len : voir http_get (meme convention - corps seul, NUL-termine, NULL/0 pour
// ne pas capturer).
//
// generique : extra_headers accepte n'importe quel en-tete, dont une authentification.
bool http_post(const char *host, uint16_t port, const char *path, bool use_tls,
                const char *content_type, const char *body,
                const char *extra_headers, uint32_t timeout_ms,
                char *out_body, size_t out_body_len);

// code de statut HTTP de la DERNIERE requete terminee (200, 202, 410...), 0 si inconnu.
//
// A lire immediatement apres http_get/http_post : le client est bloquant et sequentiel,
// donc la valeur se rapporte sans ambiguite a l'appel qui vient de rendre la main. Elle
// n'est renseignee que si un buffer de capture (out_body) a ete fourni.
//
// Necessaire des qu'un flux distingue plusieurs reponses de succes ou d'echec - l'appairage
// traite 202 (patienter), 200 (secret livre) et 410 (definitif) de trois facons differentes,
// et se tromper fait passer un boitier en SUSPECT.
int http_client_last_status(void);

#endif
