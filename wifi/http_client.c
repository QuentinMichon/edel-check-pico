#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

// altcp = "abstract tcp" : une couche lwIP qui a exactement les memes fonctions que l'api tcp_*
// (altcp_connect, altcp_write, altcp_recv, ...), mais qui peut transporter du TCP nu OU du TCP+TLS
// de facon transparente pour le code qui l'utilise. c'est la brique sur laquelle httpc_get_file_dns
// est lui-meme construit ; en l'utilisant directement, on garde le TLS "gratuit" (gere par lwIP +
// mbedTLS) tout en ecrivant nous-memes la requete HTTP (donc en controlant nos propres entetes).
#include "lwip/altcp.h"
#include "lwip/pbuf.h"
#include "lwip/dns.h"
#include "lwip/altcp_tls.h"
#include "mbedtls/base64.h"
#include "http_client.h"
#include "ca_letsencrypt.h"

#define POLL_TIME_S 10

// mbedtls (via MBEDTLS_PLATFORM_MS_TIME_ALT, voir mbedtls_config.h) a besoin qu'on lui fournisse
// une horloge monotone en millisecondes : on branche ca sur l'horloge du pico.
int64_t mbedtls_ms_time(void) {
    return (int64_t) to_ms_since_boot(get_absolute_time());
}

// tout l'etat d'une requete en cours. un seul pointeur de ce type (arg) est passe a chaque
// callback lwIP : c'est le seul moyen pour ces callbacks (appelees par lwIP, pas par nous) de
// retrouver "de quelle requete" on parle.
typedef struct {
    struct altcp_pcb *pcb;   // la "connexion" (tcp nu, ou tcp+tls selon use_tls)
    ip_addr_t remote_addr;   // ip du serveur, remplie par la resolution dns
    uint16_t port;
    char request[2048];      // notre requete HTTP construite a la main (avec nos propres entetes)
                              // taille dimensionnee pour un Authorization: Bearer <JWT> qui peut
                              // approcher MEM_BEARER_TOKEN_SIZE (800, voir storage_manager.h)
    bool dns_done;
    volatile bool complete;  // passe a true par http_client_result() quand tout est termine
    int result;              // 0 = ok, != 0 = erreur (voir http_client_result)
    struct altcp_tls_config *tls_config;

    char *out_body;           // buffer appelant recevant le corps de la reponse (NULL = pas de capture)
    size_t out_body_len;      // capacite de out_body (NUL final inclus)
    size_t out_body_used;     // nb d'octets de corps deja copies dans out_body
    uint8_t header_match;     // etat de l'automate de detection de "\r\n\r\n" (0..4)
    bool out_body_truncated;  // vrai si le corps recu depassait out_body_len

    char header_buf[1024];    // accumulation des entetes bruts, pour y chercher Transfer-Encoding
    size_t header_buf_len;    // nb d'octets accumules dans header_buf (borne a sizeof(header_buf))
    bool chunked;             // vrai si "Transfer-Encoding: chunked" detecte dans les entetes
    uint8_t chunk_state;      // etat de l'automate de dechunking (voir enum http_chunk_state_t)
    size_t chunk_remaining;   // taille du chunk courant restant a copier (ou accumulateur hexa)
} http_client_state_t;

// etats de l'automate de dechunking RFC 7230 (Transfer-Encoding: chunked)
enum http_chunk_state_t {
    HTTP_CHUNK_SIZE = 0,  // lecture des chiffres hexa de la taille du chunk (extensions ignorees)
    HTTP_CHUNK_SIZE_LF,   // "\r"vu, attente du"\n" terminant la ligne de taille
    HTTP_CHUNK_DATA,      // copie de chunk_remaining octets de donnees dans out_body
    HTTP_CHUNK_DATA_CR,   // donnees terminees, attente du "\r" qui les suit
    HTTP_CHUNK_DATA_LF,   // "\r"vu, attente du"\n" separant ce chunk du suivant
    HTTP_CHUNK_DONE,      // chunk terminal (taille 0) atteint : corps termine, reste ignore
};

