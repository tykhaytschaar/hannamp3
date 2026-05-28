# Custom LVGL fontok — Inter Medium + magyar + FontAwesome szimbólumok

Az `lv_font_montserrat_*` alapból csak ASCII-t (0x20–0x7F) tartalmaz, ezért
sem `á/é/…`, sem `ő/ű` nem jelenik meg. Ez a komponens három méretben
(12, 14, 18 pt) szállítja az **Inter Medium** fontot a teljes magyar
karakterkészlettel és a `ui.c`-ben használt LVGL FontAwesome ikonokkal egybe.

## Fájlok

| Fájl | Méret | Forrás flash | Használat |
|------|-------|--------------|-----------|
| `mp3_inter_12.c` | 12 pt | ~70 KB | default (minden label örökli `apply_screen_bg`-n keresztül) |
| `mp3_inter_14.c` | 14 pt | ~78 KB | Now Playing subtitle |
| `mp3_inter_18.c` | 18 pt | ~96 KB | Now Playing title |
| `mp3_fonts.h`    | — | — | `extern` deklarációk a három fonthoz |

A forrásfájl-mérethez képest a binárisba kerülő tényleges flash impact
nagyjából 1/3 (bpp=4 + LVGL bitmap kódolás).

## Karakter-tartományok

- `0x20–0x7E` — ASCII printable
- `0xA0–0xFF` — Latin-1 supplement (`á é í ó ö ú ü Á É Í Ó Ö Ú Ü` stb.)
- `0x0150–0x0151` — `Ő ő`
- `0x0170–0x0171` — `Ű ű`
- LVGL FontAwesome szimbólumok: AUDIO, REFRESH, MUTE, VOL_MID, VOL_MAX,
  PREV, PLAY, PAUSE, NEXT, BATTERY_FULL..EMPTY (lásd `lv_symbol_def.h`)

Ha új szimbólumot kezdesz használni a UI-ban (pl. `LV_SYMBOL_LOOP` =
`0xF079`), bővítsd a `SYMBOLS` listát a `regen.sh`-ben és futtasd újra.

## Újragenerálás

```bash
cd main/fonts
./regen.sh
```

A script:

1. Letölti a forrás TTF-eket npm-ről (@fontsource/inter, @fortawesome/fontawesome-free)
2. woff2 → ttf konvertál fonttools-zal
3. lefuttatja `lv_font_conv`-t mindhárom méreten
4. felülírja a `.c` fájlokat ebben a mappában

Előfeltételek (egyszeri):
- Node + npm
- Python 3 + `pip install fonttools brotli`

## Ha másik fontot szeretnél

Cseréld le `Inter-Medium` helyett másik fontra a `regen.sh`-ben. Ha a font
nem támogatja külön a "latin-ext" és "latin" subset-eket (mint a fontsource),
egyetlen `--font … -r 0x20-0x7E,0xA0-0xFF,0x150-0x151,0x170-0x171` is jó.
