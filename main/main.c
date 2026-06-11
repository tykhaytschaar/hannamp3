#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "rom/gpio.h"

#include "app_config.h"
#include "sd.h"
#include "audio.h"
#include "ui.h"
#include "io.h"
#include "player.h"
#include "cli.h"
#include "usb_msc.h"

// Mindkét CS lábat explicit HIGH-ra húzzuk MIELŐTT a SPI master bármit
// elkezdene csinálni. Így a köztes időben (SD init → LCD init) sem lebeg
// egyik chip select sem — a panel chip nem értelmezi a SD-nek szóló MOSI
// byteokat sajátnak.
static void pre_init_cs_pins(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_TFT_CS) | (1ULL << PIN_SD_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(PIN_TFT_CS, 1);
    gpio_set_level(PIN_SD_CS, 1);
}

// Háttérvilágítás már a legkorábbi ponton OFF. A panel power-on fehér
// flash-ét és a boot alatti üres/placeholder fázist így nem látni — a
// kijelzőt az ui_display_ready() kapcsolja fel, amikor minden betöltődött.
static void pre_init_backlight_off(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(PIN_BL, 0);
}

// GPIO 40 fixen LOW szintre (külső HW követelmény).
static void init_static_low_pins(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << GPIO_NUM_40,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(GPIO_NUM_40, 0);
}


static const char *TAG = "main";

// Deep sleep wake esetén megköveteljük, hogy a Menu gomb 500 ms-ig le legyen
// nyomva. Ha közben elengedik (véletlen érintés), azonnal visszaalszunk a
// teljes inicializálás megspórolásával. Ezt main.c eleje kell, mielőtt
// drága init-ek lefutnak.
static void deep_sleep_wake_gate(void)
{
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) return;

    gpio_config_t mc = {
        .pin_bit_mask = 1ULL << PIN_BTN_MENU,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&mc);

    // 500 ms folyamatosan LOW kell. 25 × 20 ms minta — bármikor felenged → re-sleep.
    for (int i = 0; i < 25; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (gpio_get_level(PIN_BTN_MENU) != 0) {
            esp_sleep_enable_ext1_wakeup_io(1ULL << PIN_BTN_MENU, ESP_EXT1_WAKEUP_ANY_LOW);
            esp_deep_sleep_start();
        }
    }
    // 500 ms-on át lenyomva — folytatás normál boot-tal.
}

void app_main(void)
{
    ESP_LOGI(TAG, "hannamp3 boot");

    deep_sleep_wake_gate();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // CS-eket még a SPI inicializálás előtt HIGH-ra — különben a köztes
    // pillanatban a TFT chip lebegő CS-szel SD adatokat venne fel.
    pre_init_cs_pins();
    pre_init_backlight_off();
    init_static_low_pins();

    // USB MSC boot-ág: ha a Settings-ből ide kértek újraindulást, a normál
    // init helyett az SD-t a natív USB-n külső meghajtóként tesszük ki.
    // usb_msc_run() sosem tér vissza. (A flag egyszer-használatos: belépéskor
    // törlődik, így bármilyen következő reset/power-cycle normál módba tér.)
    if (usb_msc_boot_requested()) {
        usb_msc_run();
    }

    // Sorrend fontos: SD a SPI buszt is inicializálja, amit az UI újrahasznosít.
    sd_init();
    ui_init();

    // Boot splash: a flash-be ágyazott frame-szett lejátszása. Itt, a
    // player_start (SD-szkennelés) ELŐTT fut, így a flash-ből dekódolt
    // frame-ek kirajzolása sosem esik egybe SD-olvasással. Az utolsó frame
    // kint marad, amíg az ui_display_ready le nem cseréli a kész UI-ra.
    ui_play_boot_splash();

    audio_init();
    io_init();

    player_start();

    // Minden betöltve (címek, böngésző, állapot): most már felkapcsolhatjuk a
    // háttérvilágítást — a boot alatti fehér flash / üres fázis így rejtve marad.
    ui_display_ready();

    cli_init();

    ESP_LOGI(TAG, "init done, free heap=%lu internal=%lu psram=%lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
