# Pinkiosztás — hannamp3

ESP32-S3-DevKitC-1 N16R8 lábkiosztása a projekthez. A táblázatok a fizikai
bekötést tükrözik. A kódbéli definíciók a [`main/app_config.h`](main/app_config.h)
fájlban vannak.

## Gombok (6 db)

Minden gomb egyik lába a megfelelő GPIO-ra, a **diagonálisan átellenes** lába
GND-re. Belső pull-up engedélyezve, lenyomáskor LOW.

| Gomb | ESP32-S3 GPIO | Funkció |
|---|---|---|
| **Menu** | **GPIO 1** | Screen váltás (rövid) / SD újraolvasás (hosszú). RTC-capable → deep sleep wake forrás (500 ms hold-press szükséges) |
| **Next** | **GPIO 2** | Következő szám |
| **Prev** | **GPIO 42** | Előző szám |
| **Play / Pause** | **GPIO 41** | Lejátszás indítása vagy szüneteltetése |
| **Vol +** | **GPIO 38** | Hangerő +5% / Settings kurzor fel / edit-en belül érték + |
| **Vol −** | **GPIO 39** | Hangerő −5% / Settings kurzor le / edit-en belül érték − |
| **Lock** (tolókapcsoló) | **GPIO 17** | Slide switch. GND-re zár (LOW) = locked, minden gomb-event eldobódik |

A `Vol +` és `Vol −` támogat **hold-to-repeat**-et: nyomva tartva ~120 ms-onként
újra-kiadja az eventet.

## ST7796U 3.5" (480×320) kijelző + FT6336 touch + microSD slot

Megosztott SPI2 buszt használ a TFT és az SD. A panelon 74LVC245A szintillesztő
(U2) és tranzisztoros háttérvilágítás-driver (Q1–Q3) van. A touch (CTP) egy
**FT6336** kapacitív kontroller külön I2C buszon (lásd lentebb).

A panel bal oldali 14 lábú headerje, fentről le:

| Panel láb | ESP32-S3 GPIO | Megjegyzés |
|---|---|---|
| SD_CS | GPIO 14 | SD card chip select |
| CTP_INT | — | **Nem kötjük be** (polling) — lásd a Touch szekciót |
| CTP_SDA | GPIO 15 | Touch I2C SDA |
| CTP_RST | GPIO 47 | Touch reset |
| CTP_SCL | GPIO 18 | Touch I2C SCL |
| SDO (MISO) | GPIO 13 | SD MISO (a TFT nem használja) |
| **LED** | **GPIO 16** | Háttérvilágítás, **LEDC PWM** (20 kHz). Tranzisztoros driver → logikai szint, direkt GPIO-ról hajtható, **nincs bridge-vágás**. Fényerő 0–100% (Settings / CLI `bl`, NVS-perzisztens), idle alatt duty 0 |
| SCK | GPIO 12 | SPI clock |
| SDI (MOSI) | GPIO 11 | SPI MOSI |
| LCD_RS | GPIO 9 | TFT data/command (DC) |
| LCD_RST | GPIO 8 | TFT reset |
| LCD_CS | GPIO 10 | TFT chip select |
| GND | GND | |
| VCC | **3V3** | A panel és az SD a 3V3 sínről megy (74LVC245A szintillesztőn át). SD-vel együtt is stabil |

A TFT SPI órajel **80 MHz** (`LCD_SPI_HZ`) — a SCK/MOSI (12/11) a SPI2 IOMUX
lábai, ezért nincs a 40 MHz-es GPIO-mátrix plafon. Az SD a saját órajelén megy
ugyanazon a buszon. Ha a közös buszon csíkozódás/„hó"/hibás sor van, vidd vissza
40 MHz-re.

Az **SD-órajel 10 MHz** (`sd.c`, `max_freq_khz`). A dupont-bekötés 20/40 MHz-en
CRC-error/mount-fail volt, de 10 MHz hibamentes és ~2× gyorsabb mint a korábbi
5 MHz (≈950 vs ≈540 KB/s). A sebesség az USB MSC módhoz kell (a macOS FSKit a
mountkor ~20s alatt beolvassa a FAT-táblát). A `##sdtest##` CLI-paranccsal
mérhető az olvasási sebesség és a megbízhatóság.