// ferme proprement la connexion et detache nos callbacks (sinon lwIP pourrait rappeler une
// fonction sur un pcb en cours de fermeture / deja libere).
static err_t http_client_close(http_client_state_t *state) {
    err_t err = ERR_OK;
    if (state->pcb != NULL) {
        altcp_arg(state->pcb, NULL);
        altcp_poll(state->pcb, NULL, 0);
        altcp_recv(state->pcb, NULL);
        altcp_err(state->pcb, NULL);
        err = altcp_close(state->pcb);
        if (err != ERR_OK) {
            altcp_abort(state->pcb);
            err = ERR_ABRT;
        }
        state->pcb = NULL;
    }
    return err;
}

// point de sortie commun (succes, erreur ou timeout) : enregistre le resultat et reveille la
// boucle d'attente de http_get() en passant complete a true.
static err_t http_client_result(http_client_state_t *state, int result) {
    state->result = result;
    state->complete = true;
    return http_client_close(state);
}

// recherche insensible a la casse de needle dans haystack (haystack de longueur connue, pas
// forcement NUL-termine sur toute sa capacite). utilise pour reperer "transfer-encoding: chunked"
// quel que soit le style de casse employe par le serveur.
static bool http_client_header_contains_ci(const char *haystack, size_t haystack_len, const char *needle) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0 || haystack_len < needle_len) {
        return false;
    }
    for (size_t i = 0; i + needle_len <= haystack_len; i++) {
        size_t j = 0;
        for (; j < needle_len; j++) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char) (a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char) (b - 'A' + 'a');
            if (a != b) break;
        }
        if (j == needle_len) {
            return true;
        }
    }
    return false;
}

// avance l'automate de detection de "\r\n\r\n" au fil des octets recus (le flux peut arriver
// decoupe n'importe ou, y compris au milieu du separateur, d'ou cet etat conserve dans state
// entre deux appels), en accumulant au passage les entetes dans state->header_buf (borne, pour y
// chercher Transfer-Encoding une fois les entetes termines). retourne le nombre d'octets de data
// consommes comme entetes ; le reste (data[retour..len)) appartient deja au corps.
//
// note : "\r\n\r\n" n'a pas de prefixe qui soit aussi un suffixe plus court (hormis lui-meme),
// donc un simple redemarrage sur SEP[0] en cas d'echec suffit ici (pas besoin d'un vrai KMP).
static uint16_t http_client_scan_headers(http_client_state_t *state, const uint8_t *data, uint16_t len) {
    static const char SEP[4] = {'\r', '\n', '\r', '\n'};

    if (state->header_match >= 4) {
        return 0; // deja passe les entetes, tout ce morceau appartient au corps
    }

    uint16_t i = 0;
    while (state->header_match < 4 && i < len) {
        if (state->header_buf_len + 1 < sizeof(state->header_buf)) {
            state->header_buf[state->header_buf_len++] = (char) data[i];
        }
        if (data[i] == (uint8_t) SEP[state->header_match]) {
            state->header_match++;
        } else {
            state->header_match = (data[i] == (uint8_t) SEP[0]) ? 1 : 0;
        }
        i++;
    }

    if (state->header_match == 4) {
        state->header_buf[state->header_buf_len] = '\0';
        state->chunked = http_client_header_contains_ci(state->header_buf, state->header_buf_len,
                                                          "transfer-encoding: chunked");
    }

    return i;
}

// copie brute bornee dans out_body (cas Content-Length, ou corps delimite par la fermeture de
// connexion). borne a out_body_len - 1, et NUL-termine a chaque appel.
static void http_client_copy_body_raw(http_client_state_t *state, const uint8_t *data, size_t len) {
    size_t remaining_cap = (state->out_body_len > state->out_body_used + 1)
                                ? state->out_body_len - state->out_body_used - 1 : 0;
    size_t to_copy = (len > remaining_cap) ? remaining_cap : len;
    if (to_copy < len) {
        state->out_body_truncated = true;
    }
    if (to_copy > 0) {
        memcpy(state->out_body + state->out_body_used, data, to_copy);
        state->out_body_used += to_copy;
    }
    if (state->out_body_len > 0) {
        state->out_body[state->out_body_used] = '\0';
    }
}

