# Hardver bekötési TODO

Szoftverben már implementált funkciók, amik még fizikai bekötésre várnak.

## Háttérvilágítás vezérlés — GPIO 16

A `ui_idle_check` / `ui_user_activity` már LOW-ra húzza a GPIO 16-ot 30 s
tétlenség után, és HIGH-ra ébresztéskor. Hardveresen még nincs bekötve.

**Tennivaló a ST7796 modulon:**

A panelon saját tranzisztoros háttérvilágítás-driver (Q1–Q3) van, így a
header **LED** lába logikai szintű vezérlőbemenet — **nincs híd-vágás és
nem kell MOSFET**, a GPIO közvetlenül hajtja a driver tranzisztort.

1. A panel header **LED** lábát (lásd [PINOUT.md](PINOUT.md)) kösd
   ESP32-S3 **GPIO 16**-ra.

A LEDC PWM (20 kHz) a driveren keresztül szabályozza a fényerőt
(0–100%, Settings / CLI `bl`), idle alatt duty 0.

## PCM5102 XSMT — GPIO 21 (kész, ha működik a track-váltási mute)

Ha a klikk-mentes track-váltáshoz a XSMT-t GPIO 21-re kötötted és a
modul 3V3 → XSMT hidat eltávolítottad, ez kész. Ha még nincs:

1. A PCM5102 modulon a XSMT láb mellett egy 0Ω SMD ellenállás vagy
   forrasztott jumper köti 3V3-ra. Ezt el kell távolítani.
2. XSMT láb → ESP32-S3 **GPIO 21**.
