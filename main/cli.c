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
#include "io.h"
#include "ui.h"

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

// Egy parancs lekezelése: `cmd` a `#` és `$` (vagy `#`) közti rész,
// `param` a `$` és záró `#` közti rész (üres lehet).
static void cli_dispatch(const char *cmd, const char *param)
{
    audio_status_t st;
    audio_get_status(&st);

    if (strcmp(cmd, "play") == 0) {
        ESP_LOGI(TAG, "play");
        if (st.state != AUDIO_STATE_PLAYING) {
            player_handle_button(BTN_EVT_PLAY_PAUSE);
        }
    } else if (strcmp(cmd, "pause") == 0) {
        ESP_LOGI(TAG, "pause");
        if (st.state == AUDIO_STATE_PLAYING) {
            player_handle_button(BTN_EVT_PLAY_PAUSE);
        }
    } else if (strcmp(cmd, "stop") == 0) {
        ESP_LOGI(TAG, "stop");
        audio_stop();
        ui_set_state(AUDIO_STATE_STOPPED);
        ui_set_progress(0, 0);
    } else if (strcmp(cmd, "next") == 0) {
        ESP_LOGI(TAG, "next");
        player_handle_button(BTN_EVT_NEXT);
    } else if (strcmp(cmd, "prev") == 0) {
        ESP_LOGI(TAG, "prev");
        player_handle_button(BTN_EVT_PREV);
    } else if (strcmp(cmd, "menu") == 0) {
        ESP_LOGI(TAG, "menu");
        player_handle_button(BTN_EVT_MENU);
    } else if (strcmp(cmd, "vol") == 0) {
        if (strcmp(param, "up") == 0) {
            ESP_LOGI(TAG, "vol up");
            player_handle_button(BTN_EVT_VOL_UP);
        } else if (strcmp(param, "down") == 0) {
            ESP_LOGI(TAG, "vol down");
            player_handle_button(BTN_EVT_VOL_DOWN);
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
    } else {
        ESP_LOGW(TAG, "unknown command: '%s'", cmd);
    }
}

// Parser: #command$param#  — a $param opcionális (#command# is OK).
typedef enum { ST_IDLE, ST_CMD, ST_PARAM } pstate_t;

static void cli_task(void *arg)
{
    uint8_t rx[64];
    char cmd[CLI_CMD_MAX];     int cmd_len = 0;
    char param[CLI_PARAM_MAX]; int param_len = 0;
    pstate_t state = ST_IDLE;

    ESP_LOGI(TAG, "CLI ready — formátum: #parancs$param#  (pl. #play#, #vol$up#, #vol$50#)");

    while (1) {
        int n = uart_read_bytes(CLI_UART, rx, sizeof(rx), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            char c = (char)rx[i];
            switch (state) {
                case ST_IDLE:
                    if (c == '#') { state = ST_CMD; cmd_len = 0; param_len = 0; }
                    break;
                case ST_CMD:
                    if (c == '$') {
                        cmd[cmd_len] = 0;
                        state = ST_PARAM;
                    } else if (c == '#') {
                        cmd[cmd_len] = 0;
                        param[0] = 0;
                        cli_dispatch(cmd, param);
                        state = ST_IDLE;
                    } else if (cmd_len < CLI_CMD_MAX - 1) {
                        cmd[cmd_len++] = c;
                    } else {
                        state = ST_IDLE;   // overflow
                    }
                    break;
                case ST_PARAM:
                    if (c == '#') {
                        param[param_len] = 0;
                        cli_dispatch(cmd, param);
                        state = ST_IDLE;
                    } else if (param_len < CLI_PARAM_MAX - 1) {
                        param[param_len++] = c;
                    } else {
                        state = ST_IDLE;   // overflow
                    }
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
