#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "iot_button.h"
#include "button_gpio.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "app_config.h"
#include "io.h"

static const char *TAG = "io";

static btn_cb_t  s_btn_cb  = NULL;
static bat_cb_t  s_bat_cb  = NULL;
static lock_cb_t s_lock_cb = NULL;

static volatile bool s_locked = false;

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali = NULL;

static int gpio_to_adc_channel(int gpio)
{
    // ESP32-S3 ADC1: GPIO 1..10 = CH0..CH9
    return gpio - 1;
}

static void emit(btn_event_t e)
{
    if (s_btn_cb) s_btn_cb(e);
}

static void on_btn(void *btn, void *usr)
{
    btn_event_t evt = (btn_event_t)(intptr_t)usr;
    emit(evt);
}

// Vol+/Vol- gombokra a BUTTON_LONG_PRESS_HOLD event ad ~100 ms-onként újra
// kiváltást tartás közben — finom hold-to-ramp.
static void on_btn_repeat(void *btn, void *usr)
{
    btn_event_t evt = (btn_event_t)(intptr_t)usr;
    emit(evt);
}

static void setup_button(int gpio, btn_event_t evt, bool repeat_on_hold)
{
    button_config_t cfg = {
        .type = BUTTON_TYPE_GPIO,
        .long_press_time = 500,
        .short_press_time = 80,
        .gpio_button_config = {
            .gpio_num     = gpio,
            .active_level = 0,
        },
    };
    button_handle_t h = iot_button_create(&cfg);
    if (!h) {
        ESP_LOGE(TAG, "iot_button_create failed for GPIO %d", gpio);
        return;
    }
    iot_button_register_cb(h, BUTTON_SINGLE_CLICK, on_btn, (void *)(intptr_t)evt);
    if (repeat_on_hold) {
        iot_button_register_cb(h, BUTTON_LONG_PRESS_HOLD, on_btn_repeat, (void *)(intptr_t)evt);
    }
}

// MENU külön kezelést kap: short = screen váltás, long = SD rescan.
static void setup_menu_button(int gpio)
{
    button_config_t cfg = {
        .type = BUTTON_TYPE_GPIO,
        .long_press_time = 800,
        .short_press_time = 80,
        .gpio_button_config = {
            .gpio_num     = gpio,
            .active_level = 0,
        },
    };
    button_handle_t h = iot_button_create(&cfg);
    if (!h) {
        ESP_LOGE(TAG, "iot_button_create failed for GPIO %d", gpio);
        return;
    }
    iot_button_register_cb(h, BUTTON_SINGLE_CLICK,    on_btn,
                           (void *)(intptr_t)BTN_EVT_MENU);
    iot_button_register_cb(h, BUTTON_LONG_PRESS_START, on_btn,
                           (void *)(intptr_t)BTN_EVT_MENU_LONG);
}

uint8_t io_battery_percent_from_mv(uint16_t mv)
{
    if (mv >= BAT_FULL_MV)  return 100;
    if (mv <= BAT_EMPTY_MV) return 0;
    return (uint8_t)(((uint32_t)(mv - BAT_EMPTY_MV) * 100) / (BAT_FULL_MV - BAT_EMPTY_MV));
}

uint16_t io_read_battery_mv(void)
{
    if (!s_adc) return 0;
    int ch = gpio_to_adc_channel(PIN_BAT_ADC);
    int raw = 0;
    int acc = 0;
    for (int i = 0; i < 16; i++) {
        adc_oneshot_read(s_adc, ch, &raw);
        acc += raw;
    }
    raw = acc / 16;

    int mv = 0;
    if (s_cali) {
        adc_cali_raw_to_voltage(s_cali, raw, &mv);
    } else {
        // Kalibráció nélkül durva becslés: 12-bit @ 12dB ≈ 0–3100 mV
        mv = (raw * 3100) / 4095;
    }
    return (uint16_t)(mv * BAT_DIVIDER_RATIO);
}

