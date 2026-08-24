# Banc de test autonome de l'ecran e-ink.
#
# Projet CMake SEPARE : il ne modifie pas le CMakeLists.txt de la racine et ne
# compile que ce dont l'ecran a besoin.
#
#   cmake -B build -G Ninja -S .        (depuis tests/screen_test/)
#   cmake --build build
#   -> build/screen_test.uf2
#
# Contrairement au firmware principal :
#   - aucune dependance Wi-Fi / lwIP / mbedTLS -> pas de -DWIFI_SSID a fournir
#   - aucune dependance au stockage flash      -> rien n'est ecrit en persistant
#   - PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS fini -> la carte demarre meme sans
#     terminal serie attache (le firmware principal, lui, attend indefiniment)
#   - DEBUG_PRINT active -> les diagnostics de epd_driver.c sont compiles

cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(screen_test C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# racine du depot, deux niveaux au-dessus
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

add_executable(screen_test
        screen_test.c
        ${REPO_ROOT}/screen/epd_driver.c
        ${REPO_ROOT}/gpio/gpio_driver.c
)

target_link_libraries(screen_test
        pico_stdlib
        hardware_spi
)

# les sources partagees incluent "gpio/gpio_driver.h" et "assets/..." depuis la racine
target_include_directories(screen_test PRIVATE ${REPO_ROOT})

pico_enable_stdio_usb(screen_test 1)
pico_enable_stdio_uart(screen_test 0)

target_compile_definitions(screen_test PRIVATE
        # 3 s au lieu de -1 : la carte n'attend PAS indefiniment un terminal.
        # C'est la difference volontaire avec le firmware principal.
        PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=3000
        # active les printf de diagnostic de epd_driver.c, sinon compiles hors binaire
        DEBUG_PRINT
)

pico_add_extra_outputs(screen_test)
