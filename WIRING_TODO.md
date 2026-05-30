# Hardver bekötési TODO

Szoftverben már implementált funkciók, amik még fizikai bekötésre várnak.

## Háttérvilágítás vezérlés — GPIO 16

A `ui_idle_check` / `ui_user_activity` már LOW-ra húzza a GPIO 16-ot 30 s
tétlenség után, és HIGH-ra ébresztéskor. Hardveresen még nincs bekötve.

**Tennivaló a ST7789 modulon:**

1. Keresd meg a panel BLK (vagy `BL` / `LED+`) lábát a modul hátoldalán.
2. Vágd el a 3V3 → BLK hidat — ez tipikusan egy SMD 0Ω ellenállás vagy
   forrasztott jumper a modul PCB-jén, közvetlenül a BLK pad mellett.
   (Multiméterrel folytonosság-méréssel ellenőrizhető: BLK ↔ 3V3 most
   short, vágás után nem szabad.)
3. Kösd a BLK lábat ESP32-S3 **GPIO 16**-ra.

Ha a modul a BLK lábat nem hozza ki külön headerre, a panel megfelelő
lábát kell kis vezetékkel kihozni.

**Áramfelvétel**: ST7789V backlight LED-string ~20–40 mA. Egy ESP32-S3
GPIO 40 mA-t bír sourcing-ban — direkt meghajtás határeset de elmegy.
Ha mérve magasabbnak találod, iktass be egy n-MOSFET-et (pl. AO3400)
közte: BLK → drain, source → GND, GPIO 16 → gate.

## PCM5102 XSMT — GPIO 21 (kész, ha működik a track-váltási mute)

Ha a klikk-mentes track-váltáshoz a XSMT-t GPIO 21-re kötötted és a
modul 3V3 → XSMT hidat eltávolítottad, ez kész. Ha még nincs:

1. A PCM5102 modulon a XSMT láb mellett egy 0Ω SMD ellenállás vagy
   forrasztott jumper köti 3V3-ra. Ezt el kell távolítani.
2. XSMT láb → ESP32-S3 **GPIO 21**.
