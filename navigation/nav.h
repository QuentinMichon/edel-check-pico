//
// Created by Quentin Michon on 24.07.2026.
//

#ifndef EDEL_CHECK_PICO_NAV_H
#define EDEL_CHECK_PICO_NAV_H

typedef enum NAV_PAGE {
    NAV_PAGE_START,
    NAV_PAGE_PROFILE,
    NAV_PAGE_CHECK,
    NAV_PAGE_SETTINGS,
} nav_page_t;

void poll_usb_nav_key(void);

#endif //EDEL_CHECK_PICO_NAV_H
