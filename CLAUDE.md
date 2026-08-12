# hannamp3 — ESP32-S3 MP3-lejátszó (+ Game Boy emulátor)

Hordozható MP3/WAV-lejátszó ESP32-S3-on: ST7796 TFT + FT6336 touch, PCM5102A
DAC (I2S), SD-kártya, 8 gomb, 18650 akku. LVGL 9 UI, magyar feliratokkal.
Mellette Game Boy emulátor (peanut_gb) a `/sdcard/games` ROM-okra.

## Build és flash

- **Az idf.py NINCS a hoston** — a build Dockerben fut: `tools/build.sh` (image:
  `espressif/idf:release-v5.3`, lásd a script kommentjét, miért nem tag-elt
  verzió). Flash a hoston esptool-lal (`/esp-flash` skill).
- Egy USB-kábel van: a natív USB (MSC-meghajtó) VAGY a UART port használható
  egyszerre — flash/monitor közben nem látszik az MSC-meghajtó.
- Flash után a user teszteli a panelen; commit csak az ő megerősítése után.

## Kritikus architektúra: közös SPI2 busz (SD + TFT)

**Ez a projekt legkényesebb pontja — minden SD- és kijelző-érintő változtatásnál
gondold végig.**

- Az SD-kártya (sdspi, 10 MHz) és az ST7796 (esp_lcd, 80 MHz) ugyanazon az
  SPI2 buszon van. A buszt az `sd_init()` inicializálja (main.c-ben a sorrend
  kötött: `sd_init()` → `ui_init()`).
- Az alkalmazás-szintű szerializálást az **LVGL portlock** adja:
  `ui_spi_lock()`/`ui_spi_unlock()` = `lvgl_port_lock/unlock` (rekurzív mutex).
  **Minden SD-elérésnek (fread/opendir/stat) e lock alatt kell futnia**, ha
  párhuzamosan LVGL-flush is futhat. Az audio task (audio.c) minden freadje
  lockolt; a player.c böngésző-útvonalai (sd_list_dir, sd_load_dir_tracks,
  sd_load_m3u_tracks, advance_album) viszont lock NÉLKÜL futnak — ezek csak
  azért nem ütköznek, mert user-akcióhoz kötöttek.
- **A lock nem fedi a flush "farkát"**: az esp_lcd a flush-t a busz
  `max_transfer_sz`-e (4096 B) szerint ~10 darab queued/interrupt SPI-
  tranzakcióra vágja, amelyek DMA-val, ISR-ből mennek ki **a mutex elengedése
  után is**. Az sdspi közben minden SD-parancsnál
  `spi_device_acquire_bus(portMAX_DELAY)`-t hív → a polling(SD) és az
  interrupt(LCD) tranzakciók arbitrációja az IDF `spi_bus_lock`-jára hárul.
- **Ismert IDF-hiba**: az 5.3.1…5.3.5 (és 5.4.x) spi_bus_lock-ja hibásan
  ütemezett, ha az egyik eszköz acquire-ölt, miközben egy MÁSIK eszköz
  háttér-(ISR/DMA-)tranzakciói még aktívak voltak → teljes fagyás (a mutexet
  birtokló task beragad, minden más rá vár). Javítás: `b42734af`
  (release/v5.3, 2026-06) — ezért fut a build a `release-v5.3` image-en.
  Tünet volt: lejátszás közben a Lejátszás oldali lista görgetésére fagyás.

## Task-topológia

| Task | Core | Prio | Mit csinál |
|---|---|---|---|
| audio | 1 | 10 | Helix MP3-dekód / WAV-olvasás, I2S-írás; SD-fread lock alatt |
| LVGL (esp_lvgl_port) | 0 | 4 | lv_timer_handler a portlock alatt; touch-indev 10 ms-onként |
| player | — | 4 | 200 ms-os poll: auto-next, progress (2 Hz), idle/sleep döntés |
| battery | — | 3 | 5 s-onként ADC + EMA |
| iot_button (esp_timer) | — | — | gomb-callbackek az esp_timer taskban futnak → `player_handle_button` onnan hívódik |

- A gomb/CLI/player hívások LVGL-t érintő része mindig `lvgl_port_lock` alá
  megy (a ui.c publikus API-jai maguk lockolnak).
- UI-listát újraépítő műveletet LVGL-eseménykezelőből csak `lv_async_call`-lal
  szabad indítani (a saját forrás-gombját törölné — use-after-free), lásd
  `lib_row_click` / `np_tlist_click` kommentjeit.

## UI-finomságok, amikbe már belefutottunk

- Az `lv_list_add_button` labelje defaultból `LV_LABEL_LONG_SCROLL_CIRCULAR`
  (folyamatos animáció = folyamatos render/flush!). A Könyvtár-lista ezt
  explicit `LONG_DOT`-ra írja felül (`browser_apply_cursor`); a Now Playing
  mini-lista nem — ez tudatos különbség, ha nyúlsz hozzá, nézd meg a
  fagyás-történetet.
- NVS-írás (nvs_commit) a flash-cache kikapcsolásával jár → lejátszás közben
  hangkimaradás. Ezért: mappa-navigáció NEM perzisztál, csak fájl-indítás
  (`browser_activate`), ahol a stall elrejthető. Hold-repeat gombról soha ne
  írj NVS-t.
- Track-váltáskor a teljes I2S-csatorna teardown kell (DMA-descriptorok régi
  PCM-tartalma miatt), plusz Helix dekóder-state reset — lásd audio.c
  kommentjeit. XSMT (DAC mute) + 100 ms-os szoftveres fade-in fedi a poppokat.
- A kijelző/touch tájolás, BGR-sorrend, szín-inverzió empirikus knobok —
  ui.c-ben „EMPIRIKUS KNOB" kommentek jelölik, ne „javítsd ki" őket vakon.

## Hardver-emlékeztetők

- Táp: buck-boost 3.3 V közvetlenül a 3V3 lábra (onboard LDO levéve), USB csak
  adat; az 5V lábra nem kötünk semmit.
- Pinout: `app_config.h` + `PINOUT.md`. GPIO 40 fixen LOW; a CS-lábakat a
  main.c már SPI-init előtt HIGH-ra húzza (kötelező sorrend).
- Deep sleep: Up gombról ébred (EXT1), 500 ms hold-gate a main.c-ben;
  háttérvilágítás GPIO-hold-dal lefogva alvás alatt.
