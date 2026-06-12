#pragma once

#include <stdint.h>
#include <stdbool.h>

// Játék-orientált gombnevek (GB-layout). A player-oldali jelentésük:
// A = play/pause, Right/Left = next/prev (Library: belép/fel), Up/Down =
// hangerő (Library: kurzor); B/Start/Select a playerben (még) no-op.
typedef enum {
    BTN_EVT_A,          // volt: PLAY_PAUSE
    BTN_EVT_B,
    BTN_EVT_UP,         // volt: VOL_UP
    BTN_EVT_DOWN,       // volt: VOL_DOWN
    BTN_EVT_LEFT,       // volt: PREV
    BTN_EVT_RIGHT,      // volt: NEXT
    BTN_EVT_START,
    BTN_EVT_SELECT,
    BTN_EVT_MENU,
    BTN_EVT_MENU_LONG,  // MENU long press — rescan SD (player.c kezeli)
} btn_event_t;

// Callback típus: a player.c regisztrál rá.
typedef void (*btn_cb_t)(btn_event_t evt);

// Akku frissítés callback: mV + százalék.
typedef void (*bat_cb_t)(uint16_t mv, uint8_t percent);

// Lakat-tolókapcsoló callback: értesítés állapotváltozáskor.
typedef void (*lock_cb_t)(bool locked);

void io_init(void);

void io_register_button_cb(btn_cb_t cb);
void io_register_battery_cb(bat_cb_t cb);
void io_register_lock_cb(lock_cb_t cb);

// Egyszerű azonnali olvasás (UI indulásra)
uint16_t io_read_battery_mv(void);
uint8_t  io_battery_percent_from_mv(uint16_t mv);

// LOCK tolókapcsoló aktuális állapota — true ha LOW (GND-re zárva).
// Frissítve egy 100 ms-os polling-task által, lock_cb-vel értesítéssel.
bool io_is_locked(void);

// Pillanatnyi nyers gombállapot — true, ha a gomb épp lenyomva. A game mode
// pollozza frame-enként: az eseményalapú callback csak press-t ad, a CHIP-8-
// nak viszont tartás-állapot kell (EX9E/EXA1).
bool io_button_down(btn_event_t evt);
