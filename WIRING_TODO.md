# Hardver bekötési TODO

Szoftverben már implementált funkciók, amik még fizikai bekötésre várnak.

Kész (bekötve, a [PINOUT.md](PINOUT.md) dokumentálja):
- **Háttérvilágítás — GPIO 16**: a panel LED lába GPIO 16-on, LEDC PWM
  (20 kHz, 0–100%, Settings / CLI `bl`), idle alatt duty 0.

## PCM5102 XSMT — GPIO 21 (kész, ha működik a track-váltási mute)

Ha a klikk-mentes track-váltáshoz a XSMT-t GPIO 21-re kötötted és a
modul 3V3 → XSMT hidat eltávolítottad, ez kész. Ha még nincs:

1. A PCM5102 modulon a XSMT láb mellett egy 0Ω SMD ellenállás vagy
   forrasztott jumper köti 3V3-ra. Ezt el kell távolítani.
2. XSMT láb → ESP32-S3 **GPIO 21**.