static void battery_task(void *arg)
{
    while (1) {
        uint16_t mv  = io_read_battery_mv();
        uint8_t  pct = io_battery_percent_from_mv(mv);
        if (s_bat_cb) s_bat_cb(mv, pct);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// LOCK tolókapcsoló pollozása. Polling 100 ms-onként + 30 ms debounce
// (slide switch néha visszapattan).
static void lock_task(void *arg)
{
    bool prev_raw = (gpio_get_level(PIN_LOCK_SWITCH) == 0);
    s_locked = prev_raw;
    if (s_lock_cb) s_lock_cb(s_locked);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        bool now_raw = (gpio_get_level(PIN_LOCK_SWITCH) == 0);
        if (now_raw != prev_raw) {
            vTaskDelay(pdMS_TO_TICKS(30));
            now_raw = (gpio_get_level(PIN_LOCK_SWITCH) == 0);
        }
        if (now_raw != s_locked) {
            s_locked = now_raw;
            if (s_lock_cb) s_lock_cb(s_locked);
        }
        prev_raw = now_raw;
    }
}

bool io_is_locked(void) { return s_locked; }

// Nyers gombállapot a game mode polling-jához. A gombok GND-re zárnak
// (belső pull-up, lásd setup_button active_level=0) → LOW = lenyomva.
static int evt_to_gpio(btn_event_t evt)
{
    switch (evt) {
    case BTN_EVT_A:         return PIN_BTN_A;
    case BTN_EVT_B:         return PIN_BTN_B;
    case BTN_EVT_UP:        return PIN_BTN_UP;
    case BTN_EVT_DOWN:      return PIN_BTN_DOWN;
    case BTN_EVT_LEFT:      return PIN_BTN_LEFT;
    case BTN_EVT_RIGHT:     return PIN_BTN_RIGHT;
    case BTN_EVT_START:     return PIN_BTN_START;
    case BTN_EVT_SELECT:    return PIN_BTN_SELECT;
    case BTN_EVT_MENU:
    case BTN_EVT_MENU_LONG: return PIN_BTN_MENU;
    }
    return -1;
}

bool io_button_down(btn_event_t evt)
{
    int pin = evt_to_gpio(evt);
    return pin >= 0 && gpio_get_level(pin) == 0;
}

void io_init(void)
{
    setup_button(PIN_BTN_A,      BTN_EVT_A,      false);
    setup_button(PIN_BTN_B,      BTN_EVT_B,      false);
    setup_button(PIN_BTN_RIGHT,  BTN_EVT_RIGHT,  false);
    setup_button(PIN_BTN_LEFT,   BTN_EVT_LEFT,   false);
    setup_button(PIN_BTN_START,  BTN_EVT_START,  false);
    setup_button(PIN_BTN_SELECT, BTN_EVT_SELECT, false);
    setup_menu_button(PIN_BTN_MENU);                       // short + long
    setup_button(PIN_BTN_UP,     BTN_EVT_UP,     true);    // hold = vol ramp
    setup_button(PIN_BTN_DOWN,   BTN_EVT_DOWN,   true);

    // ---- ADC1 oneshot a battery-hez ----
    adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT_1, .ulp_mode = ADC_ULP_MODE_DISABLE };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&u, &s_adc));

    adc_oneshot_chan_cfg_t c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, gpio_to_adc_channel(PIN_BAT_ADC), &c));

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t e = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration unavailable (%s) — voltages will be approximate", esp_err_to_name(e));
    }

    xTaskCreate(battery_task, "battery", 4096, NULL, 3, NULL);

    // ---- LOCK tolókapcsoló ----
    gpio_config_t lk = {
        .pin_bit_mask = 1ULL << PIN_LOCK_SWITCH,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&lk);
    xTaskCreate(lock_task, "lock", 2048, NULL, 3, NULL);
}

void io_register_button_cb(btn_cb_t cb)   { s_btn_cb  = cb; }
void io_register_battery_cb(bat_cb_t cb)  { s_bat_cb  = cb; }
void io_register_lock_cb(lock_cb_t cb)    { s_lock_cb = cb; }
