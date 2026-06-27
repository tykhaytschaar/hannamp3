# Hardver bekötési TODO

Szoftverben már implementált funkciók, amik még fizikai bekötésre várnak.

## Gombok — 8 db, SNES-layout (bekötés a [PINOUT.md](PINOUT.md)-ben)

A gombkiosztás átállt 8 gombra (D-pad + A/B/X/Y). A GB emulátorban **X = Start,
Y = Select**. Bekötés és lábak: lásd PINOUT.md. CLI-ből játék közben:
`##a## s`, `##b##`, `##x##` (=Start), `##y##` (=Select); a `##start##`/
`##select##` aliasok az X/Y-ra mennek.

## TODO — fizikai gombos kilépés a játékból

A Menu gomb megszűnt, így a CHIP-8/GB játékból **egyelőre csak a fejléc touch
„Exit" gombjával** lehet kilépni. Kell egy fizikai gomb-kombó is (pl. **X+Y**
vagy **Bal+Jobb** együtt) → `game_request_exit()` / `gbmode_request_exit()`.
A kilépés-kérő API már megvan (`game.c` / `gb.c`), csak a kombó-detektálás
hiányzik (`player.c` game-mód ága, vagy egy külön poll).

## TODO — lakat (lock) gomb hosszú nyomásra

A lakat-tolókapcsoló megszűnt. A lock-állapotot egy gomb hosszú nyomása
billentse (`io_is_locked()` egyelőre fix `false`; a `lock_cb` + `ui_set_locked`
plumbing megvan, csak a trigger hiányzik).

## Kész (bekötve, a [PINOUT.md](PINOUT.md) dokumentálja)

- **Háttérvilágítás — GPIO 16**: a panel LED lába GPIO 16-on, LEDC PWM
  (20 kHz, 0–100%, Settings / CLI `bl`), idle alatt duty 0.
- **Onboard 3V3 LDO eltávolítva**: a buck-boost 3.3 V-ja közvetlenül a `3V3`
  lábra megy, USB csak adat — nincs 3.3 V-ütközés.

## PCM5102 XSMT — GPIO 21 (kész, ha működik a track-váltási mute)

Ha a klikk-mentes track-váltáshoz a XSMT-t GPIO 21-re kötötted és a
modul 3V3 → XSMT hidat eltávolítottad, ez kész. Ha még nincs:

1. A PCM5102 modulon a XSMT láb mellett egy 0Ω SMD ellenállás vagy
   forrasztott jumper köti 3V3-ra. Ezt el kell távolítani.
2. XSMT láb → ESP32-S3 **GPIO 21**.
