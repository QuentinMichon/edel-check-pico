//
// Created by Quentin Michon on 03.08.2026.
//

#ifndef EDEL_CHECK_PICO_JSON_UTIL_H
#define EDEL_CHECK_PICO_JSON_UTIL_H

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "frozen.h"
#include "pico/stdlib.h"
#include "screen/epd_driver.h"

void handle_token(const char *body);

void print_qr_code(const char *body, int x, int y, int scale);

#endif //EDEL_CHECK_PICO_JSON_UTIL_H
