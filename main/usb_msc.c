#include "usb_msc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_attr.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

#include "sd.h"
#include "ui.h"

static const char *TAG = "usb_msc";

// Egyszer-használatos boot-flag az RTC-memóriában. RTC_NOINIT_ATTR: nem
// nullázza a startup, túléli a reset-et / deep sleep-et, de a power-loss
// elveszti — épp ezt akarjuk: szándékos reboot → MSC mód, ki-be kapcsolás →
// normál mód. A belépéskor töröljük, így bármilyen következő reset is normál
// módba tér vissza. A MAGIC egyedi konstans (a véletlen egyezés ~1/2^32).
#define MSC_FLAG_MAGIC  0x5D3C0DEAu
static RTC_NOINIT_ATTR uint32_t s_msc_flag;

void usb_msc_request_reboot(void)
{
    s_msc_flag = MSC_FLAG_MAGIC;
    ESP_LOGI(TAG, "USB MSC kérés — újraindulás");
    esp_restart();
}

bool usb_msc_boot_requested(void)
{
    if (s_msc_flag == MSC_FLAG_MAGIC) {
        s_msc_flag = 0;     // egyszer-használatos: a következő boot már normál
        return true;
    }
    return false;
}

void usb_msc_run(void)
{
    ESP_LOGI(TAG, "USB MSC mód indul");

    // 1) SD kártya nyers init (FAT-mount NÉLKÜL) — ez inicializálja a közös
    //    SPI buszt is, amit az ui_init újrahasznosít.
    sdmmc_card_t *card = sd_init_card_raw();

    // 2) Panel + LVGL, "USB mód" képernyő, háttérvilágítás fel.
    ui_init();
    ui_show_usb_mode_screen(card != NULL);
    ui_display_ready();

    // 3) LVGL befagyasztása: a port-lockot innentől végig tartjuk, így a
    //    taskLVGL nem flushel többet — a közös SPI busz szabad az MSC-nek.
    ui_suspend_for_msc();

    if (!card) {
        ESP_LOGE(TAG, "nincs SD kártya — MSC nem indul");
        while (1) vTaskDelay(portMAX_DELAY);
    }

    // 4) TinyUSB MSC: a kártya kitevése a hostnak írható-olvasható meghajtóként.
    tinyusb_msc_driver_config_t driver_cfg = {
        .callback = NULL,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(tinyusb_msc_install_driver(&driver_cfg));

    tinyusb_msc_storage_config_t storage_cfg = {
        .medium.card = card,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,   // a host birtokolja
        .fat_fs = {
            .base_path = NULL,
            .config.max_files = 3,
            .do_not_format = true,    // SOHA ne formázzuk a felhasználó kártyáját
            .format_flags = 0,
        },
    };
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_sdmmc(&storage_cfg, NULL));

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    ESP_LOGI(TAG, "USB MSC aktív — SD a natív USB-n (GPIO19/20), írható-olvasható");

    // A munkát a TinyUSB task végzi; itt csak életben tartjuk a taskot (és vele
    // az LVGL port-lockot). Kilépés: fizikai power-cycle / reset.
    while (1) vTaskDelay(portMAX_DELAY);
}
