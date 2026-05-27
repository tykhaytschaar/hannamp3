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
