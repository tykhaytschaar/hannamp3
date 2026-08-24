#pragma once

#include "driver/gpio.h"

// -----------------------------------------------------------------------------
// I2S → PCM5102A
// -----------------------------------------------------------------------------
#define PIN_I2S_BCK         GPIO_NUM_5
#define PIN_I2S_LRCK        GPIO_NUM_6
#define PIN_I2S_DIN         GPIO_NUM_7
// PCM5102 XSMT: HIGH = unmute, LOW = soft mute (~104 BCLK = ~2.4 ms ramp).
// Track-váltáskor mute-oljuk, hogy a BCLK gating-ből származó analóg
// klikkek ne hallatsszanak.
#define PIN_XSMT            GPIO_NUM_21

// -----------------------------------------------------------------------------
// SPI2 — TFT (ST7796U) + SD közös busz
// -----------------------------------------------------------------------------
#define PIN_SPI_SCK         GPIO_NUM_12
#define PIN_SPI_MOSI        GPIO_NUM_11
#define PIN_SPI_MISO        GPIO_NUM_13   // csak az SD-nek kell
#define PIN_TFT_CS          GPIO_NUM_10
#define PIN_TFT_DC          GPIO_NUM_9
#define PIN_TFT_RST         GPIO_NUM_8
#define PIN_SD_CS           GPIO_NUM_14
// Háttérvilágítás vezérlő: a panel BLK/LED+ lábát kötjük ide.
// A modul 3V3 → BLK hidat (0Ω jumper) el kell távolítani.
#define PIN_BL              GPIO_NUM_16

// -----------------------------------------------------------------------------
// Kapacitív touch (FT6336) — külön I2C busz, polling (nincs INT láb)
// CTP_SDA→15, CTP_SCL→18, CTP_RST→47. CTP_INT nincs bekötve (GPIO 40 foglalt).
// -----------------------------------------------------------------------------
#define PIN_TOUCH_SDA       GPIO_NUM_15
#define PIN_TOUCH_SCL       GPIO_NUM_18
#define PIN_TOUCH_RST       GPIO_NUM_47

// -----------------------------------------------------------------------------
// Gombok (mind GND-re zárnak, belső pull-up) — 8 gomb, SNES-layout:
// D-pad (panel bal oldal) + A/B/X/Y (panel jobb oldal). Nincs Menu/Start/Select
// és nincs lakat-tolókapcsoló (a lock később egy gomb hosszú nyomására kerül).
// Player-leképezés: A = play/pause, Right/Left = next/prev (Library: be/fel),
// Up/Down = hangerő (Library: kurzor); B/X/Y a playerben (még) no-op.
// Strap-lábat egyik gomb sem használ (0/45/46 szabadon) → bootkor nincs gond,
// kivéve a GPIO 3-at (JTAG-sel strap, de égetetlen eFuse mellett közömbös, és
// a régi firmware már bizonyítottan használta gombnak).
// -----------------------------------------------------------------------------
// D-pad — panel bal oldal (a 17/3 a bal headeren, a 2/1 a jobbról áthúzva):
#define PIN_BTN_UP          GPIO_NUM_17    // bal header; RTC → deep sleep wake
#define PIN_BTN_DOWN        GPIO_NUM_3     // bal header (JTAG-sel strap, OK)
#define PIN_BTN_LEFT        GPIO_NUM_1     // jobb header, vezeték áthúzva
#define PIN_BTN_RIGHT       GPIO_NUM_2     // jobb header, vezeték áthúzva
// Akciógombok — panel jobb oldal (mind a jobb headeren). A GB emulátorban
// X = Start, Y = Select (lásd gb.c), hogy a Game Boy játszható maradjon.
#define PIN_BTN_A           GPIO_NUM_39
#define PIN_BTN_B           GPIO_NUM_38
#define PIN_BTN_X           GPIO_NUM_42
#define PIN_BTN_Y           GPIO_NUM_41

// -----------------------------------------------------------------------------
// Akku ADC: 100k:100k osztón át a 104050 LiPo cella (+) lábáról
// -----------------------------------------------------------------------------
#define PIN_BAT_ADC         GPIO_NUM_4    // ADC1 CH3
#define BAT_DIVIDER_RATIO   2.0f          // (R1+R2)/R2 = 200k/100k
#define BAT_FULL_MV         4200
#define BAT_EMPTY_MV        3300
// Boot-gate: ha a mért töltöttség <= ennyi %, nem bootolunk — csak egy üres-
// akku jel 3 mp-ig, majd vissza deep sleepbe (lásd main.c low_battery_gate).
#define BAT_SHUTOFF_PCT     1

// -----------------------------------------------------------------------------
// Display méret — ST7796U 3.5", natív 320×480 portré, fekvőben 480×320
// -----------------------------------------------------------------------------
// Fekvő tájolás (90° CCW): swap_xy a panelen, lásd ui.c disp_cfg
#define LCD_H_RES           480
#define LCD_V_RES           320
// 2 MHz a ST7789-en a dupont-bekötés miatt volt; a ~1.5× nagyobb panelnél
// használhatatlanul lassú lenne (~1 s/teljes frame). A SCK/MOSI (12/11) a
// SPI2 IOMUX lábai → 80 MHz is megengedett. Tiszta osztók a 80 MHz APB-ből:
// 80/40/26.7/20. Ha csíkozódik/„hó"/hibás sor van a közös buszon, vidd
// vissza 40-re (a dupont-bekötés a 80 határán van).
#define LCD_SPI_HZ          (80 * 1000 * 1000)

// -----------------------------------------------------------------------------
// SD mount + zenék
// -----------------------------------------------------------------------------
#define SD_MOUNT_POINT      "/sdcard"
#define MUSIC_DIR           "/sdcard/music"
#define GAMES_DIR           "/sdcard/games"   // Game Boy .gb/.gbc ROM-ok (Játékok oldal)
#define MAX_TRACKS          128
#define MAX_DIR_ENTRIES     128
#define MAX_PATH_LEN        256

// -----------------------------------------------------------------------------
// Audio
// -----------------------------------------------------------------------------
#define MP3_READ_BUF_SIZE   (8 * 1024)
#define MP3_OUT_BUF_SAMPLES 1152          // Helix maximum frame
// Out-buffer méret: MP3-nál csak 4608 byte / frame, de WAV-nál nagyobb
// chunk-okat olvasunk (kevesebb SD/SPI round-trip) — 8 KB ≈ 46 ms @ 44.1k
// stereo. Mono WAV duplikációval éppen kifut a 8 KB-ig.
#define WAV_CHUNK_BYTES     (8 * 1024)
#define MP3_OUT_BUF_BYTES   WAV_CHUNK_BYTES
