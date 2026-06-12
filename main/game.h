#pragma once

#include <stdbool.h>

#include "io.h"   // btn_event_t

// CHIP-8 game mode (chip8.c VM + LVGL megjelenítés) és a hozzá tartozó
// game picker (Settings → Games).
//
// Belépési pontok:
//   - Settings "Games" sor → game_show_picker() → tap egy ROM-ra →
//     player_launch_game() (player.c: zene-szüneteltetés) → game_start()
//   - Library .ch8 sor → player_launch_game() → game_start()
// Kilépés: a fejléc Exit gombja (touch), vagy Menu gomb fallbackként
// (player.c routolja game_request_exit()-ként).

// Kilépés-callback: a game mode lebontása UTÁN hívódik (LVGL task kontextus).
// A player.c ebben folytatja a zenét, ha a játék előtt szólt.
typedef void (*game_exit_cb_t)(void);

// ROM betöltése + game screen + játék-loop indítása. false, ha a ROM nem
// olvasható / nem érvényes (ilyenkor semmi nem változik a UI-ban).
bool game_start(const char *rom_path, game_exit_cb_t on_exit);

// true, amíg a game screen aktív. A player.c game mode alatt minden
// gombeseményt eldob (a játék nyersen pollozza a gombokat), és a
// deep sleep döntésnél aktivitásnak számítja.
bool game_is_active(void);

// Kilépés kérése nem-LVGL kontextusból (Menu gomb, io task) — a tényleges
// lebontást a játék-loop végzi a következő tickben.
void game_request_exit(void);

// Gomb-esemény → rövid "tap" a hozzá rendelt CHIP-8 kulcson. A CLI-ből
// érkező next/prev/vol/play parancsokhoz kell (player.c routolja ide game
// mode alatt): az eseményúton nincs tartás-állapot, ezért néhány tick-nyi
// lenyomást szimulálunk. Nem KEYMAP-elt esemény → no-op.
void game_handle_button(btn_event_t evt);

// Game picker screen: a GAMES_DIR .ch8 fájljai listában, tap = indítás,
// Back = vissza a Settingsre. No-op, ha már nyitva van / játék fut.
void game_show_picker(void);
