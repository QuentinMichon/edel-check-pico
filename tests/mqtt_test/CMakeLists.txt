# Banc de test : MQTT sur TLS vers le broker EdelCheck.
#
# Projet CMake SEPARE : il ne touche pas au CMakeLists.txt de la racine.
#
#   WIFI_SSID=... WIFI_PASSWORD=... ./flash.sh
#
# Comme les autres bancs :
#   - n'utilise PAS storage_manager -> rien n'est ecrit en flash, la config de la carte
#     reste intacte, et les identifiants viennent des -D
#   - PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS fini -> demarre sans terminal serie
#   - ni ecran, ni JSON : ce banc ne prouve QUE la pile MQTT/TLS

cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(mqtt_test C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

foreach(V WIFI_SSID WIFI_PASSWORD DEVICE_ID DEVICE_SECRET BROKER_IP)
    set(${V} "${${V}}" CACHE INTERNAL "")
    if ("${${V}}" STREQUAL "")
        message(FATAL_ERROR "${V} non defini : passer par ./flash.sh")
    endif()
endforeach()

add_executable(mqtt_test
        mqtt_test.c
        ${REPO_ROOT}/wifi/wifi_setup.c
)

target_link_libraries(mqtt_test
        pico_stdlib
        pico_cyw43_arch_lwip_threadsafe_background
        pico_lwip_mbedtls
        pico_mbedtls
        # Le client MQTT est fourni par lwIP - on n'ecrit pas de protocole, seulement
        # la logique. Cf. pico-sdk/src/rp2_common/pico_lwip/CMakeLists.txt:262
        pico_lwip_mqtt
)

target_include_directories(mqtt_test PRIVATE
        ${REPO_ROOT}
        ${REPO_ROOT}/config      # lwipopts.h, mbedtls_config.h
        ${REPO_ROOT}/wifi
)

pico_enable_stdio_usb(mqtt_test 1)
pico_enable_stdio_uart(mqtt_test 0)

target_compile_definitions(mqtt_test PRIVATE
        # ── La suite de chiffrement ────────────────────────────────────────────
        # config/mbedtls_config.h n'active que RSA statique (refuse par tout serveur
        # moderne) et ECDHE-ECDSA. Le broker EdelCheck sert un certificat RSA 2048 et
        # negocie ECDHE-RSA : sans suite en commun, il ferme et lwIP remonte -15.
        #
        # Mesure sur la pile reelle, le 23.08.2026 :
        #   openssl -cipher ECDHE-ECDSA-...  -> handshake failure, alert 40
        #   openssl -cipher ECDHE-RSA-...    -> ECDHE-RSA-AES256-GCM-SHA384
        #   Peer signature type              -> RSA-PSS   (d'ou PKCS1_V21)
        #
        # Ajoute ici plutot que dans config/mbedtls_config.h, qui appartient au
        # firmware principal - c'est une correction a porter la-bas, dans sa propre
        # branche, apres validation par ce banc.
        MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
        MBEDTLS_PKCS1_V21

        # ── Pool de timers lwIP ────────────────────────────────────────────────
        # PANIC mesure le 23.08.2026, juste apres la connexion Wi-Fi :
        #   sys_timeout: timeout != NULL, pool MEMP_SYS_TIMEOUT is empty
        #
        # lwIP dimensionne ce pool par une formule (opt.h:519) qui ne compte que les
        # modules du COEUR : TCP + ARP + 2xDHCP + ACD + DNS. Les applications n'y
        # figurent pas. Or le client MQTT arme un timer cyclique a la connexion
        # (mqtt.c:1073) - il demande donc une entree de plus que ce que la formule a
        # prevu, et le pool est exactement plein a cet instant.
        #
        # Ce n'est pas propre a ce banc : TOUT ajout de MQTT a cette configuration lwIP
        # produira le meme panic, y compris dans le firmware principal le jour ou il
        # l'integrera. La correction appartient a config/lwipopts.h.
        MEMP_NUM_SYS_TIMEOUT=16

        # ── Tampon de sortie MQTT ──────────────────────────────────────────────
        # Le defaut de lwIP est 256 octets, et tout ce qu'on publie y transite : le
        # testament declare au CONNECT, le topic (dev/{uuid}/status = 44 caracteres) et
        # la charge utile de presence. Ca passerait sans doute, mais une publication qui
        # echoue en silence sur une connexion par ailleurs saine est un symptome
        # penible - autant l'ecarter tout de suite.
        MQTT_OUTPUT_RINGBUF_SIZE=1024

        PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=3000

        WIFI_SSID=\"${WIFI_SSID}\"
        WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
        DEVICE_ID=\"${DEVICE_ID}\"
        DEVICE_SECRET=\"${DEVICE_SECRET}\"
        BROKER_IP=\"${BROKER_IP}\"
)

pico_add_extra_outputs(mqtt_test)
