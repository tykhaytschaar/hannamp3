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
// SPI2 — TFT (ST7789V) + SD közös busz
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
// Gombok (mind GND-re zárnak, belső pull-up)
// -----------------------------------------------------------------------------
// MENU GPIO 1-en (RTC-capable), hogy deep sleep-ből EXT1 wake-forrás lehessen.
// PLAY átkerült GPIO 41-re.
#define PIN_BTN_MENU        GPIO_NUM_1
#define PIN_BTN_NEXT        GPIO_NUM_2
#define PIN_BTN_PREV        GPIO_NUM_42
#define PIN_BTN_PLAY        GPIO_NUM_41
#define PIN_BTN_VOL_UP      GPIO_NUM_38
#define PIN_BTN_VOL_DOWN    GPIO_NUM_39

// Lakat tolókapcsoló (slide switch) — egyik állása GND felé zár (LOW = lock).
// Pull-up belül, polling 100 ms-onként debounce-szal.
#define PIN_LOCK_SWITCH     GPIO_NUM_17

// -----------------------------------------------------------------------------
// Akku ADC: 100k:100k osztón át az 18650 (+) lábról
// -----------------------------------------------------------------------------
#define PIN_BAT_ADC         GPIO_NUM_4    // ADC1 CH3
#define BAT_DIVIDER_RATIO   2.0f          // (R1+R2)/R2 = 200k/100k
#define BAT_FULL_MV         4200
#define BAT_EMPTY_MV        3300

// -----------------------------------------------------------------------------
// Display méret
// -----------------------------------------------------------------------------
// Fekvő tájolás (90° CCW)
#define LCD_H_RES           320
#define LCD_V_RES           240
#define LCD_SPI_HZ          (2 * 1000 * 1000)

// -----------------------------------------------------------------------------
// SD mount + zenék
// -----------------------------------------------------------------------------
#define SD_MOUNT_POINT      "/sdcard"
#define MUSIC_DIR           "/sdcard/music"
#define MAX_TRACKS          128
#define MAX_DIR_ENTRIES     128
#define MAX_PATH_LEN        256

// -----------------------------------------------------------------------------
// Audio
// -----------------------------------------------------------------------------
#define MP3_READ_BUF_SIZE   (8 * 1024)
#define MP3_OUT_BUF_SAMPLES 1152          // Helix maximum frame
#define MP3_OUT_BUF_BYTES   (MP3_OUT_BUF_SAMPLES * 2 * sizeof(int16_t))
