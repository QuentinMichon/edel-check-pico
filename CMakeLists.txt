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

add_executable(${PROJECT_NAME} main.c
        
)

target_link_libraries(${PROJECT_NAME}
        pico_stdlib
        hardware_spi
)

# redirige les printf vers la sortie USB
pico_enable_stdio_usb(${PROJECT_NAME} 1)
# désactive la communication UART (pin 0 et 1 par défaut)
pico_enable_stdio_uart(${PROJECT_NAME} 0)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

# config pour rendre stdio_init_all() bloquant en attente d'une communication avec picocom
target_compile_definitions(${PROJECT_NAME} PRIVATE
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=-1
)

pico_add_extra_outputs(${PROJECT_NAME})