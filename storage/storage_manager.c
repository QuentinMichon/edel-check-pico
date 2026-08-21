//
// Created by Quentin Michon on 14.08.2026.
//

#include "storage_manager.h"

#include <stdio.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include <pico/stdlib.h>
#include <string.h>

static persistent_storage_t g_config;

// 2 secteurs = 8192 octets réservés
#define FLASH_STORAGE_SIZE (FLASH_SECTOR_SIZE)
#define FLASH_TARGET_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

// Vérification à la compilation que ça tient dans un secteur
_Static_assert(sizeof(persistent_storage_t) <= FLASH_SECTOR_SIZE, "persistent_config_t trop grande pour un secteur flash");

/*
 *  Usage :
 *         if (!load_local_storage(&ls)) { <edit storage> }
 *         save_local_storage(&ls);
 */
static void save_local_storage(const persistent_storage_t *ls) {
    uint8_t buffer[FLASH_STORAGE_SIZE];
    memset(buffer, 0xFF, FLASH_STORAGE_SIZE);
    memcpy(buffer, ls, sizeof(persistent_storage_t));

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_STORAGE_SIZE);
    flash_range_program(FLASH_TARGET_OFFSET, buffer, FLASH_STORAGE_SIZE);
    restore_interrupts(ints);
}

static bool load_local_storage(persistent_storage_t *ls) {
    const uint8_t *flash_ptr = (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET);
    memcpy(ls, flash_ptr, sizeof(persistent_storage_t));
    return ls->magic == CONFIG_MAGIC;
}

bool init_local_storage(void) {

    if (!load_local_storage(&g_config)) {
        printf("[storage] edit default local storage values\n");
        persistent_storage_t new_ls;

        // première utilisation : valeur par défaut + TODO lancement appairage
        new_ls.magic = CONFIG_MAGIC;
        new_ls.flags = 0;
        strncpy(new_ls.bearer_token, "", sizeof(new_ls.bearer_token));
        strncpy(new_ls.wifi_1_ssid, WIFI_SSID, sizeof(new_ls.wifi_1_ssid));
        strncpy(new_ls.wifi_1_password, WIFI_PASSWORD, sizeof(new_ls.wifi_1_password));

        save_local_storage(&new_ls);

        if (!load_local_storage(&g_config)) {
            printf("[storage] error local storage init nok...\n");
            return false;
        }
    }

    printf("[storage] local storage init ok\n");
    return true;
}

// TODO ADD MUTEX
persistent_storage_t *get_local_storage() {
    if (!load_local_storage(&g_config)) {
        printf("[storage] error local storage not init. You have to call [init_local_storage] before using this function...\n");
    }
    return &g_config;
}

void put_bearer_token(char *bearer_token) {
    if (bearer_token == NULL) {
        return;
    }

    if (!load_local_storage(&g_config)) {
        printf("[storage] error local storage not init. You have to call [init_local_storage] before using this function...\n");
    }

    strncpy(g_config.bearer_token, bearer_token, sizeof(g_config.bearer_token));

    save_local_storage(&g_config);

    printf("[storage] Bearer token saved\n");
}