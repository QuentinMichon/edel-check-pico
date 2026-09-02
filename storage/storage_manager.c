//
// Created by Quentin Michon on 14.08.2026.
//

#include "storage_manager.h"

#include <stdio.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include <pico/stdlib.h>
#include <stddef.h>
#include <string.h>

static persistent_storage_t g_config;

// Deux secteurs alternes : les deux derniers de la flash.
//
// Ce n'est pas de la coquetterie. Le serveur ne sert le secret d'appairage qu'une seule
// fois, et un second poll reussi est traite comme une PREUVE de duplication. Avec un seul
// emplacement, une coupure de courant pendant l'effacement laisserait le boitier sans
// identite alors que le serveur en a deja delivre une : au redemarrage il redemanderait un
// appairage et se ferait marquer SUSPECT.
//
// Avec deux emplacements, l'ancien contenu reste lisible tant que le nouveau n'est pas
// integralement ecrit ET relu. Le champ `seq` designe le plus recent des deux.
#define FLASH_STORAGE_SIZE  (FLASH_SECTOR_SIZE)
#define FLASH_SLOT_A_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define FLASH_SLOT_B_OFFSET (PICO_FLASH_SIZE_BYTES - 2 * FLASH_SECTOR_SIZE)

_Static_assert(sizeof(persistent_storage_t) <= FLASH_SECTOR_SIZE,
               "persistent_storage_t trop grande pour un secteur flash");

// CRC-32 (polynome IEEE 802.3 reflechi), sans table : quelques milliers d'iterations sur
// 2,9 Ko, c'est invisible a cote d'un effacement de secteur.
static uint32_t crc32_compute(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t) (-(int32_t) (crc & 1)));
        }
    }
    return ~crc;
}

// Le CRC couvre tout ce qui suit le champ crc32 lui-meme.
static uint32_t storage_crc(const persistent_storage_t *ls) {
    const size_t skip = offsetof(persistent_storage_t, flags);
    return crc32_compute((const uint8_t *) ls + skip, sizeof(*ls) - skip);
}

static bool slot_is_valid(const persistent_storage_t *ls) {
    return ls->magic == CONFIG_MAGIC && ls->crc32 == storage_crc(ls);
}

static void slot_read(uint32_t offset, persistent_storage_t *out) {
    memcpy(out, (const uint8_t *) (XIP_BASE + offset), sizeof(persistent_storage_t));
}

// Ecrit dans le secteur donne, puis RELIT depuis la flash et compare. Un `flash_range_program`
// qui rend la main ne prouve rien : seule la relecture le fait.
static bool slot_write(uint32_t offset, const persistent_storage_t *ls) {
    static uint8_t buffer[FLASH_STORAGE_SIZE];   // 4 Ko : bien trop pour la pile de 4 Ko du core0
    memset(buffer, 0xFF, FLASH_STORAGE_SIZE);
    memcpy(buffer, ls, sizeof(persistent_storage_t));

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(offset, FLASH_STORAGE_SIZE);
    flash_range_program(offset, buffer, FLASH_STORAGE_SIZE);
    restore_interrupts(ints);

    static persistent_storage_t relu;
    slot_read(offset, &relu);
    return slot_is_valid(&relu) && relu.seq == ls->seq
           && memcmp(&relu, ls, sizeof(persistent_storage_t)) == 0;
}

// Retourne l'offset du secteur le PLUS RECENT valide, ou UINT32_MAX si aucun ne l'est.
static uint32_t slot_newest(persistent_storage_t *out) {
    static persistent_storage_t a, b;
    slot_read(FLASH_SLOT_A_OFFSET, &a);
    slot_read(FLASH_SLOT_B_OFFSET, &b);

    bool va = slot_is_valid(&a), vb = slot_is_valid(&b);

    if (va && vb) {
        // Comparaison signee sur la difference : reste correcte si le compteur reboucle.
        if ((int32_t) (a.seq - b.seq) >= 0) { *out = a; return FLASH_SLOT_A_OFFSET; }
        *out = b; return FLASH_SLOT_B_OFFSET;
    }
    if (va) { *out = a; return FLASH_SLOT_A_OFFSET; }
    if (vb) { *out = b; return FLASH_SLOT_B_OFFSET; }
    return UINT32_MAX;
}

// Ecrit dans le secteur qui n'est PAS le courant, avec un `seq` incremente. Tant que cette
// ecriture n'est pas terminee et relue, l'ancien secteur reste le plus recent valide - donc
// une coupure de courant ramene simplement a l'etat precedent.
static bool save_local_storage(persistent_storage_t *ls) {
    static persistent_storage_t courant;
    uint32_t offset_courant = slot_newest(&courant);

    uint32_t cible = (offset_courant == FLASH_SLOT_A_OFFSET)
                         ? FLASH_SLOT_B_OFFSET : FLASH_SLOT_A_OFFSET;

    ls->magic = CONFIG_MAGIC;
    ls->seq   = (offset_courant == UINT32_MAX) ? 1 : courant.seq + 1;
    ls->crc32 = storage_crc(ls);

    if (!slot_write(cible, ls)) {
        printf("[storage] ecriture non confirmee a la relecture - flash inchangee\n");
        return false;
    }
    return true;
}

static bool load_local_storage(persistent_storage_t *ls) {
    return slot_newest(ls) != UINT32_MAX;
}

