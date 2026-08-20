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
- **8 nyomógomb (SNES-layout)** — D-pad (Fel/Le/Bal/Jobb) + A/B/X/Y; lock
  később egy gomb hosszú nyomásával (lásd [WIRING_TODO.md](WIRING_TODO.md))

## Funkciók

- **MP3-lejátszás** microSD-ről, ID3v2/ID3v1 tagek (cím / előadó / album)
- **Album art** a Now Playing képernyőn: ha a track mappájában pontosan egy
  `.jpg`/`.jpeg` van, az lesz az art; ha több, név-egyezés dönt
  (`<tracknév>.jpg` → `cover.jpg` → `folder.jpg`). Ha nincs / nem
  dekódolható → flash-be ágyazott placeholder. A dekódolás `esp_new_jpeg`-gel
  PSRAM-ba történik, a nagy borítókat 1/2–1/8 arányban kicsinyíti
- **Négy képernyő** (a fejléc nyílgombjaival váltható):
  - *Now Playing* — album art, cím, előadó · album, progress, transport
    gombok, görgethető album-tracklista (tap = lejátszás)
  - *Library* — mappa-böngésző (almappák + MP3-ak, `..` sorral); a gyökere
    a `/sdcard/music` — a kártya többi része (games, egyéb) nem látszik. Zenés
    mappákban a lista tetején **„Play all"** sor: az albumot az elsőtől
    indítja. Ha a mappában `.m3u`/`.m3u8` playlist van, az veszi át a
    „Play all" szerepét, és a lejátszási sorrendet a playlist adja —
    albumokon átívelő listák is működnek (a relatív utak az m3u mappájához
    képest értendők, a hibás sorok kimaradnak)
  - *Játékok* — a `/sdcard/games` Game Boy / Game Boy Color ROM-jainak
    választólistája (lásd lent); a lista az oldalra belépéskor frissül
  - *Rendszer* (Settings) — Fényerő-slider, Kijelző ki,
    Alvás, Album vége (Stop / Ismétlés / Következő album — utóbbi az album
    testvérmappái közt lép abc-sorrendben, körbefordulva; az egy mappából
    építkező m3u albumként viselkedik, az albumokon átívelő m3u végén stop).
    Next album módban a kézi Next/Prev is albumhatáron lép át a lista szélén
    (több-mappás m3u-nál ott no-op). USB tároló gomb és Használati útmutató
    gomb (görgethető, beépített súgó)
- **Game Boy / Game Boy Color játékok** SD-kártyáról (lásd lent)
- **Perzisztens beállítások** (NVS): hangerő, fényerő, display-off timeout,
  sleep engedély
- **Energiagazdálkodás**: display-off tétlenség után (10/15/30 s / never,
  háttérvilágítás-duty 0), opcionális deep sleep — ébresztés a Fel (Up) gomb
  500 ms-os nyomva tartásával
- **Boot splash**: flash-be ágyazott JPEG frame-animáció
  (`assets/boot/frame_*.jpg`, `esp_new_jpeg` dekód, max ~30 fps)

## Game Boy / Game Boy Color játékok (Játékok oldal)

Vendorolt **Walnut-CGB** emulátormag (`main/walnut_cgb.h`, MIT; `gb.c` —
emuláció + játékképernyő, `game.c` — a Játékok oldal ROM-listája): a
`.gb`/`.gbc` ROM-ok a kártya **`/sdcard/games`** mappájából futnak.

- **Indítás**: a *Játékok* oldal listájából, vagy a Könyvtár `.gb`/`.gbc`
  soraira koppintva.
- **Kijelző**: bal sáv (Kilépés + pillanatkép Mentés/Betöltés), középen a
  160×144-es játéktér 2× nagyítással (320×288), jobb sáv (Start / Select
  touch-gombok).
- **Vezérlés**: D-pad + A/B a fizikai gombokon; X = Start, Y = Select
  (touchról is elérhetők).
- **Mentések**: battery-save (`.sav`) automatikusan a ROM mellé az SD-re;
  save state (`.state`) a Mentés/Betöltés gombokról — build-függő
  (ELF-SHA), firmware-frissítés érvényteleníti.
- **Kilépés**: a bal sáv **Kilépés** gombja — mindig a Játékok oldalra tér
  vissza (akkor is, ha a ROM a Könyvtárból indult).
- **Zene**: a játék indításakor a lejátszás leáll (stop — nincs automatikus
  folytatás kilépéskor). Játék közben a display-off és a deep sleep nem
  aktiválódik.
- Render-mód és emulátormag futás közben váltható CLI-ből
  (`##gbfs##` / `##gbcore##`); a ROM betöltés után PSRAM-ból fut.

## USB MSC mód (USB Storage)

Az SD kártya külső, írható-olvasható meghajtóként jelenik meg a natív USB
porton (GPIO 19/20) — MP3-másoláshoz nem kell kivenni a kártyát.

- **Belépés**: Rendszer → *USB tároló* gomb, vagy `##usb##` CLI-parancs.
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
| `gbfs` | — | GB render-mód váltás (adaptív → teljes → frameskip → interlace) |
| `gbcore` | — | GB emulátormag váltás (dualfetch ↔ eredeti 8 bites) |
| `sdtest` | — | SD nyers olvasási sebesség mérése |
| `usb` | — | Újraindulás USB MSC módba |

Interaktív REPL: [`tools/mp3ctl.py`](tools/mp3ctl.py) (pyserial kell) —
kétpaneles curses TUI, `port close/open` helyi paranccsal flasheléshez.

## Build & flash

```sh
tools/build.sh              # Docker-es idf.py build (espressif/idf:release-v5.3)
tools/build.sh menuconfig   # interaktív menuconfig a konténerben
```

A flashelés a hoston történik esptool-lal, a `build/flasher_args.json`
offsetjeivel (a UART/COMM porton át).