### Touch (FT6336) — bekötés

Külön I2C busz, független a kijelző SPI-tól. Cím 0x38. Driver:
`espressif/esp_lcd_touch_ft5x06` + `lvgl_port_add_touch(...)`, **polling
módban** (nincs INT láb — az FT6336 fix címen ül, az LVGL amúgy is pollozza).

| Panel láb | ESP32-S3 GPIO | Megjegyzés |
|---|---|---|
| CTP_SDA | GPIO 15 | I2C SDA |
| CTP_SCL | GPIO 18 | I2C SCL |
| CTP_RST | GPIO 47 | Touch reset |
| CTP_INT | — | **Nem kötjük be.** A GPIO 40 (korábbi terv) fixen LOW (külső HW követelmény), más szabad RTC-láb nincs; polling miatt felesleges. |

Megjegyzés: ha az I2C instabil (NACK/olvasási hiba), a modulon hiányozhatnak a
külső pull-upok — a kód a belső pull-upot bekapcsolja, de 4.7k külső
ajánlott; vagy vidd 100 kHz-re a `TOUCH_I2C_HZ`-t a [ui.c](main/ui.c)-ben.

## PCM5102A DAC (I2S)

| PCM5102 láb | ESP32-S3 GPIO | Megjegyzés |
|---|---|---|
| VIN | 3V3 vagy 5V | A modul saját LDO-val, mindkettő OK |
| GND | GND | |
| BCK | GPIO 5 | I2S bit clock |
| LCK | GPIO 6 | I2S word select (LRCK) |
| DIN | GPIO 7 | I2S data |
| SCK | **GND** | Master clock-ot a modul belső PLL-ből csinálja |
| **XSMT** | **GPIO 21** | Soft-mute. Track-váltáskor LOW (mute fade-out), különben HIGH (unmute). A klikkmentes track-átmenethez |

Modul jumperei:
- FLT, DEMP, FMT → LOW (GND)
- XSMT → most GPIO 21-ről hajtott. Ha a modulodon hardveresen 3V3-ra van
  kötve (pull-up vagy jumper), azt a hidat el kell távolítani, hogy a GPIO
  tudja LOW-ra húzni

## Akku-mérés (18650-hez)

| Komponens | ESP32-S3 GPIO | Megjegyzés |
|---|---|---|
| Akku ADC | GPIO 4 (ADC1 CH3) | 100k:100k osztón keresztül a 18650 + lábáról |

Jelenleg lebeg — random érték a UI-n. Funkcionálisan nem zavaró.

## Foglalt lábak — ne használd

- **GPIO 0, 3, 45, 46** — strapping pinek, óvatosan
- **GPIO 19, 20** — USB D−/D+ (natív USB port)
- **GPIO 26–37** — belső flash (26–32) + OPI PSRAM (33–37), tilos
- **GPIO 40** — fixen LOW (külső HW követelmény, `init_static_low_pins`)
- **GPIO 43, 44** — UART0 RX/TX (a COMM/UART port soros logja, ne használd
  gombnak)

Jelenleg használt szabad lábak: **15, 18** (touch I2C), **47** (touch RST).
Az **egyetlen szabadon maradt** láb a **GPIO 48** (onboard RGB LED — a firmware
nem használja).

## Tápellátás

- USB tápláláskor a dev kit USB → 5V rail → 3V3 LDO
- A kijelző-modul a dev kit `3V3` lábáról megy (a 74LVC245A szintillesztőn át),
  SD-vel együtt is stabil
- A PCM5102 a 3V3 sínről kapja a tápot — terhelése kicsi

Akkus tápra később:
- 18650 (3.0–4.2 V) → védett TP4056 töltő → MT3608 boost 5V-ra → dev kit `5V`
  láb
