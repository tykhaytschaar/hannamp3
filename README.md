# hannamp3

ESP32-S3 alapú hordozható MP3-lejátszó: 3.5" érintős TFT, microSD-ről játszik,
I2S DAC-ra. ESP-IDF (v5.3) projekt, az UI LVGL 9.

## Hardver

ESP32-S3-DevKitC-1 **N16R8** (16 MB flash, 8 MB OPI PSRAM). Részletes
bekötés: [PINOUT.md](PINOUT.md), hátralévő bekötések: [WIRING_TODO.md](WIRING_TODO.md).

- **ST7796U 3.5" 480×320 TFT** — SPI2 (80 MHz), a microSD-vel közös buszon
- **FT6336 kapacitív touch** — külön I2C busz, polling (INT nincs bekötve)
- **microSD** — a kijelzőmodul foglalata, közös SPI busz (SD-órajel 10 MHz)
- **PCM5102A I2S DAC** — XSMT soft-mute GPIO 21-ről (klikkmentes track-váltás)
- **Háttérvilágítás** — GPIO 16, LEDC PWM 20 kHz (a panel saját
  tranzisztoros driverét hajtja)
- **6 nyomógomb + lock tolókapcsoló** — Menu / Next / Prev / Play-Pause /
  Vol± / Lock

## Funkciók

- **MP3-lejátszás** microSD-ről, ID3v2/ID3v1 tagek (cím / előadó / album)
- **Album art** a Now Playing képernyőn: ha a track mappájában pontosan egy
  `.jpg`/`.jpeg` van, az lesz az art; ha több, név-egyezés dönt
  (`<tracknév>.jpg` → `cover.jpg` → `folder.jpg`). Ha nincs / nem
  dekódolható → flash-be ágyazott placeholder. A dekódolás `esp_new_jpeg`-gel
  PSRAM-ba történik, a nagy borítókat 1/2–1/8 arányban kicsinyíti
- **Három képernyő** (swipe-pal vagy Menu gombbal váltható):
  - *Now Playing* — album art, cím, előadó · album, progress, transport
    gombok, görgethető album-tracklista (tap = lejátszás)
  - *Library* — mappa-böngésző (almappák + MP3-ak, `..` sorral). Zenés
    mappákban a lista tetején **„Play all"** sor: az albumot az elsőtől
    indítja. Ha a mappában `.m3u`/`.m3u8` playlist van, az veszi át a
    „Play all" szerepét, és a lejátszási sorrendet a playlist adja —
    albumokon átívelő listák is működnek (a relatív utak az m3u mappájához
    képest értendők, a hibás sorok kimaradnak)
  - *Settings* — Brightness-slider, Display off,
    Sleep, Album end (Stop / Repeat / Next album — utóbbi az album
    testvérmappái közt lép abc-sorrendben, körbefordulva; az egy mappából
    építkező m3u albumként viselkedik, az albumokon átívelő m3u végén stop).
    Next album módban a kézi Next/Prev is albumhatáron lép át a lista szélén
    (több-mappás m3u-nál ott no-op). USB Storage gomb
- **Perzisztens beállítások** (NVS): hangerő, fényerő, display-off timeout,
  sleep engedély
- **Energiagazdálkodás**: display-off tétlenség után (10/15/30 s / never,
  háttérvilágítás-duty 0), opcionális deep sleep — ébresztés a Menu gomb
  500 ms-os nyomva tartásával
- **Boot splash**: flash-be ágyazott JPEG frame-animáció
  (`assets/boot/frame_*.jpg`, `esp_new_jpeg` dekód, max ~30 fps)

## USB MSC mód (USB Storage)

Az SD kártya külső, írható-olvasható meghajtóként jelenik meg a natív USB
porton (GPIO 19/20) — MP3-másoláshoz nem kell kivenni a kártyát.

- **Belépés**: Settings → *USB Storage* sor, vagy `##usb##` CLI-parancs.
  Egyszer-használatos boot-flag áll be, az eszköz újraindul, és a normál
  init helyett `usb_msc_run()` fut (TinyUSB MSC, a hosté az SD).
- **Kilépés**: a képernyőn lévő **Exit** gomb (újraindít), vagy
  power-cycle / reset. A flag belépéskor törlődik, így a következő boot
  mindig normál módú.
- MSC alatt az LVGL be van fagyasztva (közös SPI busz — a kijelzőre nem
  flushelhetünk, amíg a host az SD-t használja).
- **Megkötés**: egyszerre csak az egyik USB-port használható — a natív
  USB-n az MSC meghajtó, a COMM/UART porton a flash/monitor/CLI. Egy
  kábellel nem megy egyszerre a kettő.

## CLI (UART0, 115200)

Keretezés: `##cmd##` vagy `##cmd$$payload##`. Ha a kijelző alszik, az első
parancs csak ébreszt (nem hajtódik végre) — mint a gomboknál.

| Parancs | Payload | Funkció |
|---|---|---|
| `play` / `pause` / `stop` | — | Transport |
| `next` / `prev` | — | Track-váltás |
| `screen` | `next` / `prev` | Képernyőváltás |
| `vol` | `up` / `down` / `max` / `off` / `0–100` | Hangerő |
| `bl` | `up` / `down` / `max` / `off` / `0–100` | Fényerő (NVS-be ment) |
| `sdtest` | — | SD nyers olvasási sebesség mérése |
| `usb` | — | Újraindulás USB MSC módba |

Interaktív REPL: [`tools/mp3ctl.py`](tools/mp3ctl.py) (pyserial kell) —
kétpaneles curses TUI, `port close/open` helyi paranccsal flasheléshez.

## Build & flash

```sh
tools/build.sh              # Docker-es idf.py build (espressif/idf:v5.3.1)
tools/build.sh menuconfig   # interaktív menuconfig a konténerben
```

A flashelés a hoston történik esptool-lal, a `build/flasher_args.json`
offsetjeivel (a UART/COMM porton át).
