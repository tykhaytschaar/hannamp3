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

// OCV→SoC tábla, tipikus 1S LiPo nyugalmi görbe. A lineáris 3300–4200 skála a
// 3.8 V-os platón durván félremér (ott van a kapacitás dereka); a pontok közt
// lineárisan interpolálunk.
static const struct { uint16_t mv; uint8_t pct; } OCV[] = {
    {BAT_FULL_MV, 100}, {4110, 92}, {4020, 84}, {3950, 75}, {3870, 62},
    {3840, 53}, {3800, 42}, {3770, 32}, {3730, 20}, {3690, 10}, {3610, 5},
    {BAT_EMPTY_MV, 0},
};

uint8_t io_battery_percent_from_mv(uint16_t mv)
{
    const int n = sizeof(OCV) / sizeof(OCV[0]);
    if (mv >= OCV[0].mv)     return 100;
    if (mv <= OCV[n-1].mv)   return 0;
    for (int i = 1; i < n; i++) {
        if (mv >= OCV[i].mv) {
            uint32_t span = OCV[i-1].mv - OCV[i].mv;
            uint32_t up   = mv - OCV[i].mv;
            return OCV[i].pct +
                   (uint8_t)(((uint32_t)(OCV[i-1].pct - OCV[i].pct) * up + span / 2) / span);
        }
    }
    return 0;
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
    // Az S3 ADC belső zaja ±20–30 mV, amit a burst-átlag nem szűr ki (a 16
    // minta µs-okon belül készül, a flicker-zaj korrelált). Ezért a méréseket
    // időben is simítjuk: futó exponenciális átlag (EMA) az 5 s-onkénti
    // mintákra. A kijelzett százalék ezen felül monoton: csak lefelé követi a
    // mérést, mert a terhelés alatti beroskadás (~100 mV) valódi, de fel-le
    // rángatná a kijelzést. Felfelé csak nagy tartós ugrás engedi (>=15%,
    // az a töltő; a terhelés-elengedés visszapattanása csak ~8-10%).
    float ema_mv   = 0;
    int   disp_pct = -1;
    while (1) {
        uint16_t mv = io_read_battery_mv();
        ema_mv = (ema_mv == 0) ? mv : ema_mv + 0.25f * ((float)mv - ema_mv);

        uint16_t smooth_mv = (uint16_t)(ema_mv + 0.5f);
        int pct = io_battery_percent_from_mv(smooth_mv);

        int new_disp = disp_pct;
        if (disp_pct < 0 || pct < disp_pct || pct - disp_pct >= 15)
            new_disp = pct;
        if (new_disp != disp_pct && s_bat_cb) {
            s_bat_cb(smooth_mv, (uint8_t)new_disp);
            disp_pct = new_disp;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// A lakat slide switch megszűnt. A lock-állapotot később egy gomb hosszú
// nyomása fogja billenteni (TODO) — addig s_locked végig false marad.
bool io_is_locked(void) { return s_locked; }

// Nyers gombállapot a game mode polling-jához. A gombok GND-re zárnak
// (belső pull-up, lásd setup_button active_level=0) → LOW = lenyomva.
static int evt_to_gpio(btn_event_t evt)
{
    switch (evt) {
    case BTN_EVT_A:     return PIN_BTN_A;
    case BTN_EVT_B:     return PIN_BTN_B;
    case BTN_EVT_UP:    return PIN_BTN_UP;
    case BTN_EVT_DOWN:  return PIN_BTN_DOWN;
    case BTN_EVT_LEFT:  return PIN_BTN_LEFT;
    case BTN_EVT_RIGHT: return PIN_BTN_RIGHT;
    case BTN_EVT_X:     return PIN_BTN_X;
    case BTN_EVT_Y:     return PIN_BTN_Y;
    }
    return -1;
}

bool io_button_down(btn_event_t evt)
{
    int pin = evt_to_gpio(evt);
    return pin >= 0 && gpio_get_level(pin) == 0;
}

// IDEIGLENES: nyers gombállapot-logoló. Az iot_button mellett fut, a live
// GPIO-szintet olvassa (a setup_button már pull-uppal inputra állította).
static void btn_debug_task(void *arg)
{
    static const struct { int gpio; const char *name; } P[] = {
        { PIN_BTN_UP, "Up" }, { PIN_BTN_DOWN, "Down" }, { PIN_BTN_LEFT, "Left" },
        { PIN_BTN_RIGHT, "Right" }, { PIN_BTN_A, "A" }, { PIN_BTN_B, "B" },
        { PIN_BTN_X, "X" }, { PIN_BTN_Y, "Y" },
    };
    int prev[8];
    for (int i = 0; i < 8; i++) prev[i] = gpio_get_level(P[i].gpio);
    ESP_LOGW(TAG, "BTN-DEBUG aktiv — nyomkodj gombokat, figyeld a logot");
    while (1) {
        for (int i = 0; i < 8; i++) {
            int lv = gpio_get_level(P[i].gpio);
            if (lv != prev[i]) {
                ESP_LOGW(TAG, "%-6s GPIO %2d -> %s", P[i].name, P[i].gpio,
                         lv ? "fel" : "LENYOMVA");
                prev[i] = lv;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void io_init(void)
{
    setup_button(PIN_BTN_A,     BTN_EVT_A,     false);
    setup_button(PIN_BTN_B,     BTN_EVT_B,     false);
    setup_button(PIN_BTN_X,     BTN_EVT_X,     false);
    setup_button(PIN_BTN_Y,     BTN_EVT_Y,     false);
    setup_button(PIN_BTN_RIGHT, BTN_EVT_RIGHT, false);
    setup_button(PIN_BTN_LEFT,  BTN_EVT_LEFT,  false);
    setup_button(PIN_BTN_UP,    BTN_EVT_UP,    true);    // hold = vol ramp
    setup_button(PIN_BTN_DOWN,  BTN_EVT_DOWN,  true);

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

    // ---- IDEIGLENES gomb-diagnosztika ----
    // Nyers GPIO-szint pollozás mind a 8 gombra, váltáskor logol. Megkerüli az
    // iot_button-t → eldönti, hogy a Bal/Jobb (GPIO 2/1) láb reagál-e (FW), vagy
    // a vezeték a hibás (HW). Mérés után törlendő.
    xTaskCreate(btn_debug_task, "btndbg", 3072, NULL, 3, NULL);
}

void io_register_button_cb(btn_cb_t cb)   { s_btn_cb  = cb; }
void io_register_battery_cb(bat_cb_t cb)  { s_bat_cb  = cb; }
void io_register_lock_cb(lock_cb_t cb)    { s_lock_cb = cb; }
