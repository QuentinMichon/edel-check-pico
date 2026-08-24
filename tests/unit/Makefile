# Tests unitaires du firmware, executes sur la machine de developpement.
#
#   make          compile et lance
#   make clean
#
# Aucune dependance : ni SDK Pico, ni chaine arm-none-eabi, ni materiel. Les modules
# testes sont ceux qui ne parlent pas au materiel ; le pilote d'ecran est remplace par
# stub_epd.h, qui n'expose que le framebuffer et deux primitives de dessin.
#
# Ce que ces tests NE couvrent PAS, et qui reste du ressort des bancs materiels :
# la pile TLS, le client MQTT, la flash, et le panneau lui-meme.

RACINE := ../..

CC      := cc
CFLAGS  := -std=c11 -Wall -Wextra -Wno-unused-parameter -O1 -g \
           -I. -I$(RACINE) -I$(RACINE)/json
# Le bouchon prend la place de screen/epd_driver.h pour les modules qui l'incluent.
CFLAGS  += -include stub_epd.h -D'EDEL_CHECK_PICO_EPD_DRIVER_H'

SOURCES := test_main.c \
           $(RACINE)/image/image_rx.c \
           $(RACINE)/profiles/profiles.c \
           $(RACINE)/screen/epd_text.c \
           $(RACINE)/json/frozen.c

BINAIRE := run_tests

.PHONY: test clean
test: $(BINAIRE)
	@./$(BINAIRE)

$(BINAIRE): $(SOURCES) stub_epd.h
	@$(CC) $(CFLAGS) -o $@ $(SOURCES)

clean:
	@rm -f $(BINAIRE)
