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
// GP22 s'est retrouvee en court-circuit avec le 3V3 le 03.09.2026 : le plot ne descend
// plus, meme au courant de sortie maximum, et un tirage bas interne perd 20 fois sur 20.
// L'impulsion de reset n'atteignait donc plus la dalle, qui restait en veille profonde,
// dont on ne sort que par un reset materiel.
//
// La broche est surchargeable a la compilation, pour deplacer le fil sans toucher au code :
//     cmake -B build -G Ninja -DEDEL_PIN_RST=26 ...
#ifndef PIN_RST
#define PIN_RST         22
#endif
#define PIN_BUSY        20


// instance SPI du controller (SPI0 = 16/18/19) - SCREEN E-INK FRIEND
#define EPD_SPI_PORT    spi0
#define EPD_SPI_BAUDRATE (2 * 1000 * 1000)

void init_gpio(void);

#endif //EDEL_CHECK_PICO_GPIO_DRIVER_H