// decode un flux Transfer-Encoding: chunked (RFC 7230 4.1) au fil des octets recus, et ne copie
// dans out_body que les donnees de chunk (jamais les lignes de taille ni les CRLF de separation).
// l'etat (chunk_state/chunk_remaining) est conserve dans state entre deux appels, puisqu'une
// ligne de taille ou un chunk de donnees peut tres bien etre coupe entre deux morceaux TCP.
static void http_client_copy_body_chunked(http_client_state_t *state, const uint8_t *data, size_t len) {
    size_t i = 0;
    while (i < len && state->chunk_state != HTTP_CHUNK_DONE) {
        uint8_t c = data[i];
        switch (state->chunk_state) {
            case HTTP_CHUNK_SIZE: {
                int hex;
                if (c >= '0' && c <= '9') hex = c - '0';
                else if (c >= 'a' && c <= 'f') hex = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') hex = c - 'A' + 10;
                else hex = -1;

                if (hex >= 0) {
                    state->chunk_remaining = state->chunk_remaining * 16 + (size_t) hex;
                } else if (c == '\r') {
                    state->chunk_state = HTTP_CHUNK_SIZE_LF;
                }
                // tout autre caractere (ex: extension";foo=bar") est simplement ignore
                i++;
                break;
            }
            case HTTP_CHUNK_SIZE_LF:
                state->chunk_state = (state->chunk_remaining == 0) ? HTTP_CHUNK_DONE : HTTP_CHUNK_DATA;
                i++;
                break;
            case HTTP_CHUNK_DATA: {
                size_t remaining_cap = (state->out_body_len > state->out_body_used + 1)
                                            ? state->out_body_len - state->out_body_used - 1 : 0;
                size_t available = len - i;
                size_t chunk_avail = (available < state->chunk_remaining) ? available : state->chunk_remaining;
                size_t to_copy = (chunk_avail > remaining_cap) ? remaining_cap : chunk_avail;
                if (to_copy < chunk_avail) {
                    state->out_body_truncated = true;
                }
                if (to_copy > 0) {
                    memcpy(state->out_body + state->out_body_used, data + i, to_copy);
                    state->out_body_used += to_copy;
                }
                // on avance toujours du nombre d'octets de DONNEES consommes (meme si to_copy est
                // plus petit faute de place) : il faut continuer a suivre le flux chunked
                // correctement, juste sans copier le surplus dans out_body.
                i += chunk_avail;
                state->chunk_remaining -= chunk_avail;
                if (state->chunk_remaining == 0) {
                    state->chunk_state = HTTP_CHUNK_DATA_CR;
                }
                break;
            }
            case HTTP_CHUNK_DATA_CR:
                state->chunk_state = HTTP_CHUNK_DATA_LF;
                i++;
                break;
            case HTTP_CHUNK_DATA_LF:
                state->chunk_state = HTTP_CHUNK_SIZE;
                state->chunk_remaining = 0; // reinitialise l'accumulateur pour la prochaine taille
                i++;
                break;
            default:
                i = len;
                break;
        }
    }
    if (state->out_body_len > 0) {
        state->out_body[state->out_body_used] = '\0';
    }
}

// point d'entree de la capture du corps : avance d'abord le scan des entetes (et la detection de
// Transfer-Encoding), puis dispatche le reste du morceau vers le decodeur chunked ou la copie
// brute selon ce qui a ete detecte.
static void http_client_capture_body(http_client_state_t *state, const uint8_t *data, uint16_t len) {
    uint16_t consumed = http_client_scan_headers(state, data, len);
    if (consumed >= len) {
        return; // tout ce morceau appartenait aux entetes
    }

    if (state->chunked) {
        http_client_copy_body_chunked(state, data + consumed, (size_t) (len - consumed));
    } else {
        http_client_copy_body_raw(state, data + consumed, (size_t) (len - consumed));
    }
}

