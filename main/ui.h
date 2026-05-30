#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "sd.h"

// LVGL + ST7789 init.
void ui_init(void);

// LVGL-safe wrapper: a hívás belép az LVGL mutexbe, beállítja a UI-t,
// majd elenged.
void ui_show_track(const track_t *tr);   // cím + álbumkép töltés
void ui_show_no_track(void);

// A Now Playing állapot-ikont állítja: PLAYING → ▶, PAUSED → ⏸, STOPPED/FINISHED → ■
#include "audio.h"
void ui_set_state(audio_state_t st);
void ui_set_progress(uint32_t pos_ms, uint32_t dur_ms);
void ui_set_volume(uint8_t vol);
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
// A MENU gomb (rövid nyomás) a player.c-ben ui_next_screen()-t hív, ami
// ciklikusan vált a három képernyő között. A Library képernyőn a
// NEXT/PREV gombok a teljes lista cursorát mozgatják, és a PLAY gomb a
// kiválasztott elemet indítja el — a player.c ezt a két helper-rel intézi:
// ui_library_move_cursor() és ui_library_get_selected_index().
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

// Idle timeout konfiguráció. Értékek: 10, 15, 30 mp, vagy 0 = never.
// A player.c menti NVS-be (idle_s key) és visszaolvassa bootkor.
void ui_set_idle_timeout_s(int seconds);
int  ui_get_idle_timeout_s(void);
// dir > 0: 10→15→30→Never→10 ; dir <= 0: visszafelé. Visszaadja az új értéket.
int  ui_cycle_idle_timeout(int dir);

// -----------------------------------------------------------------------------
// Settings képernyő — kurzor + edit mód az állítható elemek között.
// A NOW Playing aktuális dal highlightjához hasonlóan:
//   - kurzor (nem-edit): 3 px accent-színű bal-oldali csík
//   - edit mód: a teljes sor highlighted (sötétebb háttér + bal csík)
//
// Gombok a player.c-ből (Settings képernyőn):
//   Vol +/−   : edit-en kívül kurzor mozgatása; edit-en belül érték ±
//   Next      : edit mód bekapcsolása az aktuális elemen
//   Prev      : edit mód kikapcsolása + revert az eredeti értékre
//   Play      : edit mód kikapcsolása + perzisztens mentés (NVS)
// -----------------------------------------------------------------------------
typedef enum {
    UI_SETTING_VOLUME = 0,
    UI_SETTING_IDLE_TIMEOUT,
    UI_SETTING_SLEEP,
    UI_SETTING_COUNT
} ui_setting_t;

void          ui_settings_move_cursor(int delta);
bool          ui_settings_is_editing(void);
void          ui_settings_set_editing(bool on);
ui_setting_t  ui_settings_get_cursor(void);

// Sleep on/off — ha enabled és minden feltétel teljesül, a player_task
// deep sleep-be megy. A player.c menti NVS-be (sleep_en key).
void ui_set_sleep_enabled(bool enabled);
bool ui_get_sleep_enabled(void);
bool ui_toggle_sleep_enabled(void);   // visszaadja az új értéket

// Lakat ikon a header-ben: állapot átkapcsolása. io_init hívja induláskor
// + a switch változásakor.
void ui_set_locked(bool locked);
