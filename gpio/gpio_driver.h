//
// Created by Quentin Michon on 24.07.2026.
//

#ifndef EDEL_CHECK_PICO_GPIO_DRIVER_H
#define EDEL_CHECK_PICO_GPIO_DRIVER_H

// --- Définition des pins ---
#define PIN_SCK         18
#define PIN_MISO        16
#define PIN_MOSI        19

#define PIN_ECS         17
#define PIN_DC          21
#define PIN_RST         22
#define PIN_BUSY        20


// instance SPI du controller (SPI0 = 16/18/19) - SCREEN E-INK FRIEND
#define EPD_SPI_PORT    spi0
#define EPD_SPI_BAUDRATE (2 * 1000 * 1000)

void init_gpio(void);

#endif //EDEL_CHECK_PICO_GPIO_DRIVER_H
