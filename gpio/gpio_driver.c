//
// Created by Quentin Michon on 24.07.2026.
//

#include "gpio_driver.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include <stdio.h>

void init_gpio(void) {

    // SCREEN E-INK
    spi_init(EPD_SPI_PORT, EPD_SPI_BAUDRATE);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    gpio_init(PIN_ECS);
    gpio_set_dir(PIN_ECS, GPIO_OUT);
    gpio_put(PIN_ECS, 1);

    gpio_init(PIN_DC);
    gpio_set_dir(PIN_DC, GPIO_OUT);
    gpio_put(PIN_DC, 0);

    gpio_init(PIN_RST);
    gpio_set_dir(PIN_RST, GPIO_OUT);
    gpio_put(PIN_RST, 1);

    gpio_init(PIN_BUSY);
    gpio_set_dir(PIN_BUSY, GPIO_IN);
    // SCREEN E-INK - end

    printf("SPI + GPIO ready!\n");
}
