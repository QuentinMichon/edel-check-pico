#!/usr/bin/env bash
# Compile le banc MQTT/TLS et le televerse.
#
#   WIFI_SSID="mon-hotspot" WIFI_PASSWORD="..." ./flash.sh
#   ./flash.sh --build      compile seulement
#   ./flash.sh --log        televerse puis ouvre la console serie
#
# Identifiants du boitier : lus dans le fichier JSON produit par l'appairage, dont le
# chemin est donne par CREDS. Ils ne sont JAMAIS ecrits dans un fichier de ce depot —
# ils finissent uniquement dans le binaire.
#
#   CREDS=/chemin/device-creds.json ./flash.sh
#
# Adresse du broker : par defaut l'IP de CE PC sur son interface par defaut, parce que
# c'est lui qui heberge la pile Docker.
#
#   ⚠ LE PICO ET LE PC DOIVENT ETRE SUR LE MEME RESEAU.
#     Le Pico 2 W est 2,4 GHz uniquement. Si le Pico est sur un hotspot telephone, le
#     PC doit rejoindre CE MEME hotspot — sinon l'IP detectee ci-dessous n'est pas
#     joignable depuis le boitier, et la connexion expire sans message clair.
#     Le script verifie ce point avant de compiler.

set -euo pipefail
cd "$(dirname "$0")"

: "${PICO_SDK_PATH:=$HOME/pico-sdk}"
export PICO_SDK_PATH

: "${CREDS:=$HOME/.edelcheck-device-creds.json}"
: "${BROKER_PORT:=8883}"

if [ -z "${WIFI_SSID:-}" ] || [ -z "${WIFI_PASSWORD:-}" ]; then
    echo "erreur : definir WIFI_SSID et WIFI_PASSWORD" >&2
    echo "  exemple : WIFI_SSID=\"mon-hotspot\" WIFI_PASSWORD=\"...\" ./flash.sh" >&2
    exit 1
fi

if [ ! -f "$CREDS" ]; then
    echo "erreur : identifiants du boitier introuvables a $CREDS" >&2
    echo "  les obtenir en jouant l'appairage, puis :" >&2
    echo "    CREDS=/chemin/vers/device-creds.json ./flash.sh" >&2
    exit 1
fi

read -r DEVICE_ID DEVICE_SECRET <<EOF
$(python3 -c "import json;d=json.load(open('$CREDS'));print(d['deviceId'],d['secret'])")
EOF

# IP de ce PC sur l'interface qui porte la route par defaut.
: "${BROKER_IP:=$(ip route get 1.1.1.1 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="src") print $(i+1)}' | head -1)}"

if [ -z "$BROKER_IP" ]; then
    echo "erreur : impossible de determiner l'IP de ce PC. Forcer avec BROKER_IP=..." >&2
    exit 1
fi

# Les deux verifications reseau ci-dessous n'ont de sens qu'avant un televersement.
# `--build` sert a verifier que ca compile, y compris depuis un autre reseau.
if [ "${1:-}" != "--build" ]; then

# Le PC est-il sur le meme reseau que celui ou ira le Pico ?
CUR_SSID="$(iwgetid -r 2>/dev/null || nmcli -t -f active,ssid dev wifi 2>/dev/null | awk -F: '$1=="yes"{print $2;exit}' || true)"
if [ -n "$CUR_SSID" ] && [ "$CUR_SSID" != "$WIFI_SSID" ]; then
    echo "⚠  CE PC est sur \"$CUR_SSID\", le Pico ira sur \"$WIFI_SSID\"." >&2
    echo "   Le boitier ne pourra pas joindre $BROKER_IP depuis un autre reseau." >&2
    echo "   Connecte ce PC a \"$WIFI_SSID\" puis relance, ou force BROKER_IP=..." >&2
    exit 1
fi

# Le broker repond-il vraiment sur cette adresse ?
if ! timeout 5 bash -c "</dev/tcp/$BROKER_IP/$BROKER_PORT" 2>/dev/null; then
    echo "⚠  rien n'ecoute sur $BROKER_IP:$BROKER_PORT." >&2
    echo "   La pile edelcheck tourne-t-elle ? (docker compose ps)" >&2
    exit 1
fi

fi   # fin des verifications reseau

echo "==> boitier : $DEVICE_ID"
echo "==> broker  : $BROKER_IP:$BROKER_PORT  (joignable)"
echo "==> wifi    : $WIFI_SSID"

cmake -B build -G Ninja -S . \
      -DWIFI_SSID="$WIFI_SSID" -DWIFI_PASSWORD="$WIFI_PASSWORD" \
      -DDEVICE_ID="$DEVICE_ID" -DDEVICE_SECRET="$DEVICE_SECRET" \
      -DBROKER_IP="$BROKER_IP" >/dev/null
cmake --build build

UF2="build/mqtt_test.uf2"
echo "==> $UF2 ($(du -h "$UF2" | cut -f1))"
[ "${1:-}" = "--build" ] && exit 0

PICOTOOL=""
for c in "$HOME/.local/bin/picotool" "$(command -v picotool || true)"; do
    if [ -n "$c" ] && [ -x "$c" ] && ! "$c" info 2>&1 | grep -q "without USB support"; then
        PICOTOOL="$c"; break
    fi
done

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