bool init_local_storage(void) {

    if (!load_local_storage(&g_config)) {
        printf("[storage] edit default local storage values\n");
        static persistent_storage_t new_ls;
        memset(&new_ls, 0, sizeof(new_ls));

        // première utilisation : valeur par défaut + appairage au premier démarrage
        new_ls.flags = 0;
        strncpy(new_ls.bearer_token, "", sizeof(new_ls.bearer_token));
        strncpy(new_ls.wifi_1_ssid, WIFI_SSID, sizeof(new_ls.wifi_1_ssid) - 1);
        strncpy(new_ls.wifi_1_password, WIFI_PASSWORD, sizeof(new_ls.wifi_1_password) - 1);

        if (!save_local_storage(&new_ls) || !load_local_storage(&g_config)) {
            printf("[storage] error local storage init nok...\n");
            return false;
        }
    }

    printf("[storage] local storage init ok (generation %lu, %s)\n",
           (unsigned long) g_config.seq,
           g_config.device_id[0] ? "boitier appaire" : "boitier non appaire");
    return true;
}

// Pas de verrou, et ce n'est pas un oubli : tous les appelants actuels sont sur la boucle
// principale. g_config est partage et relu a chaque appel, donc un appel depuis un rappel
// lwIP introduirait une course sur une structure a moitie chargee. Le mode
// pico_cyw43_arch_lwip_threadsafe_background rend ce cas possible : verifier ce point avant
// d'appeler get_local_storage() ailleurs que depuis la boucle.
persistent_storage_t *get_local_storage() {
    if (!load_local_storage(&g_config)) {
        printf("[storage] error local storage not init. You have to call [init_local_storage] before using this function...\n");
    }
    return &g_config;
}

// --- appairage ---------------------------------------------------------------

bool storage_is_enrolled(void) {
    if (!load_local_storage(&g_config)) {
        return false;
    }
    return g_config.device_id[0] != '\0' && g_config.device_secret[0] != '\0';
}

bool storage_save_enrollment(const char *device_id, const char *device_secret,
                             const char *broker_host, uint16_t broker_port,
                             const char *ca_cert_pem) {
    if (device_id == NULL || device_secret == NULL) {
        return false;
    }
    if (!load_local_storage(&g_config)) {
        printf("[storage] appairage impossible : stockage non initialise\n");
        return false;
    }

    strncpy(g_config.device_id,     device_id,     sizeof(g_config.device_id) - 1);
    strncpy(g_config.device_secret, device_secret, sizeof(g_config.device_secret) - 1);
    strncpy(g_config.broker_host,   broker_host ? broker_host : "",
            sizeof(g_config.broker_host) - 1);
    g_config.broker_port = broker_port;

    if (ca_cert_pem != NULL) {
        if (strlen(ca_cert_pem) >= sizeof(g_config.ca_cert_pem)) {
            // Tronquer un certificat PEM le rend inutilisable : mieux vaut refuser
            // l'appairage tout de suite que decouvrir a la connexion suivante que la
            // chaine de confiance est corrompue.
            printf("[storage] certificat d'autorite trop grand (%u > %u octets)\n",
                   (unsigned) strlen(ca_cert_pem), (unsigned) sizeof(g_config.ca_cert_pem));
            return false;
        }
        strncpy(g_config.ca_cert_pem, ca_cert_pem, sizeof(g_config.ca_cert_pem) - 1);
    }

    if (!save_local_storage(&g_config)) {
        return false;
    }

    // Relecture depuis la flash, pas depuis g_config : c'est ce qui distingue "ecrit" de
    // "ecrit et verifie", et c'est cette distinction qui evite le marquage SUSPECT.
    static persistent_storage_t verif;
    if (!load_local_storage(&verif)
        || strcmp(verif.device_id, device_id) != 0
        || strcmp(verif.device_secret, device_secret) != 0) {
        printf("[storage] relecture de l'appairage incoherente\n");
        return false;
    }

    printf("[storage] appairage persiste et verifie (generation %lu)\n",
           (unsigned long) verif.seq);
    return true;
}

bool storage_clear_enrollment(void) {
    if (!load_local_storage(&g_config)) {
        return false;
    }
    memset(g_config.device_id,     0, sizeof(g_config.device_id));
    memset(g_config.device_secret, 0, sizeof(g_config.device_secret));
    memset(g_config.broker_host,   0, sizeof(g_config.broker_host));
    memset(g_config.ca_cert_pem,   0, sizeof(g_config.ca_cert_pem));
    g_config.broker_port = 0;

    return save_local_storage(&g_config);
}

bool storage_save_wifi(const char *ssid, const char *password) {
    if (ssid == NULL || ssid[0] == '\0') {
        return false;
    }
    if (!load_local_storage(&g_config)) {
        printf("[storage] enregistrement wifi impossible : stockage non initialise\n");
        return false;
    }

    strncpy(g_config.wifi_1_ssid, ssid, sizeof(g_config.wifi_1_ssid) - 1);
    g_config.wifi_1_ssid[sizeof(g_config.wifi_1_ssid) - 1] = '\0';
    strncpy(g_config.wifi_1_password, password ? password : "",
            sizeof(g_config.wifi_1_password) - 1);
    g_config.wifi_1_password[sizeof(g_config.wifi_1_password) - 1] = '\0';

    if (!save_local_storage(&g_config)) {
        printf("[storage] ecriture flash du reseau echouee\n");
        return false;
    }
    printf("[storage] reseau enregistre : %s\n", g_config.wifi_1_ssid);
    return true;
}
