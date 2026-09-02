// Portail de configuration reseau.
//
// Quand le boitier ne trouve pas le reseau enregistre, il devient lui-meme un point
// d acces. Le commerçant s y connecte avec son telephone, choisit son reseau et saisit le
// mot de passe. Aucun cable, aucun ordinateur, aucune recompilation.
//
// Le serveur HTTP est ecrit ici plutot que repris de lwIP : httpd sert un systeme de
// fichiers compile a l avance et passe par des gestionnaires CGI, ce qui demande une etape
// de generation dans le build. Pour une page et un formulaire, du TCP brut coute moins
// cher et se lit d un bloc.

#include "wifi_portal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "dhcpserver.h"
#include "storage/storage_manager.h"

#define PORT_HTTP        80
#define TAILLE_REQUETE   1024

static dhcp_server_t dhcp;
static volatile bool  recu;          // un reseau a ete soumis
static char           ssid_recu[MEM_SSID_SIZE];
static char           pass_recu[MEM_PASSWORD_SIZE];

// ---------------------------------------------------------------------------

static const char PAGE[] =
    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
    "<!doctype html><html lang=fr><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>EdelCheck</title>"
    "<style>body{background:#191A2E;color:#fff;font:16px system-ui;margin:0;padding:32px}"
    "h1{font-size:22px;margin:0 0 4px}p{color:#E8E8E4;margin:0 0 24px;font-size:14px}"
    "label{display:block;font-size:13px;color:#E8E8E4;margin:16px 0 6px}"
    "input{width:100%;box-sizing:border-box;padding:12px;border:1px solid #2B2C3E;"
    "border-radius:3px;background:#2B2C3E;color:#fff;font-size:16px}"
    "button{width:100%;margin-top:24px;padding:14px;border:0;border-radius:3px;"
    "background:#02D2E2;color:#191A2E;font-size:16px;font-weight:600}</style>"
    "<h1>Configuration reseau</h1>"
    "<p>Indiquez le reseau que ce boitier doit rejoindre.</p>"
    "<form method=POST action=/>"
    "<label for=s>Nom du reseau</label><input id=s name=s required>"
    "<label for=p>Mot de passe</label><input id=p name=p type=password>"
    "<button type=submit>Enregistrer</button></form></html>";

static const char CONFIRME[] =
    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
    "<!doctype html><html lang=fr><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<style>body{background:#191A2E;color:#fff;font:16px system-ui;padding:32px}"
    "b{color:#02D2E2}</style>"
    "<h1>Enregistre</h1><p>Le boitier redemarre et rejoint <b>le reseau indique</b>.</p></html>";

// ---------------------------------------------------------------------------
// Decodage d un corps application/x-www-form-urlencoded : s=...&p=...

