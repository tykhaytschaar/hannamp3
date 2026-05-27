# Pinkiosztás — hannamp3

ESP32-S3-DevKitC-1 N16R8 lábkiosztása a projekthez. A táblázatok a fizikai
bekötést tükrözik. A kódbéli definíciók a [`main/app_config.h`](main/app_config.h)
fájlban vannak.

## Gombok (6 db)

Minden gomb egyik lába a megfelelő GPIO-ra, a **diagonálisan átellenes** lába
GND-re. Belső pull-up engedélyezve, lenyomáskor LOW.

| Gomb | ESP32-S3 GPIO | Funkció |
|---|---|---|
| **Play / Pause** | **GPIO 1** | Lejátszás indítása vagy szüneteltetése |
| **Next** | **GPIO 2** | Következő szám |
| **Prev** | **GPIO 42** | Előző szám |
| **Menu** | **GPIO 41** | SD újraolvasás (későbbi: beállítások menü) |
| **Vol +** | **GPIO 38** | Hangerő +5% (tartva folyamatosan emel) |
| **Vol −** | **GPIO 39** | Hangerő −5% (tartva folyamatosan csökkent) |

A `Vol +` és `Vol −` támogat **hold-to-repeat**-et: nyomva tartva ~120 ms-onként
újra-kiadja az eventet.

## ST7789V kijelző + microSD slot

Megosztott SPI2 buszt használ a TFT és az SD. A panel hátoldali címkéi szerint:

| Panel láb | ESP32-S3 GPIO | Megjegyzés |
|---|---|---|
| GND | GND | |
| VCC | **5V** (USB / VIN) | A panelen lévő AMS1117-3V3 LDO-nak kell 5V bemenet, különben az SD insert mellett a rail beesik |
| SCL | GPIO 12 | SPI clock |
| SDA | GPIO 11 | SPI MOSI |
| RST | GPIO 8 | TFT reset |
| DC | GPIO 9 | TFT data/command |
| CS | GPIO 10 | TFT chip select |
| CS-TF | GPIO 14 | SD card chip select |
| OUT | GPIO 13 | SD MISO (a TFT nem használja) |

Az SPI órajel 10 MHz (mind a TFT, mind az SD oldalán) — a közös buszon óvatos
érték, magasabb órajelen jelintegritási problémák lehetnek.

## PCM5102A DAC (I2S)

| PCM5102 láb | ESP32-S3 GPIO | Megjegyzés |
|---|---|---|
| VIN | 3V3 vagy 5V | A modul saját LDO-val, mindkettő OK |
| GND | GND | |
| BCK | GPIO 5 | I2S bit clock |
| LCK | GPIO 6 | I2S word select (LRCK) |
| DIN | GPIO 7 | I2S data |
| SCK | **GND** | Master clock-ot a modul belső PLL-ből csinálja |

Modul jumperei (alapból általában jól vannak):
- XSMT → HIGH (3V3) — különben néma
- FLT, DEMP, FMT → LOW (GND)

## Akku-mérés (18650-hez)

| Komponens | ESP32-S3 GPIO | Megjegyzés |
|---|---|---|
| Akku ADC | GPIO 4 (ADC1 CH3) | 100k:100k osztón keresztül a 18650 + lábáról |

Jelenleg lebeg — random érték a UI-n. Funkcionálisan nem zavaró.

## Foglalt lábak — ne használd

- **GPIO 0, 3, 45, 46** — strapping pinek, óvatosan
- **GPIO 19, 20** — USB D−/D+ (natív USB port)
- **GPIO 26–37** — belső flash (26–32) + OPI PSRAM (33–37), tilos
- **GPIO 43, 44** — UART0 RX/TX (a COMM/UART port soros logja, ne használd
  gombnak)

## Tápellátás

- USB tápláláskor a dev kit USB → 5V rail → 3V3 LDO
- A kijelző-modulra a dev kit `5V` lábáról jön, NEM a `3V3`-ról (különben SD
  bedugva a panel rail-ja összeesik)
- A PCM5102 a 3V3 sínről kapja a tápot — terhelése kicsi

Akkus tápra később:
- 18650 (3.0–4.2 V) → védett TP4056 töltő → MT3608 boost 5V-ra → dev kit `5V`
  láb
