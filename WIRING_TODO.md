# Hardver bekötési TODO

Szoftverben már implementált funkciók, amik még fizikai bekötésre várnak.

## B / Start / Select gombok (Game Boy módhoz)

Három új nyomógomb, a meglévőkkel azonos bekötéssel (egyik láb a GPIO-ra,
átellenes láb GND-re; belső pull-up, lenyomva LOW):

| Gomb | ESP32-S3 GPIO | Megjegyzés |
|---|---|---|
| **B** | **GPIO 48** | onboard RGB LED DIN — bemenetnek szabad |
| **Start** | **GPIO 3** | JTAG-sel strap — default eFuse mellett közömbös |
| **Select** | **GPIO 0** | BOOT strap — reset közben nyomva = download mód |

Amíg nincsenek bekötve, a CLI-ből ütheted őket játék közben:
`##b##`, `##start##`, `##select##` (és `##a##` az A-hoz).

Kész (bekötve, a [PINOUT.md](PINOUT.md) dokumentálja):
- **Háttérvilágítás — GPIO 16**: a panel LED lába GPIO 16-on, LEDC PWM
  (20 kHz, 0–100%, Settings / CLI `bl`), idle alatt duty 0.

## PCM5102 XSMT — GPIO 21 (kész, ha működik a track-váltási mute)

Ha a klikk-mentes track-váltáshoz a XSMT-t GPIO 21-re kötötted és a
modul 3V3 → XSMT hidat eltávolítottad, ez kész. Ha még nincs:

1. A PCM5102 modulon a XSMT láb mellett egy 0Ω SMD ellenállás vagy
   forrasztott jumper köti 3V3-ra. Ezt el kell távolítani.
2. XSMT láb → ESP32-S3 **GPIO 21**.
