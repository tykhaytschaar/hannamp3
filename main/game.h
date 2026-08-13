#pragma once

#include "lvgl.h"

// Játékok oldal (UI_SCREEN_GAMES): a GAMES_DIR Game Boy / Game Boy Color
// (.gb/.gbc) ROM-jainak választólistája. Tap egy soron → player_launch_game()
// → gb.c (emuláció). GB-kilépéskor a gb.c erre az oldalra tér vissza.
//
// Belépési pontok:
//   - header nyilak → UI_SCREEN_GAMES (ui.c screen-rotáció)
//   - Library .gb sora → player_launch_game() (a lista megkerülésével)

// Az oldal statikus váza a megadott screenre — ui_init hívja, LVGL lock alatt.
void game_screen_create(lv_obj_t *scr);

// ROM-lista újraépítés az SD-ről — ui_show_screen hívja az oldalra belépéskor,
// a portlock alatt (SD a közös SPI buszon). game_screen_create után hívható.
void game_screen_refresh(void);
