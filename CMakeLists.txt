cmake_minimum_required(VERSION 3.13)

# spécifie d'utiliser le pico avec wi-fi (garde pico2_w même si on ne s'en sert pas encore ici,
# pour rester cohérent avec le reste du projet et le même hardware)
set(PICO_BOARD pico2_w)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(edel-check-pico C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# propre au SDK de pico
pico_sdk_init()

# identifiants du reseau wifi local, a fournir via -DWIFI_SSID=... -DWIFI_PASSWORD=... au cmake configure
set(WIFI_SSID "${WIFI_SSID}" CACHE INTERNAL "SSID du reseau wifi local")
set(WIFI_PASSWORD "${WIFI_PASSWORD}" CACHE INTERNAL "mot de passe du reseau wifi local")

if ("${WIFI_SSID}" STREQUAL "" OR "${WIFI_PASSWORD}" STREQUAL "")
        message(WARNING "WIFI_SSID / WIFI_PASSWORD non definis : relancer cmake avec -DWIFI_SSID=... -DWIFI_PASSWORD=...")
endif()

# adresse de l'API d'appairage. Les deux SEULS appels HTTP du boitier de toute sa vie
# (POST /provisioning/claim et /provisioning/poll) ; ensuite tout passe par MQTT.
set(EDEL_API_HOST "${EDEL_API_HOST}" CACHE STRING "hote de l'API d'appairage")
set(EDEL_API_PORT "${EDEL_API_PORT}" CACHE STRING "port de l'API d'appairage")
set(EDEL_API_USE_TLS "${EDEL_API_USE_TLS}" CACHE STRING "true/false : HTTPS vers l'API")

# Bequille de developpement, a laisser vide en production.
#
# /provisioning/poll renvoie l'adresse du broker, et c'est elle qui fait foi : le boitier
# la stocke en flash, donc demenager l'infrastructure ne demande aucune recompilation.
# Tant que MQTT_PUBLIC_HOST vaut `localhost` cote edelcheck, le serveur annonce cependant
# une adresse injoignable depuis le terrain - cette variable permet de la remplacer en
# attendant que la configuration serveur soit corrigee.
set(EDEL_BROKER_HOST_OVERRIDE "${EDEL_BROKER_HOST_OVERRIDE}" CACHE STRING "force l'adresse du broker (dev)")

if ("${EDEL_API_HOST}" STREQUAL "")
        set(EDEL_API_HOST "api.edelcheck.ch")
endif()
if ("${EDEL_API_PORT}" STREQUAL "")
        set(EDEL_API_PORT 443)
endif()
if ("${EDEL_API_USE_TLS}" STREQUAL "")
        set(EDEL_API_USE_TLS true)
endif()

add_executable(${PROJECT_NAME} main.c
        json/frozen.c
        json/json_util.c
        wifi/wifi_setup.c
        wifi/http_client.c
        wifi/ca_letsencrypt.c
        wifi/dhcpserver/dhcpserver.c
        wifi/wifi_portal.c
        navigation/nav.c
        boutons/boutons.c
        gpio/gpio_driver.c
        screen/epd_driver.c
        screen/epd_text.c
        storage/storage_manager.c
        enrollment/enrollment.c
        mqtt/mqtt_client.c
        profiles/profiles.c
        session/session.c
        image/image_rx.c
        power/power.c
)

target_link_libraries(${PROJECT_NAME}
        pico_stdlib
        hardware_spi
        hardware_adc
        hardware_flash
        hardware_sync
        hardware_watchdog
        pico_cyw43_arch_lwip_threadsafe_background
        pico_lwip_mbedtls
        pico_mbedtls
        # Le client MQTT vient de lwIP : on n'ecrit pas de protocole, seulement la logique
        # du contrat. Cible fournie par le SDK (pico_lwip/CMakeLists.txt).
        pico_lwip_mqtt
)

# redirige les printf vers la sortie USB
pico_enable_stdio_usb(${PROJECT_NAME} 1)
# désactive la communication UART (pin 0 et 1 par défaut)
pico_enable_stdio_uart(${PROJECT_NAME} 0)

target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/config
        ${CMAKE_CURRENT_LIST_DIR}/wifi
        ${CMAKE_CURRENT_LIST_DIR}/wifi/dhcpserver
        ${CMAKE_CURRENT_LIST_DIR}/json
)

# config pour rendre stdio_init_all() bloquant en attente d'une communication avec picocom
target_compile_definitions(${PROJECT_NAME} PRIVATE
        # json/frozen.c utilise PRId64 / PRIu64 (lignes 579 et 698-701), que la chaine
        # arm-none-eabi ne fournit pas : dans le <inttypes.h> de newlib ces macros sont
        # gardees par `#if __int64_t_defined`, qui vaut faux ici. Sans ces deux lignes le
        # firmware NE COMPILE PAS avec gcc arm-none-eabi 13.2.1 :
        #   json/frozen.c:579:47: error: expected ':' before 'PRIu64'
        # On les fournit en ligne de commande plutot que de modifier frozen.c, qui est du
        # code tiers.
        PRId64=\"lld\"
        PRIu64=\"llu\"

        # Attente d'un terminal serie au demarrage, en millisecondes.
        #
        # -1 = attendre indefiniment. C'etait pratique pour ne rien rater des traces de
        # demarrage, mais ca rend le boitier INUTILISABLE hors du bureau : branche sur une
        # prise, sans ordinateur, il reste fige a stdio_init_all() et l'ecran ne s'allume
        # jamais. Un delai fini laisse les traces lisibles quand un terminal est ouvert, et
        # laisse demarrer quand il n'y en a pas.
        PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=3000
        WIFI_SSID=\"${WIFI_SSID}\"
        WIFI_PASSWORD=\"${WIFI_PASSWORD}\"

        EDEL_API_HOST=\"${EDEL_API_HOST}\"
        EDEL_API_PORT=${EDEL_API_PORT}
        EDEL_API_USE_TLS=${EDEL_API_USE_TLS}
)

if (NOT "${EDEL_BROKER_HOST_OVERRIDE}" STREQUAL "")
        target_compile_definitions(${PROJECT_NAME} PRIVATE
                EDEL_BROKER_HOST_OVERRIDE=\"${EDEL_BROKER_HOST_OVERRIDE}\")
        message(STATUS "adresse du broker forcee a ${EDEL_BROKER_HOST_OVERRIDE} (bequille de dev)")
endif()

pico_add_extra_outputs(${PROJECT_NAME})