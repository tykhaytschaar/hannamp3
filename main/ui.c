#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "app_config.h"
#include "ui.h"

static const char *TAG = "ui";

static lv_display_t *s_disp = NULL;

// LVGL widgetek
static lv_obj_t *lbl_title;
static lv_obj_t *lbl_time;
static lv_obj_t *bar_progress;
static lv_obj_t *lbl_state;
static lv_obj_t *lbl_volume;
static lv_obj_t *lbl_battery;
static lv_obj_t *img_cover;
static lv_obj_t *list_view;

static char s_cover_path[MAX_PATH_LEN];

// 320×240 fekvő layout
static void ui_build_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_text_color(scr, lv_color_white(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    img_cover = lv_image_create(scr);
    lv_obj_set_size(img_cover, 128, 128);
    lv_obj_align(img_cover, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_bg_color(img_cover, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(img_cover, LV_OPA_COVER, 0);

    lbl_battery = lv_label_create(scr);
    lv_obj_align(lbl_battery, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_label_set_text(lbl_battery, LV_SYMBOL_BATTERY_FULL " ---");

    // Cím — scroll helyett "..." levágás (scroll megakasztaná az audiót).
    lbl_title = lv_label_create(scr);
    lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_title, 168);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, 144, 8);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_title, "—");

    lbl_time = lv_label_create(scr);
    lv_obj_align(lbl_time, LV_ALIGN_TOP_LEFT, 144, 46);
    lv_label_set_text(lbl_time, "0:00 / 0:00");

    bar_progress = lv_bar_create(scr);
    lv_obj_set_size(bar_progress, 168, 6);
    lv_obj_align(bar_progress, LV_ALIGN_TOP_LEFT, 144, 70);
    lv_bar_set_range(bar_progress, 0, 1000);
    lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);

    lbl_state = lv_label_create(scr);
    lv_obj_align(lbl_state, LV_ALIGN_TOP_LEFT, 144, 86);
    lv_label_set_text(lbl_state, LV_SYMBOL_PAUSE);

    lbl_volume = lv_label_create(scr);
    lv_obj_align(lbl_volume, LV_ALIGN_TOP_LEFT, 172, 86);
    lv_label_set_text(lbl_volume, LV_SYMBOL_VOLUME_MAX " 70%");

    list_view = lv_list_create(scr);
    lv_obj_set_size(list_view, 304, 84);
    lv_obj_align(list_view, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    lv_obj_set_style_bg_color(list_view, lv_color_black(), 0);
    lv_obj_set_style_text_color(list_view, lv_color_white(), 0);
    lv_obj_set_style_border_width(list_view, 0, 0);
    lv_obj_set_style_pad_all(list_view, 2, 0);
}

void ui_init(void)
{
    ESP_LOGI(TAG, "ST7789 + LVGL init");

    // ---- esp_lcd panel IO (SPI2, közös busz az SD-vel) ----
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_TFT_DC,
        .cs_gpio_num = PIN_TFT_CS,
        .pclk_hz = LCD_SPI_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SD_SPI_HOST, &io_cfg, &io_handle));

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = PIN_TFT_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &dev_cfg, &panel));
    esp_lcd_panel_reset(panel);
    vTaskDelay(pdMS_TO_TICKS(100));            // extra idő a chipnek reset után
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true);   // IPS ST7789 szinte mindig kell
    // A rotációt NEM itt állítjuk — az esp_lvgl_port a disp_cfg.rotation
    // alapján felülírná. A fekvő beállítás lentebb, a disp_cfg-ban van.
    esp_lcd_panel_disp_on_off(panel, true);

    // ---- LVGL port ----
    // 4 KB stack a default — kevés a rajzolás+image decoder kombinációhoz.
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack = 8192;
    lvgl_cfg.task_affinity = 0;   // Core 0 — az audio Core 1-en, ne versengjenek CPU-ért
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * 40,                // 40 sornyi tile
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        // Fekvő, 90° CCW — ezt a port alkalmazza a panelra (egyszer).
        .rotation = { .swap_xy = true, .mirror_x = false, .mirror_y = true },
        .flags = { .buff_dma = true, .buff_spiram = false },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);

    // ---- UI build ----
    lvgl_port_lock(0);
    ui_build_screen();
    lvgl_port_unlock();
}

// --- API ---

