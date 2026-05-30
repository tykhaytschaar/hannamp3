#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    BTN_EVT_PLAY_PAUSE,
    BTN_EVT_NEXT,
    BTN_EVT_PREV,
    BTN_EVT_MENU,
    BTN_EVT_MENU_LONG,  // MENU long press — rescan SD (player.c kezeli)
    BTN_EVT_VOL_UP,
    BTN_EVT_VOL_DOWN,
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
