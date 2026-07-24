#include <stdio.h>
#include <stdbool.h>


#include "pico/stdlib.h"

/*
 *  Main
 */
int main(int argc, char *argv[]) {
    stdio_init_all();   // init USB
    sleep_ms(1000);     // time for picocom to print initial log
    // --- BLOC until USB connected ---

    printf("\n\n\n======== EDEL CHECK ==========\n\n");
    printf("version 1.0.0\n");
    printf("   -   test\n\n");

    while (true) {
        printf("ok\n");
        sleep_ms(1000);
    }

    return 0;
}
