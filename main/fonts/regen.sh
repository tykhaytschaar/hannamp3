#!/usr/bin/env bash
#
# Inter Medium (500) + LVGL FontAwesome szimbólumok 12/14/18 pt-en.
#
# Karakter-tartomány:
#   0x20-0x7E     ASCII printable
#   0xA0-0xFF     Latin-1 supplement (á é í ó ö ú ü stb.)
#   0x150-0x151   Ő ő
#   0x170-0x171   Ű ű
#   FontAwesome:  AUDIO, REFRESH, MUTE, VOL_MID, VOL_MAX, PREV, PLAY, PAUSE,
#                 NEXT, DIRECTORY, BATTERY_FULL..EMPTY  (lásd lv_symbol_def.h)
#
# Előfeltételek (egyszer):
#   npm install -g lv_font_conv
#   npm install @fontsource/inter @fortawesome/fontawesome-free
#   pip3 install fonttools brotli   # woff2 -> ttf konverzióhoz
#
# Használat:
#   cd main/fonts && ./regen.sh
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cd "$TMP"
echo "[1/3] Letöltöm a font csomagokat (npm)…"
npm init -y >/dev/null
npm install --no-audit --no-fund @fontsource/inter @fortawesome/fontawesome-free lv_font_conv >/dev/null

echo "[2/3] woff2 → ttf konverzió…"
python3 - <<'PY'
from fontTools.ttLib import TTFont
for src, dst in [
    ("node_modules/@fontsource/inter/files/inter-latin-500-normal.woff2",  "inter-500-latin.ttf"),
    ("node_modules/@fontsource/inter/files/inter-latin-ext-500-normal.woff2", "inter-500-latin-ext.ttf"),
    ("node_modules/@fortawesome/fontawesome-free/webfonts/fa-solid-900.woff2", "fa-solid-900.ttf"),
]:
    f = TTFont(src); f.flavor = None; f.save(dst)
PY

echo "[3/3] LVGL font generálás (bpp=4)…"
SYMBOLS="0xF001,0xF021,0xF026-0xF028,0xF048,0xF04B-0xF04C,0xF051,0xF07B,0xF240-0xF244"
LATIN_BASIC="0x20-0x7E,0xA0-0xFF"
LATIN_EXT="0x0150-0x0151,0x0170-0x0171"

for SIZE in 12 14 18; do
    echo "  - mp3_inter_${SIZE}"
    npx lv_font_conv \
        --bpp 4 --size "$SIZE" \
        --font inter-500-latin.ttf      -r "$LATIN_BASIC" \
        --font inter-500-latin-ext.ttf  -r "$LATIN_EXT" \
        --font fa-solid-900.ttf         -r "$SYMBOLS" \
        --format lvgl --lv-include lvgl.h \
        -o "$SCRIPT_DIR/mp3_inter_${SIZE}.c"
done

echo
echo "Kész. Generált fájlok:"
ls -la "$SCRIPT_DIR"/mp3_inter_*.c
