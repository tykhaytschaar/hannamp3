#pragma once

#include <stdbool.h>

// Game picker (Settings → Games): a GAMES_DIR Game Boy (.gb) ROM-jainak
// választólistája. Tap egy soron → player_launch_game() → gb.c (emuláció).
//
// Belépési pontok:
//   - Settings "Games" sor → game_show_picker()
//   - Library .gb sora → player_launch_game() (a picker megkerülésével)

// Game picker screen megnyitása: a GAMES_DIR .gb fájljai listában, tap =
// indítás, Back = vissza a Settingsre. No-op, ha már nyitva van / GB fut.
void game_show_picker(void);
