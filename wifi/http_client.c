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
#include "http_client.h"

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
    char request[384];       // notre requete HTTP construite a la main (avec nos propres entetes)
    bool dns_done;
    volatile bool complete;  // passe a true par http_client_result() quand tout est termine
    int result;              // 0 = ok, != 0 = erreur (voir http_client_result)
    struct altcp_tls_config *tls_config;
} http_client_state_t;

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

// appelee par lwIP a chaque fois qu'un morceau de reponse arrive. contrairement a la version
// "httpc", on ne separe pas entetes/corps ici : on recoit le flux d'octets brut, tel qu'il arrive
// sur le fil, et on l'affiche tel quel (c'est a nous de reperer "\r\n\r\n" si on veut distinguer
// les entetes du corps).
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







/*
 *      --- GET --------------------
 */

bool http_get(const char *host, uint16_t port, const char *path, bool use_tls,
              const char *extra_headers, uint32_t timeout_ms) {
    http_client_state_t *state = calloc(1, sizeof(http_client_state_t));
    if (!state) {
        printf("[http] allocation echouee\n");
        return false;
    }

    if (use_tls) {
        // NULL, 0 = pas de certificat client, pas de CA custom (verification optionnelle,
        // comme discute precedemment).
        state->tls_config = altcp_tls_create_config_client(NULL, 0);
        if (!state->tls_config) {
            printf("[http] echec de creation de la config tls\n");
            free(state);
            return false;
        }
        if (port == 0) port = 443;
    } else if (port == 0) {
        port = 80;
    }
    state->port = port;

    // on construit nous-memes la requete texte : c'est ici, et uniquement ici, qu'on decide des
    // entetes envoyes - contrairement a httpc_get_file_dns qui imposait "Accept: */*" en dur.
    snprintf(state->request, sizeof(state->request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: pico2w-lab05\r\n"
             "Connection: close\r\n"
             "%s" // entetes additionnels fournis par l'appelant (deja termines par \r\n chacun)
             "\r\n",
             path, host, extra_headers ? extra_headers : "");

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

    printf("[http] connexion %s a %s:%u...\n", use_tls ? "tls" : "tcp", ip4addr_ntoa(&state->remote_addr), port);

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


    bool ok = (state->result == 0);
    printf("\n[http] --- fin (resultat = %d) ---\n", state->result);
    free(state);
    return ok;
}
