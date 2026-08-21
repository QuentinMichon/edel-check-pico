//
// Created by Quentin Michon on 14.08.2026.
//

#ifndef EDEL_CHECK_PICO_STORAGE_MANAGER_H
#define EDEL_CHECK_PICO_STORAGE_MANAGER_H

#include <pico/stdlib.h>

#define MEM_BEARER_TOKEN_SIZE 800
#define MEM_SSID_SIZE 32
#define MEM_PASSWORD_SIZE 64

#define CONFIG_MAGIC 0xCAFE7478

typedef struct {
    uint32_t magic;
    uint8_t flags;              // flags[0] ~ manufacturer state (1 true / 0 false)

    char bearer_token[MEM_BEARER_TOKEN_SIZE];

    char wifi_1_ssid[MEM_SSID_SIZE];
    char wifi_1_password[MEM_PASSWORD_SIZE];
    char wifi_2_ssid[MEM_SSID_SIZE];
    char wifi_2_password[MEM_PASSWORD_SIZE];
    char wifi_3_ssid[MEM_SSID_SIZE];
    char wifi_3_password[MEM_PASSWORD_SIZE];

} persistent_storage_t;

bool init_local_storage(void);
persistent_storage_t *get_local_storage();
void put_bearer_token(char *bearer_token);

#endif //EDEL_CHECK_PICO_STORAGE_MANAGER_H
