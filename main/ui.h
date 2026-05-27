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

void ui_set_playing(bool playing);
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

// Library cursor — csak akkor van értelme, ha UI_SCREEN_LIBRARY az aktív.
// delta = +1 → lefelé, -1 → felfelé. Túlcsordulás esetén wrap-around.
void ui_library_move_cursor(int delta);
int  ui_library_get_selected_index(void);

// Tájékoztató panel a Settings képernyőn — a player.c rescan után frissíti.
void ui_set_track_count(int count);
