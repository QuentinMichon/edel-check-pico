# Banc de test : l'ecran est pilote par une API distante.
#
# Projet CMake SEPARE : il ne modifie pas le CMakeLists.txt de la racine.
#
#   WIFI_SSID=... WIFI_PASSWORD=... ./flash.sh
#
# Contrairement au firmware principal :
#   - n'utilise PAS storage_manager -> rien n'est ecrit en flash, la config de la carte
#     reste intacte, et les identifiants Wi-Fi viennent directement des -D (pas de la
#     flash semee au premier boot)
#   - PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS fini -> demarre sans terminal serie
#   - un echec Wi-Fi affiche un ecran d'erreur au lieu de faire return -1

cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(api_screen C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(WIFI_SSID "${WIFI_SSID}" CACHE INTERNAL "SSID du reseau wifi")
set(WIFI_PASSWORD "${WIFI_PASSWORD}" CACHE INTERNAL "mot de passe du reseau wifi")

if ("${WIFI_SSID}" STREQUAL "" OR "${WIFI_PASSWORD}" STREQUAL "")
    message(FATAL_ERROR "WIFI_SSID / WIFI_PASSWORD non definis : relancer avec -DWIFI_SSID=... -DWIFI_PASSWORD=...")
endif()

add_executable(api_screen
        api_screen.c
        ${REPO_ROOT}/screen/epd_driver.c
        ${REPO_ROOT}/gpio/gpio_driver.c
        ${REPO_ROOT}/wifi/wifi_setup.c
        ${REPO_ROOT}/wifi/http_client.c
        ${REPO_ROOT}/json/frozen.c
)

target_link_libraries(api_screen
        pico_stdlib
        hardware_spi
        pico_cyw43_arch_lwip_threadsafe_background
        pico_lwip_mbedtls
        pico_mbedtls
)

target_include_directories(api_screen PRIVATE
        ${REPO_ROOT}
        ${REPO_ROOT}/config      # lwipopts.h, mbedtls_config.h
        ${REPO_ROOT}/wifi
        ${REPO_ROOT}/json
)

pico_enable_stdio_usb(api_screen 1)
pico_enable_stdio_uart(api_screen 0)

# json/frozen.c utilise PRId64 / PRIu64 (lignes 579 et 698-701) sans que la chaine
# arm-none-eabi les fournisse : dans le <inttypes.h> de newlib, ces macros sont gardees
# par `#if __int64_t_defined`, qui vaut faux ici. On les fournit donc en ligne de
# commande plutot que de modifier frozen.c, qui est du code tiers.
# NB : le CMakeLists.txt de la racine ne le fait PAS — voir docs/ARCHITECTURE.md.
target_compile_definitions(api_screen PRIVATE
        PRId64=\"lld\"
        PRIu64=\"llu\"
        # config/mbedtls_config.h n'active que deux echanges de cles : RSA statique
        # (refuse par tout serveur moderne) et ECDHE-ECDSA. Ca suffit pour
        # auth.edel-id.app, dont le certificat est ECDSA — mais PAS pour un serveur a
        # certificat RSA, qui negocie ECDHE-RSA. Sans suite en commun, le serveur ferme
        # et lwIP remonte -15 (ERR_CLSD).
        #   vercel.app  -> ECDHE-RSA-AES128-GCM-SHA256, certificat rsaEncryption
        #   edel-id.app -> ECDHE-ECDSA-AES256-GCM-SHA384, certificat id-ecPublicKey
        # PKCS1_V21 va avec : les serveurs recents signent en RSA-PSS.
        # Ajoute ici plutot que dans config/mbedtls_config.h, qui appartient au firmware.
        MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
        MBEDTLS_PKCS1_V21
        PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=3000
        WIFI_SSID=\"${WIFI_SSID}\"
        WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
)

pico_add_extra_outputs(api_screen)
