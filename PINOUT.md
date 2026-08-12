# Pinkiosztás — hannamp3

ESP32-S3-DevKitC-1 N16R8 lábkiosztása a projekthez. A táblázatok a fizikai
bekötést tükrözik. A kódbéli definíciók a [`main/app_config.h`](main/app_config.h)
fájlban vannak.

## Gombok (8 db) — SNES-layout

Minden gomb egyik lába a megfelelő GPIO-ra, a **diagonálisan átellenes** lába
GND-re. Belső pull-up engedélyezve, lenyomáskor LOW — **külső felhúzó nem
kell**. D-pad a panel bal oldalán, A/B/X/Y a jobb oldalán. Nincs Menu/Start/
Select gomb és nincs lakat-tolókapcsoló (a lock később egy gomb hosszú
nyomására kerül — lásd [WIRING_TODO.md](WIRING_TODO.md)).

| Gomb | ESP32-S3 GPIO | Header | Player-funkció | Játék-funkció |
|---|---|---|---|---|
| **Up** | **GPIO 17** | bal | Hangerő + / Library: kurzor fel | D-pad fel · deep sleep wake (500 ms hold) |
| **Down** | **GPIO 3** | bal | Hangerő − / Library: kurzor le | D-pad le |
| **Left** | **GPIO 2** | jobb (áthúzva) | Előző szám / Library: fel | D-pad balra |
| **Right** | **GPIO 1** | jobb (áthúzva) | Következő szám / Library: belép | D-pad jobbra |
| **A** | **GPIO 39** | jobb | Play / Pause | A (tűz/akció) |
| **B** | **GPIO 38** | jobb | — | B |
| **X** | **GPIO 42** | jobb | — | **GB: Start** |
| **Y** | **GPIO 41** | jobb | — | **GB: Select** |

A D-pad fizikailag a panel bal oldalára kerül, de a bal headeren csak a 17 és a
3 szabad (a többit a kijelző/SD/touch/audio foglalja), ezért a Left/Right
vezetéke a jobb headerre van **áthúzva** — pár cm, gombjelnél lényegtelen.

Az `Up` és `Down` támogat **hold-to-repeat**-et: nyomva tartva ~120 ms-onként
újra-kiadja az eventet (hangerő-rámpa).

Strap-megkötések:
- **Egyik gomb sem ül a problémás strap-lábakon** (0/45/46 szabadon) → bootkor
  nincs download-mód-kockázat.
- **GPIO 3** (Down): JTAG-sel strap, de égetetlen eFuse (default) mellett
  közömbös, és a régi firmware már bizonyítottan használta gombnak.
- A **GPIO 45/46 tilos** gombnak: a 45 a VDD_SPI feszültség-strap (pull-up
  mellett 1,8 V-ra húzná a flash-t), a 46 a boot-mód strapje.
- A korábbi B/Start/Select lábak (**48, 0**) felszabadultak, üresen maradnak.

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

## Akku-mérés (104050 LiPo cella)

| Komponens | ESP32-S3 GPIO | Megjegyzés |
|---|---|---|
| Akku ADC | GPIO 4 (ADC1 CH3) | 100k:100k osztón keresztül a cella + lábáról — bekötve |

Az osztó Thevenin-impedanciája 50k, ami sok az S3 ADC-jének (zajos,
ugráló mérés) → **100 nF – 1 µF kerámia kondenzátor kell** a GPIO 4 és GND
közé, az osztó középpontjánál.

## Foglalt lábak — ne használd

- **GPIO 0, 3, 45, 46** — strapping pinek, óvatosan
- **GPIO 19, 20** — USB D−/D+ (natív USB port)
- **GPIO 26–37** — belső flash (26–32) + OPI PSRAM (33–37), tilos
- **GPIO 40** — fixen LOW (külső HW követelmény, `init_static_low_pins`)
- **GPIO 43, 44** — UART0 RX/TX (a COMM/UART port soros logja, ne használd
  gombnak)

A gombok lábai: **17, 3** (bal header), **2, 1, 39, 38, 42, 41** (jobb header).
Felszabadult, üresen álló lábak: **48** (onboard RGB DIN) és **0** (BOOT strap)
— további bővítéshez ezek, illetve I2C-s GPIO-expander jöhet szóba.

## Tápellátás

Akkus üzem, hatékonyság-optimalizált lánccal:

- **3.7 V LiPo → TP4056 töltőmodul → buck-boost konverter (3.3 V) → a dev kit
  `3V3` lába.** A kijelző, az SD és a PCM5102 is 3.3 V-ról megy, így nincs
  felesleges lineáris veszteség (a buck-boost ~90%).
- **Az onboard 3V3 LDO le van forrasztva a panelról.** Ezért amikor USB-t
  dugsz be (flash/monitor), az csak **adatot + GND-t** visz — az USB-5V → LDO
  → 3V3 út megszűnt, így **nincs két 3.3 V forrás ütközése** a 3V3 sínen.
  A natív-USB Serial-JTAG a chip 3V3-járól megy (a buck-boost adja), a UART-
  híd a saját USB-VBUS-áról kap tápot → mindkét flash-út működik.
- A buck-boost és az akku **nem veszik fel a kapcsolatot az USB-vel**: az ESP
  USB-VBUS-a nem megy vissza a töltőre, az akkut nem tölti/terheli.
- **A 5V lábra ne köss semmit**, amíg az „IN-OUT" jumper zárt és USB is be
  lehet dugva — különben az USB-VBUS visszatáplálná (lásd a klón devkit
  IN-OUT áthidalóját).
- A PCM5102 terhelése kicsi; a háttérvilágítás (GPIO 16, LEDC PWM) a legnagyobb
  fogyasztó a 3V3 sínen.