// appelee par lwIP a chaque fois qu'un morceau de reponse arrive. on affiche le flux brut tel
// qu'il arrive sur le fil (comportement inchange), et, si l'appelant a fourni un buffer de
// capture (state->out_body), on en extrait en parallele le corps de la reponse (sans les entetes)
// via http_client_capture_body.
static err_t http_client_recv(void *arg, struct altcp_pcb *conn, struct pbuf *p, err_t err) {
    http_client_state_t *state = (http_client_state_t *) arg;
    (void) err;
    if (!p) {
        // p == NULL signifie que le serveur a ferme la connexion : reponse complete
        return http_client_result(state, 0);
    }

    cyw43_arch_lwip_check();
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        fwrite(q->payload, 1, q->len, stdout);
        if (state->out_body) {
            http_client_capture_body(state, (const uint8_t *) q->payload, q->len);
        }
    }
    // dit a lwIP "j'ai fini de lire ces octets" : necessaire pour que le controle de flux TCP
    // (la fenetre de reception) reste ouvert et que le serveur puisse continuer a envoyer.
    altcp_recved(conn, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

// filet de securite : si aucun octet n'arrive pendant POLL_TIME_S*2 (l'unite de altcp_poll est
// 0.5s), on considere que la connexion est morte et on abandonne plutot que d'attendre indefiniment.
static err_t http_client_poll(void *arg, struct altcp_pcb *conn) {
    (void) conn;
    return http_client_result((http_client_state_t *) arg, -1);
}

static void http_client_err(void *arg, err_t err) {
    // a ce stade lwIP a deja libere le pcb lui-meme : on met juste a jour notre etat, sans
    // rappeler close/abort dessus.
    http_client_state_t *state = (http_client_state_t *) arg;
    state->pcb = NULL;
    printf("\n[http] erreur de connexion (%d)\n", err);
    state->result = err;
    state->complete = true;
}

// appelee une seule fois, quand la connexion (tcp, ou tcp+tls) est etablie : c'est le bon moment
// pour envoyer notre requete, puisque le "canal" est pret (le handshake tls, s'il y en a un,
// est deja termine a ce stade - altcp nous le cache completement).
static err_t http_client_connected(void *arg, struct altcp_pcb *conn, err_t err) {
    http_client_state_t *state = (http_client_state_t *) arg;
    if (err != ERR_OK) {
        return http_client_result(state, err);
    }

    err_t werr = altcp_write(conn, state->request, (u16_t) strlen(state->request), TCP_WRITE_FLAG_COPY);
    if (werr != ERR_OK) {
        return http_client_result(state, werr);
    }
    return altcp_output(conn);
}

// cree le pcb altcp adapte : avec TLS (mbedTLS) si use_tls, ou tcp nu sinon. le reste du code
// (callbacks, ecriture, lecture...) est rigoureusement identique dans les deux cas - c'est
// exactement l'interet de la couche altcp.
static bool http_client_open(http_client_state_t *state, bool use_tls, const char *host) {
    u8_t ip_type = IP_GET_TYPE(&state->remote_addr);

    if (use_tls) {
        state->pcb = altcp_tls_new(state->tls_config, ip_type);
        if (state->pcb) {
            // SNI (server name indication) : indique au serveur quel certificat/domaine on veut
            // joindre, indispensable quand plusieurs sites https partagent la meme adresse ip.
            mbedtls_ssl_set_hostname(altcp_tls_context(state->pcb), host);
        }
    } else {
        state->pcb = altcp_new_ip_type(NULL, ip_type);
    }

    if (!state->pcb) {
        return false;
    }

    altcp_arg(state->pcb, state);
    altcp_poll(state->pcb, http_client_poll, POLL_TIME_S * 2);
    altcp_recv(state->pcb, http_client_recv);
    altcp_err(state->pcb, http_client_err);

    // les appels lwIP faits depuis le contexte "main" (hors callback lwIP) doivent etre encadres
    // par cyw43_arch_lwip_begin/end pour garantir le bon verrouillage face a l'irq d'arriere-plan.
    cyw43_arch_lwip_begin();
    err_t err = altcp_connect(state->pcb, &state->remote_addr, state->port, http_client_connected);
    cyw43_arch_lwip_end();
    return err == ERR_OK;
}

// callback de resolution dns (voir dns_gethostbyname plus bas).
static void http_client_dns_found(const char *name, const ip_addr_t *ipaddr, void *arg) {
    http_client_state_t *state = (http_client_state_t *) arg;
    (void) name;
    if (ipaddr) {
        state->remote_addr = *ipaddr;
    }
    state->dns_done = true;
}







// alloue et initialise l'etat commun a toute requete (GET ou POST) : creation de la config tls
// si necessaire, et port par defaut (443/80) si port == 0. *port est mis a jour avec le port
// effectivement retenu. NULL en cas d'echec (deja nettoye).
//
// out_body/out_body_len : buffer optionnel (NULL = pas de capture) qui recevra le corps de la
// reponse (voir http_client_capture_body). out_body est immediatement mis a "" pour garantir une
// chaine valide meme si la requete echoue avant de recevoir la moindre donnee.
static http_client_state_t *http_client_alloc(bool use_tls, uint16_t *port,
                                               char *out_body, size_t out_body_len) {
    http_client_state_t *state = calloc(1, sizeof(http_client_state_t));
    if (!state) {
        printf("[http] allocation echouee\n");
        return NULL;
    }

    if (use_tls) {
        // L'autorite est OBLIGATOIRE ici, pas facultative : lwipopts.h impose
        // ALTCP_MBEDTLS_AUTHMODE = MBEDTLS_SSL_VERIFY_REQUIRED a TOUTES les connexions
        // TLS. Avec NULL, la verification ne peut pas aboutir et la poignee de main est
        // interrompue -- lwIP rend ERR_CLSD (-15), sans rien dire de plus.
        //
        // Combine a mbedtls_ssl_set_hostname() appele plus bas, on obtient une
        // verification complete : chaine ET nom d'hote.
        state->tls_config = altcp_tls_create_config_client(
                (const u8_t *) EDEL_CA_LETSENCRYPT, EDEL_CA_LETSENCRYPT_LEN);
        if (!state->tls_config) {
            printf("[http] echec de creation de la config tls\n");
            free(state);
            return NULL;
        }
        if (*port == 0) *port = 443;
    } else if (*port == 0) {
        *port = 80;
    }
    state->port = *port;

    state->out_body = out_body;
    state->out_body_len = out_body_len;
    if (out_body && out_body_len > 0) {
        out_body[0] = '\0';
    }

    return state;
}

// partie commune a toute requete, une fois state->request deja rempli : resolution dns,
// connexion tcp/tls, attente bloquante de la reponse, puis nettoyage. libere state dans tous
// les cas (succes ou echec).
// Code de statut de la DERNIERE requete terminee (0 = inconnu).
//
// Le client est bloquant et sequentiel : une requete est entierement terminee quand
// http_get/http_post rend la main, donc lire cette valeur juste apres l'appel est bien
// defini. Elle n'est renseignee que si l'appelant a fourni un buffer de capture, seul cas
// ou les entetes sont accumules.
//
// Distinguer 200 / 202 / 410 est indispensable a l'appairage : le contrat traite un 410
// comme definitif (appairage expire, ou secret deja delivre) alors qu'une panne passagere
// doit etre retentee. Confondre les deux fait passer un boitier en SUSPECT.
static int g_last_status = 0;

int http_client_last_status(void) {
    return g_last_status;
}

// extrait NNN de "HTTP/1.1 NNN ...", 0 si la ligne est absente ou malformee
static int http_client_parse_status(const char *headers) {
    if (strncmp(headers, "HTTP/", 5) != 0) {
        return 0;
    }
    const char *space = strchr(headers, ' ');
    if (space == NULL) {
        return 0;
    }
    return (int) strtol(space + 1, NULL, 10);
}

static bool http_client_run(http_client_state_t *state, const char *host, bool use_tls, uint32_t timeout_ms) {
    g_last_status = 0;

    // 1) RESOLUTION DNS =============================================================

    printf("[http] resolution DNS de \"%s\"...\n", host);

    // section critique (sans irq)
    cyw43_arch_lwip_begin();
    err_t err = dns_gethostbyname(host, &state->remote_addr, http_client_dns_found, state);
    cyw43_arch_lwip_end();

    if (err == ERR_OK) {
        // deja en cache, ou hote fourni sous forme d'adresse ip litterale
        state->dns_done = true;
    } else if (err != ERR_INPROGRESS) {
        printf("[http] echec de la resolution DNS (code %d)\n", err);
        if (state->tls_config) {
            altcp_tls_free_config(state->tls_config);
        }
        free(state);
        return false;
    }

    // dns_gethostbyname() est non bloquante : on attend que http_client_dns_found() ait ete
    // appelee par l'irq d'arriere-plan avant de continuer.
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!state->dns_done) {
        if (time_reached(deadline)) {
            printf("[http] timeout de resolution DNS\n");
            if (state->tls_config) {
                altcp_tls_free_config(state->tls_config);
            }
            free(state);
            return false;
        }
        sleep_ms(10);
    }

    if (ip_addr_isany(&state->remote_addr)) {
        printf("[http] hote introuvable\n");
        if (state->tls_config) {
            altcp_tls_free_config(state->tls_config);
        }
        free(state);
        return false;
    }

    // 2) CONNEXION TCP / TLS =============================================================

    printf("[http] connexion %s a %s:%u...\n", use_tls ? "tls" : "tcp", ip4addr_ntoa(&state->remote_addr), state->port);

    if (!http_client_open(state, use_tls, host)) {
        printf("[http] echec de connexion\n");
        if (state->tls_config) {
            altcp_tls_free_config(state->tls_config);
        }
        free(state);
        return false;
    }

    printf("[http] --- reponse ---\n");
    // meme principe que pour le dns : altcp_connect() est non bloquante, on attend que les
    // callbacks (recv/result) fassent leur travail et signalent la fin via state->complete.
    deadline = make_timeout_time_ms(timeout_ms);
    while (!state->complete) {
        if (time_reached(deadline)) {
            printf("\n[http] timeout de la requete\n");
            http_client_result(state, -1);
            break;
        }
        sleep_ms(10);
    }

    if (state->tls_config) {
        altcp_tls_free_config(state->tls_config);
    }

    if (state->out_body && state->out_body_truncated) {
        printf("\n[http] attention: payload tronque (buffer de sortie trop petit)\n");
    }

    if (state->header_buf_len > 0) {
        state->header_buf[state->header_buf_len < sizeof(state->header_buf)
                              ? state->header_buf_len : sizeof(state->header_buf) - 1] = '\0';
        g_last_status = http_client_parse_status(state->header_buf);
    }

    bool ok = (state->result == 0);
    printf("\n[http] --- fin (resultat = %d, statut = %d) ---\n", state->result, g_last_status);
    free(state);
    return ok;
}




