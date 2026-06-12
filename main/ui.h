#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "sd.h"

// LVGL + ST7796 init. A háttérvilágítás boot alatt OFF marad.
void ui_init(void);

// Boot splash lejátszása: a firmware-be ágyazott JPEG frame-szett ("videó").
// Az ui_init UTÁN, de a player_start (SD-szkennelés) ELŐTT hívandó — a
// frame-ek flash-ből jönnek, így nincs SD-olvasás a kirajzolással egy időben.
// Az utolsó frame a képernyőn marad, amíg az ui_display_ready le nem cseréli a
// kész UI-ra. A háttérvilágítást az első frame után maga kapcsolja fel.
void ui_play_boot_splash(void);

// USB MSC mód: statikus "USB Storage Mode" képernyő Exit gombbal
// (have_card=false esetén "No SD card" üzenet). Az ui_init után hívandó.
void ui_show_usb_mode_screen(bool have_card);

// MSC alatt (fagyasztott LVGL mellett) hívható: közvetlenül a touch driverből
// (külön I2C busz) olvassa az érintést, és igazat ad, ha az az Exit gomb
// területére esik. Az usb_msc_run pollozza, találatkor esp_restart().
bool ui_usb_exit_touched(void);

// LVGL befagyasztása az MSC mód idejére: a port-lockot véglegesen megtartja,
// így a taskLVGL nem flushel a közös SPI buszra, amíg az MSC az SD-t használja.
// Hívd az "USB mód" képernyő kirajzolása + backlight után, az MSC indítása előtt.
void ui_suspend_for_msc(void);

// A teljes init után (player_start lefutott, tartalom betöltve) hívd: a UI-t
// azonnal kirajzoltatja, megvárja a flush-t, majd felkapcsolja a
// háttérvilágítást — így a boot-kori fehér flash + üres fázis nem látszik.
void ui_display_ready(void);

// LVGL-safe wrapper: a hívás belép az LVGL mutexbe, beállítja a UI-t,
// majd elenged.
void ui_show_track(const track_t *tr);   // cím + álbumkép töltés
void ui_show_no_track(void);

// A Now Playing állapot-ikont állítja: PLAYING → ▶, PAUSED → ⏸, STOPPED/FINISHED → ■
#include "audio.h"
void ui_set_state(audio_state_t st);
void ui_set_progress(uint32_t pos_ms, uint32_t dur_ms);
void ui_set_volume(uint8_t vol);

// Háttérvilágítás fényereje százalékban (0–100), LEDC PWM-en. 0 = sötét,
// 100 = teljes. A beállított érték idle/sleep alatt megmarad, ébredéskor
// visszaáll. Boot: 100%.
void    ui_set_backlight(uint8_t pct);
uint8_t ui_get_backlight(void);
void ui_set_battery(uint16_t mv, uint8_t percent);

// Egyszerű playlist navigáció (cursor highlight)
void ui_set_playlist(const track_t *tracks, int count, int current_idx);

// Megosztott SPI busz serializációja: a fread-eknek (SD-n) vissza kell
// tartaniuk az LVGL flush-okat, különben a HAL assertel.
// Belül lvgl_port_lock/unlock-ot hív.
void ui_spi_lock(void);
void ui_spi_unlock(void);

// -----------------------------------------------------------------------------
// Multi-screen API — Now Playing / Library / Settings
// -----------------------------------------------------------------------------
//
// A három képernyő közti váltás touch swipe-pal történik:
//   - swipe left  → ui_next_screen()  (Now → Library → Settings → Now)
//   - swipe right → ui_prev_screen()
// A MENU gomb short press már nem cikláltatja a képernyőket (csak a long
// press maradt = SD rescan a player.c-ben).
//
// A Library képernyőn a NEXT/PREV gombok a teljes lista cursorát mozgatják,
// és a PLAY gomb a kiválasztott elemet indítja el — a player.c ezt a két
// helper-rel intézi: ui_browser_set_cursor() és (külön) a kiválasztás.
//
// Minden ui_* hívás belép az LVGL mutexbe, ahogy eddig is.

typedef enum {
    UI_SCREEN_NOW_PLAYING = 0,
    UI_SCREEN_LIBRARY,
    UI_SCREEN_SETTINGS,
    UI_SCREEN_COUNT
} ui_screen_t;