void ui_show_track(const track_t *tr)
{
    lvgl_port_lock(0);
    char title[256];
    if (tr->album[0]) {
        snprintf(title, sizeof(title), "%.95s — %.127s", tr->album, tr->name);
    } else {
        snprintf(title, sizeof(title), "%.127s", tr->name);
    }
    lv_label_set_text(lbl_title, title);

    // Album art keresése: cover.jpg / folder.jpg / azonos nevű .jpg
    char art[MAX_PATH_LEN];
    extern bool sd_find_album_art(const char *mp3_path, char *out_path, int out_path_len);
    if (sd_find_album_art(tr->path, art, sizeof(art))) {
        // LVGL POSIX fájlrendszer 'S:' meghajtón át éri el a /sdcard-ot.
        snprintf(s_cover_path, sizeof(s_cover_path), "S:%s", art + strlen(SD_MOUNT_POINT));
        lv_image_set_src(img_cover, s_cover_path);
        lv_obj_clear_flag(img_cover, LV_OBJ_FLAG_HIDDEN);
    } else {
        // NULL forrás helyett a widgetet eltakarjuk — biztonságosabb LVGL v9-en.
        lv_obj_add_flag(img_cover, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

void ui_show_no_track(void)
{
    lvgl_port_lock(0);
    lv_label_set_text(lbl_title, "(no tracks on SD)");
    lv_obj_add_flag(img_cover, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void ui_set_playing(bool playing)
{
    lvgl_port_lock(0);
    lv_label_set_text(lbl_state, playing ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    lvgl_port_unlock();
}

static void fmt_mmss(uint32_t ms, char *buf, int n)
{
    uint32_t s = ms / 1000;
    snprintf(buf, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

void ui_set_progress(uint32_t pos_ms, uint32_t dur_ms)
{
    lvgl_port_lock(0);
    int32_t pct = (dur_ms > 0) ? (int32_t)((uint64_t)pos_ms * 1000 / dur_ms) : 0;
    if (pct > 1000) pct = 1000;
    lv_bar_set_value(bar_progress, pct, LV_ANIM_OFF);
    char a[16], b[16], line[40];
    fmt_mmss(pos_ms, a, sizeof(a));
    fmt_mmss(dur_ms, b, sizeof(b));
    snprintf(line, sizeof(line), "%s / %s", a, b);
    lv_label_set_text(lbl_time, line);
    lvgl_port_unlock();
}

void ui_set_volume(uint8_t vol)
{
    lvgl_port_lock(0);
    char s[24];
    const char *sym = (vol == 0) ? LV_SYMBOL_MUTE :
                      (vol < 33)  ? LV_SYMBOL_VOLUME_MID :
                                    LV_SYMBOL_VOLUME_MAX;
    snprintf(s, sizeof(s), "%s %u%%", sym, vol);
    lv_label_set_text(lbl_volume, s);
    lvgl_port_unlock();
}

void ui_set_battery(uint16_t mv, uint8_t percent)
{
    lvgl_port_lock(0);
    char s[32];
    const char *sym = (percent > 80) ? LV_SYMBOL_BATTERY_FULL :
                      (percent > 55) ? LV_SYMBOL_BATTERY_3 :
                      (percent > 30) ? LV_SYMBOL_BATTERY_2 :
                      (percent > 10) ? LV_SYMBOL_BATTERY_1 :
                                       LV_SYMBOL_BATTERY_EMPTY;
    snprintf(s, sizeof(s), "%s %u%% (%u mV)", sym, percent, mv);
    lv_label_set_text(lbl_battery, s);
    lvgl_port_unlock();
}

void ui_spi_lock(void)   { lvgl_port_lock(0); }
void ui_spi_unlock(void) { lvgl_port_unlock(); }

void ui_set_playlist(const track_t *tracks, int count, int current_idx)
{
    lvgl_port_lock(0);
    lv_obj_clean(list_view);
    int start = current_idx - 1;
    if (start < 0) start = 0;
    int end = start + 3;
    if (end > count) end = count;
    for (int i = start; i < end; i++) {
        lv_obj_t *btn = lv_list_add_button(list_view, NULL, tracks[i].name);
        if (i == current_idx) {
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x305080), 0);
            lv_obj_set_style_text_color(btn, lv_color_white(), 0);
        }
    }
    lvgl_port_unlock();
}
