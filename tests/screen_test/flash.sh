#!/usr/bin/env bash
# Compile le banc de test ecran et le televerse.
#
#   ./flash.sh              compile, met la carte en BOOTSEL si possible, televerse
#   ./flash.sh --build      compile seulement
#   ./flash.sh --log        televerse puis ouvre la console serie
#
# Prerequis : PICO_SDK_PATH exporte, arm-none-eabi-gcc installe.
#
# Si un picotool avec support USB est disponible, la carte est basculee en
# BOOTSEL toute seule : plus besoin de toucher au bouton. Sinon, le script
# attend qu'on le fasse a la main.
#
# ATTENTION : le picotool que le SDK compile tout seul est bati avec
# -DPICOTOOL_NO_LIBUSB=1 et ne sait PAS parler a la carte. Il en faut un
# compile separement (voir docs/ARCHITECTURE.md section 9.1).

set -euo pipefail
cd "$(dirname "$0")"

: "${PICO_SDK_PATH:=$HOME/pico-sdk}"
export PICO_SDK_PATH

if [ ! -f "$PICO_SDK_PATH/external/pico_sdk_import.cmake" ]; then
    echo "erreur : PICO_SDK_PATH invalide ($PICO_SDK_PATH)" >&2
    exit 1
fi

# --- picotool avec support USB ? -------------------------------------------
PICOTOOL=""
for candidate in "$HOME/.local/bin/picotool" "$(command -v picotool || true)"; do
    if [ -n "$candidate" ] && [ -x "$candidate" ] \
       && ! "$candidate" info 2>&1 | grep -q "without USB support"; then
        PICOTOOL="$candidate"
        break
    fi
done

# --- compilation -----------------------------------------------------------
echo "==> compilation (SDK : $PICO_SDK_PATH)"
cmake -B build -G Ninja -S . >/dev/null
cmake --build build

UF2="build/screen_test.uf2"
echo "==> $UF2 ($(du -h "$UF2" | cut -f1))"

if [ "${1:-}" = "--build" ]; then
    exit 0
fi

# --- recherche d'un volume BOOTSEL -----------------------------------------
find_mount() {
    for m in /media/"$USER"/RP2350  /media/"$USER"/RPI-RP2 \
             /run/media/"$USER"/RP2350 /run/media/"$USER"/RPI-RP2; do
        [ -d "$m" ] && { echo "$m"; return 0; }
    done
    return 1
}

if ! find_mount >/dev/null; then
    if [ -n "$PICOTOOL" ]; then
        echo "==> bascule de la carte en BOOTSEL via $PICOTOOL"
        # -f : force meme si la carte execute du code ; -u : rester en BOOTSEL
        "$PICOTOOL" reboot -f -u 2>/dev/null || \
            echo "    (echec : la carte est peut-etre deja en BOOTSEL, ou debranchee)"
    else
        echo "==> pas de picotool USB : passer la carte en BOOTSEL a la main"
        echo "    (maintenir BOOTSEL, debrancher/rebrancher l'USB, relacher)"
    fi

    echo -n "==> attente du volume BOOTSEL "
    for _ in $(seq 1 60); do
        find_mount >/dev/null && break
        echo -n "."
        sleep 2
    done
    echo
fi

MOUNT="$(find_mount || true)"
if [ -z "$MOUNT" ]; then
    echo "erreur : aucun volume RP2350 apparu." >&2
    exit 1
fi

echo "==> televersement vers $MOUNT"
cp "$UF2" "$MOUNT/"
sync
echo "==> fait. La carte redemarre et la demo tourne en boucle."

# --- console serie optionnelle ---------------------------------------------
if [ "${1:-}" = "--log" ]; then
    echo -n "==> attente de /dev/ttyACM0 "
    for _ in $(seq 1 30); do
        [ -e /dev/ttyACM0 ] && break
        echo -n "."
        sleep 1
    done
    echo
    exec picocom -b 115200 /dev/ttyACM0
else
    echo
    echo "    console serie :  picocom -b 115200 /dev/ttyACM0    (Ctrl-A Ctrl-X pour quitter)"
    echo "    ou directement : ./flash.sh --log"
fi