void        ui_show_screen(ui_screen_t s);
void        ui_next_screen(void);
void        ui_prev_screen(void);
ui_screen_t ui_current_screen(void);

// Library = fájlböngésző. A player.c birtokolja a navigációs állapotot
// (aktuális könyvtár + kurzor), és ezeken keresztül rajzoltat:
//   ui_browser_show()       — teljes lista újraépítés (path + bejegyzések)
//   ui_browser_set_cursor() — csak a kijelölés mozgatása + görgetés
void ui_browser_show(const char *path, const dir_entry_t *entries,
                     int count, int cursor);
void ui_browser_set_cursor(int cursor);

// -----------------------------------------------------------------------------
// Idle / energy management
// -----------------------------------------------------------------------------
// 30 másodperc inaktivitás után a panel kikapcsol (DISPOFF). Bármilyen
// felhasználói trigger (gomb / CLI) visszakapcsolja és nullázza az idő-számlálót.
//
// ui_user_activity():  hívd minden user-eseménynél (gomb / cli dispatch).
//                      Visszakapcsolja a panelt ha aludt, és nullázza az idő-t.
//                      Visszatérési érték: true, ha ezzel a hívással felébredt
//                      a kijelző (eddig DISPOFF volt). Gomboknál ekkor érdemes
//                      "csak ébresztés, esemény nem fut" viselkedést alkalmazni.
// ui_idle_check():     periodikusan hívd (pl. player_task-ból, 200 ms-enként).
//                      30 s tétlenség után DISPOFF-ot küld a panelra.
bool ui_user_activity(void);
void ui_idle_check(void);

// Wake-pending állapot kikényszerítése: a következő ui_user_activity() hívás
// úgy fog viselkedni, mintha aludt volna a kijelző (visszaadja true, és így
// a player.c eldobja az adott gombnyomás funkcióját). main.c hívja deep
// sleep wake után, hogy a wake-triggert ne lehessen összekeverni egy
// szándékos műveletbe a felhasználó számára.
void ui_force_wake_pending(void);

// Idle timeout konfiguráció. Értékek: 10, 15, 30 mp, vagy 0 = never.
// A player.c menti NVS-be (idle_s key) és visszaolvassa bootkor.
void ui_set_idle_timeout_s(int seconds);
int  ui_get_idle_timeout_s(void);
// dir > 0: 10→15→30→Never→10 ; dir <= 0: visszafelé. Visszaadja az új értéket.
int  ui_cycle_idle_timeout(int dir);

// -----------------------------------------------------------------------------
// Settings képernyő — touch-vezérelt: volume/brightness sliderek, Display off /
// Sleep tap-cycle sorok. Nincs edit-mód; a kurzor csak vizuális (a legutóbb
// érintett soron 3 px accent bal-csík). Az értékeket a player.c menti NVS-be.
// -----------------------------------------------------------------------------
typedef enum {
    UI_SETTING_VOLUME = 0,
    UI_SETTING_BACKLIGHT,
    UI_SETTING_IDLE_TIMEOUT,
    UI_SETTING_SLEEP,
    UI_SETTING_ALBUM_END,
    UI_SETTING_COUNT
} ui_setting_t;

// Sleep on/off — ha enabled és minden feltétel teljesül, a player_task
// deep sleep-be megy. A player.c menti NVS-be (sleep_en key).
void ui_set_sleep_enabled(bool enabled);
bool ui_get_sleep_enabled(void);
bool ui_toggle_sleep_enabled(void);   // visszaadja az új értéket

// Album (lista) végi viselkedés — a player_task ez alapján dönt, amikor az
// utolsó track is lejátszódott. A player.c menti NVS-be (alb_end key).
typedef enum {
    UI_ALBUM_END_STOP = 0,    // megállás (eddigi viselkedés)
    UI_ALBUM_END_REPEAT,      // a lista újra elölről
    UI_ALBUM_END_NEXT,        // következő testvérmappa abc-ben (körbefordul);
                              // m3u listánál nem értelmezett → stop
    UI_ALBUM_END_COUNT
} ui_album_end_t;
void ui_set_album_end_mode(int mode);
int  ui_get_album_end_mode(void);
int  ui_cycle_album_end_mode(void);   // Stop→Repeat→Next→Stop, visszaadja az újat

// Lakat ikon a header-ben: állapot átkapcsolása. io_init hívja induláskor
// + a switch változásakor.
void ui_set_locked(bool locked);
