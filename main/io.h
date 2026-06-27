#pragma once

#include <stdint.h>
#include <stdbool.h>

// 8 gomb, SNES-layout (D-pad + A/B/X/Y). A player-oldali jelentésük:
// A = play/pause, Right/Left = next/prev (Library: belép/fel), Up/Down =
// hangerő (Library: kurzor); B/X/Y a playerben (még) no-op. A GB emulátorban
// X = Start, Y = Select (lásd gb.c).
typedef enum {
    BTN_EVT_A,          // play/pause
    BTN_EVT_B,
    BTN_EVT_UP,         // hangerő +
    BTN_EVT_DOWN,       // hangerő −
    BTN_EVT_LEFT,       // prev
    BTN_EVT_RIGHT,      // next
    BTN_EVT_X,          // GB: Start
    BTN_EVT_Y,          // GB: Select
} btn_event_t;

// Callback típus: a player.c regisztrál rá.
typedef void (*btn_cb_t)(btn_event_t evt);

// Akku frissítés callback: mV + százalék.
typedef void (*bat_cb_t)(uint16_t mv, uint8_t percent);

// Lakat callback: értesítés állapotváltozáskor. A slide switch megszűnt; a
// lock-állapotot később egy gomb hosszú nyomása fogja billenteni (TODO).
typedef void (*lock_cb_t)(bool locked);

void io_init(void);

void io_register_button_cb(btn_cb_t cb);
void io_register_battery_cb(bat_cb_t cb);
void io_register_lock_cb(lock_cb_t cb);

// Egyszerű azonnali olvasás (UI indulásra)
uint16_t io_read_battery_mv(void);
uint8_t  io_battery_percent_from_mv(uint16_t mv);

// Lakat aktuális állapota. A slide switch megszűnt → egyelőre mindig false,
// amíg a gomb-hosszúnyomásos lock be nem kerül (TODO).
bool io_is_locked(void);

// Pillanatnyi nyers gombállapot — true, ha a gomb épp lenyomva. A game mode
// pollozza frame-enként: az eseményalapú callback csak press-t ad, a CHIP-8-
// nak viszont tartás-állapot kell (EX9E/EXA1).
bool io_button_down(btn_event_t evt);
