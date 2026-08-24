#!/usr/bin/env bash
# Compile le banc "ecran pilote par l'API" et le televerse.
#
#   WIFI_SSID="mon-hotspot" WIFI_PASSWORD="motdepasse" ./flash.sh
#   ./flash.sh --build       compile seulement
#   ./flash.sh --log         televerse puis ouvre la console serie
#
# Les identifiants Wi-Fi ne sont JAMAIS ecrits dans un fichier de ce depot : ils passent
# par l'environnement et finissent uniquement dans le binaire. Utiliser un mot de passe
# de hotspot temporaire, pas celui d'une box.
#
# Le Pico 2 W est 2,4 GHz UNIQUEMENT. Un hotspot iPhone doit avoir
# "Maximiser la compatibilite" active, un hotspot Android doit etre force en 2,4 GHz.

set -euo pipefail
cd "$(dirname "$0")"

: "${PICO_SDK_PATH:=$HOME/pico-sdk}"
export PICO_SDK_PATH

if [ -z "${WIFI_SSID:-}" ] || [ -z "${WIFI_PASSWORD:-}" ]; then
    echo "erreur : definir WIFI_SSID et WIFI_PASSWORD" >&2
    echo "  exemple : WIFI_SSID=\"mon-hotspot\" WIFI_PASSWORD=\"...\" ./flash.sh" >&2
    exit 1
fi

PICOTOOL=""
for c in "$HOME/.local/bin/picotool" "$(command -v picotool || true)"; do
    if [ -n "$c" ] && [ -x "$c" ] && ! "$c" info 2>&1 | grep -q "without USB support"; then
        PICOTOOL="$c"; break
    fi
done

echo "==> compilation (SSID : $WIFI_SSID)"
cmake -B build -G Ninja -S . \
      -DWIFI_SSID="$WIFI_SSID" -DWIFI_PASSWORD="$WIFI_PASSWORD" >/dev/null
cmake --build build

UF2="build/api_screen.uf2"
echo "==> $UF2 ($(du -h "$UF2" | cut -f1))"
[ "${1:-}" = "--build" ] && exit 0

find_mount() {
    for m in /media/"$USER"/RP2350 /media/"$USER"/RPI-RP2 \
             /run/media/"$USER"/RP2350 /run/media/"$USER"/RPI-RP2; do
        [ -d "$m" ] && { echo "$m"; return 0; }
    done
    return 1
}

if ! find_mount >/dev/null; then
    if [ -n "$PICOTOOL" ]; then
        echo "==> bascule en BOOTSEL via picotool"
        "$PICOTOOL" reboot -f -u 2>/dev/null || echo "    (deja en BOOTSEL, ou debranche)"
    else
        echo "==> passer la carte en BOOTSEL a la main (maintenir BOOTSEL, rebrancher)"
    fi
    echo -n "==> attente du volume BOOTSEL "
    for _ in $(seq 1 60); do find_mount >/dev/null && break; echo -n "."; sleep 2; done
    echo
fi

MOUNT="$(find_mount || true)"
[ -z "$MOUNT" ] && { echo "erreur : aucun volume RP2350." >&2; exit 1; }

echo "==> televersement vers $MOUNT"
cp "$UF2" "$MOUNT/"
sync
echo "==> fait."

if [ "${1:-}" = "--log" ]; then
    echo -n "==> attente de /dev/ttyACM0 "
    for _ in $(seq 1 30); do [ -e /dev/ttyACM0 ] && break; echo -n "."; sleep 1; done
    echo
    exec picocom -b 115200 /dev/ttyACM0
fi
echo
echo "    console : picocom -b 115200 /dev/ttyACM0   (Ctrl-A Ctrl-X pour quitter)"
