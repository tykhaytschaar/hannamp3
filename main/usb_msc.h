#pragma once

#include <stdbool.h>

// -----------------------------------------------------------------------------
// USB MSC mód — az SD kártya kitevése a natív USB-n (GPIO19/20) külső
// írható-olvasható meghajtóként, hogy PC-ről lehessen MP3-akat másolni a
// kártya kivétele nélkül.
//
// A mód külön boot-ág: a Settings egy gombja beállít egy egyszer-használatos
// flaget és újraindít; boot-kor a main.c ezt ellenőrzi és — ha be van állítva —
// a normál init helyett usb_msc_run()-t hív. A kilépés fizikai power-cycle /
// reset (a flag belépéskor törlődik, így a következő boot már normál módú).
//
// Megkötés: MSC alatt az SD-t a host vezérli (TinyUSB taskból), ezért az LVGL
// nem flushelhet a közös SPI buszra — az "USB mód" képernyő egyszer kirajzolódik,
// majd az LVGL befagyasztva marad (lásd ui_suspend_for_msc).
// -----------------------------------------------------------------------------

// Settings gomb: egyszer-használatos flag beállítása + esp_restart().
// Sosem tér vissza.
void usb_msc_request_reboot(void);

// Boot-kor (a deep-sleep gate után) hívandó. Igaz, ha USB MSC módba kell
// indulni. A flaget törli (egyszer-használatos).
bool usb_msc_boot_requested(void);

// USB MSC mód futtatása — sosem tér vissza.
void usb_msc_run(void);