static void decoder_valeur(const char *src, size_t len, char *dst, size_t dst_len) {
    size_t j = 0;
    for (size_t i = 0; i < len && j + 1 < dst_len; i++) {
        if (src[i] == '+') {
            dst[j++] = ' ';
        } else if (src[i] == '%' && i + 2 < len) {
            char hex[3] = { src[i + 1], src[i + 2], 0 };
            dst[j++] = (char) strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

static bool champ(const char *corps, const char *nom, char *dst, size_t dst_len) {
    char cle[8];
    snprintf(cle, sizeof(cle), "%s=", nom);
    const char *p = strstr(corps, cle);
    if (!p) return false;
    p += strlen(cle);
    const char *fin = strchr(p, '&');
    decoder_valeur(p, fin ? (size_t)(fin - p) : strlen(p), dst, dst_len);
    return dst[0] != '\0';
}

// ---------------------------------------------------------------------------

// Un navigateur envoie couramment les en-tetes et le corps du POST dans deux segments
// TCP distincts. Lire un seul paquet trouvait donc le separateur \r\n\r\n avec un corps
// encore vide, et le formulaire semblait ne jamais arriver. On accumule jusqu a disposer
// des Content-Length octets annonces.
static char   tampon[TAILLE_REQUETE];
static size_t tampon_len;

static bool requete_complete(void) {
    const char *fin_entetes = strstr(tampon, "\r\n\r\n");
    if (!fin_entetes) return false;
    if (strncmp(tampon, "POST", 4) != 0) return true;

    const char *cl = strstr(tampon, "Content-Length:");
    if (!cl) return true;
    size_t attendu = (size_t) strtol(cl + 15, NULL, 10);
    size_t recu_corps = tampon_len - (size_t)(fin_entetes + 4 - tampon);
    return recu_corps >= attendu;
}

static err_t sur_donnees(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    if (!p) { tcp_close(pcb); return ERR_OK; }

    size_t place = TAILLE_REQUETE - 1 - tampon_len;
    size_t n = p->tot_len < place ? p->tot_len : place;
    pbuf_copy_partial(p, tampon + tampon_len, n, 0);
    tampon_len += n;
    tampon[tampon_len] = '\0';
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    if (!requete_complete()) {
        return ERR_OK;                       // on attend la suite
    }

    const char *reponse = PAGE;
    if (strncmp(tampon, "POST", 4) == 0) {
        const char *corps = strstr(tampon, "\r\n\r\n");
        if (corps && champ(corps + 4, "s", ssid_recu, sizeof(ssid_recu))) {
            champ(corps + 4, "p", pass_recu, sizeof(pass_recu));
            reponse = CONFIRME;
            printf("[portail] reseau soumis : \"%s\"\n", ssid_recu);
            recu = true;
        } else {
            printf("[portail] POST recu mais champ 's' absent\n");
        }
    }

    tampon_len = 0;
    tcp_write(pcb, reponse, strlen(reponse), TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    tcp_close(pcb);
    return ERR_OK;
}

static err_t sur_connexion(void *arg, struct tcp_pcb *pcb, err_t err) {
    if (err != ERR_OK || !pcb) return ERR_VAL;
    tampon_len = 0;
    tcp_recv(pcb, sur_donnees);
    return ERR_OK;
}

// ---------------------------------------------------------------------------

bool wifi_portal_run(const char *ap_ssid, const char *ap_password, uint32_t timeout_ms) {
    recu = false;
    ssid_recu[0] = pass_recu[0] = '\0';

    cyw43_arch_enable_ap_mode(ap_ssid, ap_password, CYW43_AUTH_WPA2_AES_PSK);

    ip4_addr_t passerelle, masque;
    IP4_ADDR(ip_2_ip4(&passerelle), 192, 168, 4, 1);
    IP4_ADDR(ip_2_ip4(&masque), 255, 255, 255, 0);
    dhcp_server_init(&dhcp, &cyw43_state.netif[CYW43_ITF_AP], &passerelle, &masque);

    struct tcp_pcb *ecoute = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (ecoute) ip_set_option(ecoute, SOF_REUSEADDR);
    if (!ecoute || tcp_bind(ecoute, IP_ANY_TYPE, PORT_HTTP) != ERR_OK) {
        printf("[portail] impossible d ecouter sur le port %d\n", PORT_HTTP);
        dhcp_server_deinit(&dhcp);
        cyw43_arch_disable_ap_mode();
        return false;
    }
    ecoute = tcp_listen_with_backlog(ecoute, 1);
    tcp_accept(ecoute, sur_connexion);

    printf("[portail] point d acces \"%s\" ouvert, page sur http://192.168.4.1\n", ap_ssid);

    absolute_time_t limite = make_timeout_time_ms(timeout_ms);
    while (!recu && absolute_time_diff_us(get_absolute_time(), limite) > 0) {
        cyw43_arch_poll();
        sleep_ms(50);
    }

    tcp_close(ecoute);
    dhcp_server_deinit(&dhcp);
    cyw43_arch_disable_ap_mode();

    if (!recu) {
        printf("[portail] delai ecoule, personne n a configure le boitier\n");
        return false;
    }

    storage_save_wifi(ssid_recu, pass_recu);
    printf("[portail] reseau enregistre en flash\n");
    return true;
}
