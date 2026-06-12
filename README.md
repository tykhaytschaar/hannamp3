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
  - *Library* — mappa-böngésző (almappák + MP3-ak, `..` sorral); a gyökere
    a `/sdcard/music` — a kártya többi része (games, egyéb) nem látszik. Zenés
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
    (több-mappás m3u-nál ott no-op). Games és USB Storage gomb
- **CHIP-8 játékok** SD-kártyáról (lásd lent)
- **Perzisztens beállítások** (NVS): hangerő, fényerő, display-off timeout,
  sleep engedély
- **Energiagazdálkodás**: display-off tétlenség után (10/15/30 s / never,
  háttérvilágítás-duty 0), opcionális deep sleep — ébresztés a Menu gomb
  500 ms-os nyomva tartásával
- **Boot splash**: flash-be ágyazott JPEG frame-animáció
  (`assets/boot/frame_*.jpg`, `esp_new_jpeg` dekód, max ~30 fps)

## CHIP-8 játékok (Games)

Beépített CHIP-8 emulátor (`chip8.c` — tiszta C VM, `game.c` — game mode UI):
a `.ch8` ROM-ok a kártya **`/sdcard/games`** mappájából futnak, public domain
klasszikusok (Space Invaders, Pong, Tetris, Brix, ...) tucatjával elérhetők,
fájlonként pár KB.

- **Indítás**: Settings → *Games* sor (választólista a `/sdcard/games`
  tartalmából).
- **Kijelző**: felül fejléc-sáv (Exit gomb, játéknév, bíp-jelző), alatta a
  64×32-es játéktér 7×-es nagyítással (448×224). Hang helyett (1. fázis) a
  fejléc-jelző villan, amíg a ROM "bípel".
- **Vezérlés**: Prev/Next = 4/6 (bal/jobb), Play = 5 (tűz/akció),
  Vol± = 2/8 (fel/le) — a klasszikusok zömének ez a kiosztása. A gombokra
  kötött CLI-parancsok (`next`/`prev`/`play`/`vol up`/`vol down`) játék
  közben ugyanezeket a kulcsokat ütik (rövid tapként). Kilépés: a fejléc
  **Exit** gombja (fallback: Menu gomb).
- **Zene**: a játék indításakor a lejátszás leáll (stop — nincs automatikus
  folytatás kilépéskor). Játék közben a display-off és a deep sleep nem
  aktiválódik.
- **Időzítés**: a frame-határ adaptív — egy rajzolási szakasz (sűrűn követő
  sprite-rajzolások, pl. az Invaders teljes rácsmenete) egy frame-en belül
  fut le, a játékciklus viszont frame-enként egy iterációt halad. A tempó
  így nem függ az utasítás-büdzsétől; a `gips` CLI-parancs csak a rajzolás
  nélküli frame-ek plafonját állítja. Game mode alatt az LVGL 60 fps-sel
  frissít (egyébként 30), és csak a változott területeket rajzolja újra
  (dirty-rect lista a VM-ből).
- A ROM betöltés után RAM-ból fut (nincs SD-hozzáférés játék közben);
  a VM quirk-profilja CHIP-48/SCHIP (a 90-es évekbeli játékpakkok ezt várják).

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
| `gips` | `5–200` | CHIP-8 utasítás-büdzsé a rajzolás nélküli frame-ekre |
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
