#pragma once

#include <stdbool.h>

#include "io.h"   // btn_event_t

// Game Boy (DMG) mód — Peanut-GB emulátormag (main/peanut_gb.h, MIT) +
// LVGL megjelenítés. A .gb ROM-okat a player_launch_game() irányítja ide
// (kiterjesztés szerint); a picker/Settings-integráció közös a CHIP-8-cal.
//
// Architektúra: az emuláció saját FreeRTOS taskban fut az 1-es magon
// (az LVGL a 0-son), valós idejű ~59,7 fps-sel, PSRAM-beli dupla
// framebufferbe (160×144 → 2× = 320×288 RGB565). Az LVGL-oldali
// present-timer a kész frame-eket lv_image-ként jeleníti meg — a
// megjelenítési ráta a flush sebességén múlik, az emuláció tempóján nem.

typedef void (*gbmode_exit_cb_t)(void);

// ROM betöltése (PSRAM-ba), GB screen + emulációs task indítása. false, ha
// a ROM nem olvasható/érvénytelen (ilyenkor a UI nem változik).
bool gbmode_start(const char *rom_path, gbmode_exit_cb_t on_exit);

// true, amíg a GB mód aktív (screen + task él).
bool gbmode_is_active(void);

// Kilépés kérése bármely kontextusból (Menu gomb / Exit touch) — az
// emulációs task a frame végén áll le, a lebontás az LVGL taskban fut.
void gbmode_request_exit(void);

// Gomb-esemény → rövid szimulált tap a GB joypadon (CLI-injektáláshoz,
// amíg a B/Start/Select nincs bekötve). Nem game-gomb esemény → no-op.
void gbmode_handle_button(btn_event_t evt);