/*
 *      --- GET --------------------
 */

bool http_get(const char *host, uint16_t port, const char *path, bool use_tls,
              const char *extra_headers, uint32_t timeout_ms,
              char *out_body, size_t out_body_len) {
    http_client_state_t *state = http_client_alloc(use_tls, &port, out_body, out_body_len);
    if (!state) {
        return false;
    }

    // on construit nous-memes la requete texte : c'est ici, et uniquement ici, qu'on decide des
    // entetes envoyes - contrairement a httpc_get_file_dns qui imposait "Accept: */*" en dur.
    int n = snprintf(state->request, sizeof(state->request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: pico2w-lab05\r\n"
             "Connection: close\r\n"
             "%s" // entetes additionnels fournis par l'appelant (deja termines par \r\n chacun)
             "\r\n",
             path, host, extra_headers ? extra_headers : "");
    if (n < 0 || (size_t) n >= sizeof(state->request)) {
        printf("[http] requete GET trop longue pour le buffer\n");
        if (state->tls_config) {
            altcp_tls_free_config(state->tls_config);
        }
        free(state);
        return false;
    }

    return http_client_run(state, host, use_tls, timeout_ms);
}




/*
 *      --- POST --------------------
 */

bool http_post(const char *host, uint16_t port, const char *path, bool use_tls,
                const char *content_type, const char *body,
                const char *extra_headers, uint32_t timeout_ms,
                char *out_body, size_t out_body_len) {
    http_client_state_t *state = http_client_alloc(use_tls, &port, out_body, out_body_len);
    if (!state) {
        return false;
    }

    size_t body_len = body ? strlen(body) : 0;

    int n = snprintf(state->request, sizeof(state->request),
             "POST %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: pico2w-lab05\r\n"
             "Connection: close\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "%s" // entetes additionnels fournis par l'appelant (deja termines par \r\n chacun)
             "\r\n"
             "%s",
             path, host,
             content_type ? content_type : "application/x-www-form-urlencoded",
             body_len,
             extra_headers ? extra_headers : "",
             body ? body : "");
    // Content-Length doit correspondre exactement au corps effectivement envoye : une requete
    // tronquee par snprintf serait un bug silencieux (le serveur attendrait plus d'octets que
    // ce qu'on lui a reellement envoye), d'ou ce controle explicite du retour de snprintf.
    if (n < 0 || (size_t) n >= sizeof(state->request)) {
        printf("[http] requete POST trop longue pour le buffer\n");
        if (state->tls_config) {
            altcp_tls_free_config(state->tls_config);
        }
        free(state);
        return false;
    }

    return http_client_run(state, host, use_tls, timeout_ms);
}




/*
 *      --- AUTH BASIC --------------------
 */

