#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

#include "cli.h"
#include "audio.h"
#include "player.h"
#include "usb_msc.h"
#include "io.h"
#include "ui.h"
#include "game.h"
#include "gb.h"

static const char *TAG = "cli";

#define CLI_UART        UART_NUM_0
#define CLI_CMD_MAX     32
#define CLI_PARAM_MAX   32

static void set_volume(uint8_t v)
{
    if (v > 100) v = 100;
    audio_set_volume(v);
    ui_set_volume(v);
}

static void set_backlight(int v)
{
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    player_set_backlight((uint8_t)v);   // alkalmaz + NVS-be ment
}

// Egy parancs lekezelése: `cmd` a `#` és `$` (vagy `#`) közti rész,
// `param` a `$` és záró `#` közti rész (üres lehet).
static void cli_dispatch(const char *cmd, const char *param)
{
    // Display-off "wake-only first event" — ugyanaz mint a gombnyomásnál:
    // ha sötét volt a kijelző, ez a parancs CSAK ébreszt, a funkcióját nem
    // hajtjuk végre. A user-nek még egy parancsot kell küldenie.
    if (ui_user_activity()) {
        ESP_LOGI(TAG, "wake from CLI '%s' — command not executed", cmd);
        return;
    }

    // Game mode (CHIP-8 / GB): a gombokra kötött parancsok a játék kulcsait
    // ütik (rövid szimulált tap) — a player ilyenkor nem kap eseményt.
    if (game_is_active() || gbmode_is_active()) {
        btn_event_t gevt;
        bool mapped = true;
        if      (strcmp(cmd, "play") == 0)   gevt = BTN_EVT_A;
        else if (strcmp(cmd, "a") == 0)      gevt = BTN_EVT_A;
        else if (strcmp(cmd, "b") == 0)      gevt = BTN_EVT_B;
        // X = GB Start, Y = GB Select — a start/select aliasok megmaradnak.
        else if (strcmp(cmd, "x") == 0)      gevt = BTN_EVT_X;
        else if (strcmp(cmd, "y") == 0)      gevt = BTN_EVT_Y;
        else if (strcmp(cmd, "start") == 0)  gevt = BTN_EVT_X;
        else if (strcmp(cmd, "select") == 0) gevt = BTN_EVT_Y;
        else if (strcmp(cmd, "next") == 0)   gevt = BTN_EVT_RIGHT;
        else if (strcmp(cmd, "prev") == 0)   gevt = BTN_EVT_LEFT;
        else if (strcmp(cmd, "vol") == 0 && strcmp(param, "up") == 0)   gevt = BTN_EVT_UP;
        else if (strcmp(cmd, "vol") == 0 && strcmp(param, "down") == 0) gevt = BTN_EVT_DOWN;
        else mapped = false;
        if (mapped) {
            if (game_is_active()) game_handle_button(gevt);
            else                  gbmode_handle_button(gevt);
            return;
        }
        // a többi parancs (gips, bl, screen...) a normál úton fut tovább
    }

    audio_status_t st;
    audio_get_status(&st);

    if (strcmp(cmd, "play") == 0) {
        ESP_LOGI(TAG, "play");
        if (st.state != AUDIO_STATE_PLAYING) {
            player_handle_button(BTN_EVT_A);
        }
    } else if (strcmp(cmd, "pause") == 0) {
        ESP_LOGI(TAG, "pause");
        if (st.state == AUDIO_STATE_PLAYING) {
            player_handle_button(BTN_EVT_A);
        }
    } else if (strcmp(cmd, "stop") == 0) {
        ESP_LOGI(TAG, "stop");
        audio_stop();
        ui_set_state(AUDIO_STATE_STOPPED);
        ui_set_progress(0, 0);
    } else if (strcmp(cmd, "next") == 0) {
        ESP_LOGI(TAG, "next");
        player_handle_button(BTN_EVT_RIGHT);
    } else if (strcmp(cmd, "prev") == 0) {
        ESP_LOGI(TAG, "prev");
        player_handle_button(BTN_EVT_LEFT);
    } else if (strcmp(cmd, "screen") == 0) {
        if (strcmp(param, "next") == 0) {
            ESP_LOGI(TAG, "screen next");
            ui_next_screen();
        } else if (strcmp(param, "prev") == 0) {
            ESP_LOGI(TAG, "screen prev");
            ui_prev_screen();
        } else {
            ESP_LOGW(TAG, "screen: unknown param '%s' (use next/prev)", param);
        }
    } else if (strcmp(cmd, "vol") == 0) {
        if (strcmp(param, "up") == 0) {
            ESP_LOGI(TAG, "vol up");
            player_handle_button(BTN_EVT_UP);
        } else if (strcmp(param, "down") == 0) {
            ESP_LOGI(TAG, "vol down");
            player_handle_button(BTN_EVT_DOWN);
        } else if (strcmp(param, "max") == 0) {
            ESP_LOGI(TAG, "vol max");
            set_volume(100);
        } else if (strcmp(param, "off") == 0) {
            ESP_LOGI(TAG, "vol off");
            set_volume(0);
        } else if (param[0] >= '0' && param[0] <= '9') {
            int v = atoi(param);
            ESP_LOGI(TAG, "vol %d%%", v);
            set_volume((uint8_t)v);
        } else {
            ESP_LOGW(TAG, "vol: unknown param '%s' (use up/down/max/off/0-100)", param);
        }
    } else if (strcmp(cmd, "bl") == 0) {
        int cur = ui_get_backlight();
        if (strcmp(param, "up") == 0) {
            set_backlight(cur + 10);
            ESP_LOGI(TAG, "bl up -> %d%%", ui_get_backlight());
        } else if (strcmp(param, "down") == 0) {
            set_backlight(cur - 10);
            ESP_LOGI(TAG, "bl down -> %d%%", ui_get_backlight());
        } else if (strcmp(param, "max") == 0) {
            ESP_LOGI(TAG, "bl max");
            set_backlight(100);
        } else if (strcmp(param, "off") == 0) {
            ESP_LOGI(TAG, "bl off");
            set_backlight(0);
        } else if (param[0] >= '0' && param[0] <= '9') {
            int v = atoi(param);
            ESP_LOGI(TAG, "bl %d%%", v);
            set_backlight(v);
        } else {
            ESP_LOGW(TAG, "bl: unknown param '%s' (use up/down/max/off/0-100)", param);
        }
    } else if (strcmp(cmd, "gips") == 0) {
        // CHIP-8 utasítás-büdzsé / frame (élő hangoláshoz, lásd game.c).
        if (param[0] >= '0' && param[0] <= '9') {
            game_set_ips(atoi(param));
        } else {
            ESP_LOGW(TAG, "gips: szám kell (5-200), pl. ##gips$$20##");
        }
    } else if (strcmp(cmd, "sdtest") == 0) {
        // Nyers SD-olvasási sebesség mérése (diagnosztika az USB MSC mount
        // lassúságához). A közös SPI busz miatt az LVGL flush-okat kizárjuk.
        ESP_LOGI(TAG, "sdtest: SD olvasási sebesség mérése...");
        ui_spi_lock();
        sd_speed_test();
        ui_spi_unlock();
    } else if (strcmp(cmd, "usb") == 0) {
        // Újraindulás USB MSC módba (SD a natív USB-n külső meghajtóként).
        ESP_LOGI(TAG, "usb -> reboot to USB MSC mode");
        usb_msc_request_reboot();   // sosem tér vissza
    } else {
        ESP_LOGW(TAG, "unknown command: '%s'", cmd);
    }
}

