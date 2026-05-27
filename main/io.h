#pragma once

#include <stdint.h>

typedef enum {
    BTN_EVT_PLAY_PAUSE,
    BTN_EVT_NEXT,
    BTN_EVT_PREV,
    BTN_EVT_MENU,
    BTN_EVT_VOL_UP,     // MENU long press → vol up cycle
    BTN_EVT_VOL_DOWN,   // PREV long press
} btn_event_t;

// Callback típus: a player.c regisztrál rá.
typedef void (*btn_cb_t)(btn_event_t evt);

// Akku frissítés callback: mV + százalék.
typedef void (*bat_cb_t)(uint16_t mv, uint8_t percent);

void io_init(void);

void io_register_button_cb(btn_cb_t cb);
void io_register_battery_cb(bat_cb_t cb);

// Egyszerű azonnali olvasás (UI indulásra)
uint16_t io_read_battery_mv(void);
uint8_t  io_battery_percent_from_mv(uint16_t mv);
