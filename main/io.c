#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "iot_button.h"
#include "button_gpio.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "app_config.h"
#include "io.h"

static const char *TAG = "io";

static btn_cb_t s_btn_cb = NULL;
static bat_cb_t s_bat_cb = NULL;

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

void io_init(void)
{
    setup_button(PIN_BTN_PLAY,     BTN_EVT_PLAY_PAUSE, false);
    setup_button(PIN_BTN_NEXT,     BTN_EVT_NEXT,       false);
    setup_button(PIN_BTN_PREV,     BTN_EVT_PREV,       false);
    setup_button(PIN_BTN_MENU,     BTN_EVT_MENU,       false);
    setup_button(PIN_BTN_VOL_UP,   BTN_EVT_VOL_UP,     true);   // hold = ramp
    setup_button(PIN_BTN_VOL_DOWN, BTN_EVT_VOL_DOWN,   true);

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
}

void io_register_button_cb(btn_cb_t cb)  { s_btn_cb = cb; }
void io_register_battery_cb(bat_cb_t cb) { s_bat_cb = cb; }