// Parser: ##cmd$$payload##  (vagy ##cmd## payload nélkül).
// A duplázott delimiterek miatt egy egyszerű állapotgép kell.
typedef enum {
    ST_IDLE,            // várjuk az első #-et
    ST_HASH1,           // egy # megvolt, a másikat várjuk
    ST_CMD,             // cmd gyűjtése
    ST_CMD_HASH,        // cmd-ben egy # — második #-re zárul a frame (payload nélkül)
    ST_CMD_DOLLAR,      // cmd-ben egy $ — második $-re indul a payload
    ST_PAYLOAD,         // payload gyűjtése
    ST_PAYLOAD_HASH,    // payload-ban egy # — második #-re zárul a frame
} pstate_t;

static void cli_task(void *arg)
{
    uint8_t rx[64];
    char cmd[CLI_CMD_MAX];      int cmd_len = 0;
    char payload[CLI_PARAM_MAX]; int payload_len = 0;
    pstate_t state = ST_IDLE;

    ESP_LOGI(TAG, "CLI ready — formátum: ##cmd##  vagy  ##cmd$$payload##");

    while (1) {
        int n = uart_read_bytes(CLI_UART, rx, sizeof(rx), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            char c = (char)rx[i];
            switch (state) {
            case ST_IDLE:
                if (c == '#') state = ST_HASH1;
                break;
            case ST_HASH1:
                if (c == '#') { state = ST_CMD; cmd_len = 0; payload_len = 0; }
                else          { state = ST_IDLE; }   // egyetlen # → false alarm
                break;
            case ST_CMD:
                if      (c == '#') state = ST_CMD_HASH;
                else if (c == '$') state = ST_CMD_DOLLAR;
                else if (cmd_len < CLI_CMD_MAX - 1) cmd[cmd_len++] = c;
                else               state = ST_IDLE;   // overflow
                break;
            case ST_CMD_HASH:
                if (c == '#') {
                    cmd[cmd_len] = 0; payload[0] = 0;
                    cli_dispatch(cmd, payload);
                }
                state = ST_IDLE;
                break;
            case ST_CMD_DOLLAR:
                if (c == '$') state = ST_PAYLOAD;
                else          state = ST_IDLE;   // árva $ → false alarm, reset
                break;
            case ST_PAYLOAD:
                if      (c == '#') state = ST_PAYLOAD_HASH;
                else if (payload_len < CLI_PARAM_MAX - 1) payload[payload_len++] = c;
                else               state = ST_IDLE;
                break;
            case ST_PAYLOAD_HASH:
                if (c == '#') {
                    cmd[cmd_len] = 0; payload[payload_len] = 0;
                    cli_dispatch(cmd, payload);
                }
                state = ST_IDLE;
                break;
            }
        }
    }
}

void cli_init(void)
{
    uart_driver_install(CLI_UART, 512, 0, 0, NULL, 0);
    // 8192: a parancsok player_handle_button-t hívnak, ami FATFS könyvtár-
    // szkennt + LVGL rebuildet csinálhat (browser_activate) — 4096 kevés.
    xTaskCreate(cli_task, "cli", 8192, NULL, 3, NULL);
}
