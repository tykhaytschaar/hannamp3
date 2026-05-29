#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "app_config.h"
#include "ui.h"
#include "mp3_fonts.h"

static const char *TAG = "ui";

// -----------------------------------------------------------------------------
// Theme tokens (új sötét/cyan design, 320×240-re hangolva)
// -----------------------------------------------------------------------------
#define COL_BG          lv_color_hex(0x0E1116)
#define COL_BG_PANEL    lv_color_hex(0x161B22)
#define COL_BG_PANEL_2  lv_color_hex(0x1F2630)
#define COL_TEXT        lv_color_hex(0xE6EDF3)
#define COL_TEXT_DIM    lv_color_hex(0x8B98A5)
#define COL_ACCENT      lv_color_hex(0x2EE6D6)
#define COL_ACCENT_DIM  lv_color_hex(0x1A8A80)

// -----------------------------------------------------------------------------
// Module state
// -----------------------------------------------------------------------------
static lv_display_t *s_disp = NULL;

// Idle / energy management — 30 s után a panel DISPOFF, user-eseményre vissza.
#define UI_IDLE_TIMEOUT_MS  30000
static esp_lcd_panel_handle_t s_panel = NULL;
static int64_t                s_last_activity_us = 0;
static bool                   s_disp_off = false;

typedef struct {
    // Screens
    lv_obj_t *scr[UI_SCREEN_COUNT];
    ui_screen_t current;

    // Persistent overlay (battery + screen chip rendered on lv_layer_top)
    lv_obj_t *ovr_screen_chip;
    lv_obj_t *ovr_battery;

    // Now Playing widgets
    lv_obj_t *np_img_cover;
    lv_obj_t *np_lbl_title;
    lv_obj_t *np_lbl_subtitle;
    lv_obj_t *np_lbl_time;
    lv_obj_t *np_bar_progress;
    lv_obj_t *np_lbl_state;
    lv_obj_t *np_lbl_volume;
    lv_obj_t *np_mini_list;

    // Library (fájlböngésző) widgetek
    lv_obj_t *lib_list;
    lv_obj_t *lib_path;   // aktuális könyvtár fejléc

    // Settings widgets
    lv_obj_t *set_val_volume;
    lv_obj_t *set_val_battery;
    lv_obj_t *set_val_tracks;
    lv_obj_t *set_val_heap;

    // Now Playing mini-lista adatai (a játszó album, player.c birtokolja)
    const track_t *lib_tracks;
    int lib_count;
    int lib_current;   // currently playing track (highlight)

    // Böngésző adatai (Library képernyő, player.c birtokolja)
    const dir_entry_t *br_entries;
    int br_count;
    int br_cursor;
} ui_t;

static ui_t U;

static char s_cover_path[MAX_PATH_LEN];

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------
static void build_overlay(void);
static void build_now_playing(void);
static void build_library(void);
static void build_settings(void);

static void apply_screen_bg(lv_obj_t *scr);
static lv_obj_t *make_panel(lv_obj_t *parent);
static void update_screen_chip(void);
static void browser_rebuild_list(void);
static void browser_apply_cursor(void);
static void update_mini_playlist(void);
static void settings_refresh_heap(void);

// -----------------------------------------------------------------------------
// LCD bring-up + LVGL port — VÁLTOZATLAN a korábbi kódhoz képest
// -----------------------------------------------------------------------------
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
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_disp_on_off(panel, true);
    s_panel = panel;                                  // idle/wake-hez
    s_last_activity_us = esp_timer_get_time();

    // ---- LVGL port ----
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack = 8192;
    lvgl_cfg.task_affinity = 0;
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * 40,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = { .swap_xy = true, .mirror_x = false, .mirror_y = true },
        .flags = { .buff_dma = true, .buff_spiram = false, .swap_bytes = true },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);

    // ---- UI build ----
    lvgl_port_lock(0);
    memset(&U, 0, sizeof(U));
    U.br_cursor = 0;
    U.lib_current = -1;

    for (int i = 0; i < UI_SCREEN_COUNT; i++) {
        U.scr[i] = lv_obj_create(NULL);
        apply_screen_bg(U.scr[i]);
    }

    build_overlay();
    build_now_playing();
    build_library();
    build_settings();

    U.current = UI_SCREEN_NOW_PLAYING;
    lv_screen_load(U.scr[U.current]);
    update_screen_chip();
    lvgl_port_unlock();
}

// -----------------------------------------------------------------------------
// Common helpers
// -----------------------------------------------------------------------------
static void apply_screen_bg(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(scr, COL_TEXT, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    // Default font az egész screen-en — minden label örökli, kivéve ahol
    // explicit font van beállítva (title 18, subtitle 14).
    lv_obj_set_style_text_font(scr, &mp3_inter_12, LV_PART_MAIN);
}

static lv_obj_t *make_panel(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_style_bg_color(p, COL_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(p, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(p, 8, LV_PART_MAIN);
    return p;
}

// -----------------------------------------------------------------------------
// Persistent overlay (screen chip felül balra, battery felül jobbra)
// Mind a három képernyőn látszik, mert lv_layer_top()-on van.
// -----------------------------------------------------------------------------
static void build_overlay(void)
{
    lv_obj_t *top = lv_layer_top();
    // Az overlay-en kattintásokat nem akarunk elnyelni — pl. a list scroll
    // alatta legyen elérhető.
    lv_obj_remove_flag(top, LV_OBJ_FLAG_CLICKABLE);

    U.ovr_screen_chip = lv_label_create(top);
    lv_obj_set_style_text_color(U.ovr_screen_chip, COL_ACCENT, LV_PART_MAIN);
    lv_label_set_text(U.ovr_screen_chip, "NOW");
    lv_obj_align(U.ovr_screen_chip, LV_ALIGN_TOP_LEFT, 8, 6);

    U.ovr_battery = lv_label_create(top);
    lv_obj_set_style_text_color(U.ovr_battery, COL_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text(U.ovr_battery, LV_SYMBOL_BATTERY_FULL " ---");
    lv_obj_align(U.ovr_battery, LV_ALIGN_TOP_RIGHT, -8, 6);
}

static void update_screen_chip(void)
{
    static const char *labels[UI_SCREEN_COUNT] = { "NOW", "LIB", "SET" };
    if (!U.ovr_screen_chip) return;
    lv_label_set_text(U.ovr_screen_chip, labels[U.current]);
}

// -----------------------------------------------------------------------------
// Screen 1: Now Playing
// Layout 320×240:
//   y 0..24    : overlay header (screen chip + battery)
//   y 28..148  : cover 120×120 (bal) + info blokk (jobb)
//   y 152..232 : mini playlist panel (3 sor)
// -----------------------------------------------------------------------------
static void build_now_playing(void)
{
    lv_obj_t *scr = U.scr[UI_SCREEN_NOW_PLAYING];

    // Cover (bal oldal)
    U.np_img_cover = lv_image_create(scr);
    lv_obj_set_size(U.np_img_cover, 120, 120);
    lv_obj_align(U.np_img_cover, LV_ALIGN_TOP_LEFT, 8, 28);
    lv_obj_set_style_bg_color(U.np_img_cover, COL_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(U.np_img_cover, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(U.np_img_cover, 6, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(U.np_img_cover, true, LV_PART_MAIN);

    // Info blokk (jobb oldal): title, artist, progress, time, state + volume
    // Egységes baseline x=140, szélesség 172
    U.np_lbl_title = lv_label_create(scr);
    lv_label_set_long_mode(U.np_lbl_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(U.np_lbl_title, 172);
    lv_obj_align(U.np_lbl_title, LV_ALIGN_TOP_LEFT, 140, 30);
    lv_obj_set_style_text_font(U.np_lbl_title, &mp3_inter_18, 0);
    lv_obj_set_style_text_color(U.np_lbl_title, COL_TEXT, 0);
    lv_label_set_text(U.np_lbl_title, "—");

    U.np_lbl_subtitle = lv_label_create(scr);
    lv_label_set_long_mode(U.np_lbl_subtitle, LV_LABEL_LONG_DOT);
    lv_obj_set_width(U.np_lbl_subtitle, 172);
    lv_obj_align(U.np_lbl_subtitle, LV_ALIGN_TOP_LEFT, 140, 54);
    lv_obj_set_style_text_font(U.np_lbl_subtitle, &mp3_inter_14, 0);
    lv_obj_set_style_text_color(U.np_lbl_subtitle, COL_TEXT_DIM, 0);
    lv_label_set_text(U.np_lbl_subtitle, "");

    U.np_bar_progress = lv_bar_create(scr);
    lv_obj_set_size(U.np_bar_progress, 172, 6);
    lv_obj_align(U.np_bar_progress, LV_ALIGN_TOP_LEFT, 140, 90);
    lv_bar_set_range(U.np_bar_progress, 0, 1000);
    lv_bar_set_value(U.np_bar_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(U.np_bar_progress, COL_BG_PANEL_2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(U.np_bar_progress, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(U.np_bar_progress, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(U.np_bar_progress, 3, LV_PART_INDICATOR);

    U.np_lbl_time = lv_label_create(scr);
    lv_obj_align(U.np_lbl_time, LV_ALIGN_TOP_LEFT, 140, 102);
    lv_obj_set_style_text_color(U.np_lbl_time, COL_TEXT_DIM, 0);
    lv_label_set_text(U.np_lbl_time, "0:00 / 0:00");

    U.np_lbl_state = lv_label_create(scr);
    lv_obj_align(U.np_lbl_state, LV_ALIGN_TOP_LEFT, 140, 124);
    lv_obj_set_style_text_color(U.np_lbl_state, COL_ACCENT, 0);
    lv_label_set_text(U.np_lbl_state, LV_SYMBOL_STOP);   // boot-kor idle

    U.np_lbl_volume = lv_label_create(scr);
    lv_obj_align(U.np_lbl_volume, LV_ALIGN_TOP_LEFT, 168, 124);
    lv_obj_set_style_text_color(U.np_lbl_volume, COL_TEXT, 0);
    lv_label_set_text(U.np_lbl_volume, LV_SYMBOL_VOLUME_MAX " 70%");

    // Mini playlist (alul, 3 sor)
    U.np_mini_list = make_panel(scr);
    lv_obj_set_size(U.np_mini_list, 304, 80);
    lv_obj_align(U.np_mini_list, LV_ALIGN_BOTTOM_LEFT, 8, -6);
    lv_obj_set_flex_flow(U.np_mini_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(U.np_mini_list, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(U.np_mini_list, 4, LV_PART_MAIN);
}

// -----------------------------------------------------------------------------
// Screen 2: Library — full-screen scrollozó lista
// -----------------------------------------------------------------------------
static void build_library(void)
{
    lv_obj_t *scr = U.scr[UI_SCREEN_LIBRARY];

    // Aktuális könyvtár fejléc (a "LIB" chip alatt)
    U.lib_path = lv_label_create(scr);
    lv_label_set_long_mode(U.lib_path, LV_LABEL_LONG_DOT);
    lv_obj_set_width(U.lib_path, 304);
    lv_obj_align(U.lib_path, LV_ALIGN_TOP_LEFT, 8, 28);
    lv_obj_set_style_text_color(U.lib_path, COL_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text(U.lib_path, "/");

    U.lib_list = lv_list_create(scr);
    lv_obj_set_size(U.lib_list, 304, 184);
    lv_obj_align(U.lib_list, LV_ALIGN_TOP_LEFT, 8, 48);
    lv_obj_set_style_bg_color(U.lib_list, COL_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_border_width(U.lib_list, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(U.lib_list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(U.lib_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(U.lib_list, 2, LV_PART_MAIN);
}

// -----------------------------------------------------------------------------
// Screen 3: Settings — read-only status panel
// -----------------------------------------------------------------------------
static lv_obj_t *settings_row(lv_obj_t *parent, const char *icon, const char *label,
                              lv_obj_t **out_value)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(row, 2, LV_PART_MAIN);

    lv_obj_t *left = lv_label_create(row);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s  %s", icon, label);
    lv_label_set_text(left, buf);
    lv_obj_set_style_text_color(left, COL_TEXT_DIM, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, "—");
    lv_obj_set_style_text_color(val, COL_TEXT, 0);
    if (out_value) *out_value = val;
    return row;
}

static void build_settings(void)
{
    lv_obj_t *scr = U.scr[UI_SCREEN_SETTINGS];

    lv_obj_t *panel = make_panel(scr);
    lv_obj_set_size(panel, 304, 200);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 8, 30);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, 4, LV_PART_MAIN);

    settings_row(panel, LV_SYMBOL_VOLUME_MAX, "Volume",  &U.set_val_volume);
    settings_row(panel, LV_SYMBOL_BATTERY_FULL, "Battery", &U.set_val_battery);
    settings_row(panel, LV_SYMBOL_AUDIO, "Tracks",  &U.set_val_tracks);
    settings_row(panel, LV_SYMBOL_REFRESH, "Free heap", &U.set_val_heap);

    lv_obj_t *hint = lv_label_create(panel);
    lv_label_set_text(hint, "Menu long-press: rescan SD");
    lv_obj_set_style_text_color(hint, COL_TEXT_DIM, 0);
    lv_obj_set_style_pad_top(hint, 8, 0);
}

// -----------------------------------------------------------------------------
// Library helpers
// -----------------------------------------------------------------------------
// Egy list-gomb szöveg-label gyermekének megkeresése.
static lv_obj_t *list_btn_label(lv_obj_t *btn)
{
    uint32_t n = lv_obj_get_child_count(btn);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(btn, i);
        if (lv_obj_check_type(c, &lv_label_class)) return c;
    }
    return NULL;
}

static void browser_apply_cursor(void)
{
    if (!U.lib_list) return;
    uint32_t n = lv_obj_get_child_count(U.lib_list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *btn = lv_obj_get_child(U.lib_list, i);
        bool is_cursor = ((int)i == U.br_cursor);
        lv_obj_t *lbl = list_btn_label(btn);
        if (is_cursor) {
            lv_obj_set_style_bg_color(btn, COL_ACCENT, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_text_color(btn, lv_color_hex(0x06141A), LV_PART_MAIN);
            // Csak a kijelölt sor neve gördül, ha hosszú.
            if (lbl) lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        } else {
            lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_text_color(btn, COL_TEXT, LV_PART_MAIN);
            // A többi "..."-tal levágva, nem gördül.
            if (lbl) lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        }
    }
    if (U.br_cursor >= 0 && (uint32_t)U.br_cursor < n) {
        lv_obj_scroll_to_view(lv_obj_get_child(U.lib_list, U.br_cursor), LV_ANIM_ON);
    }
}

static void browser_rebuild_list(void)
{
    if (!U.lib_list) return;
    lv_obj_clean(U.lib_list);
    if (!U.br_entries || U.br_count == 0) {
        lv_obj_t *empty = lv_list_add_text(U.lib_list, "(empty)");
        lv_obj_set_style_text_color(empty, COL_TEXT_DIM, 0);
        return;
    }
    for (int i = 0; i < U.br_count; i++) {
        // Mappa = folder ikon, fájl = audio ikon
        const char *icon = U.br_entries[i].is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_AUDIO;
        lv_obj_t *btn = lv_list_add_button(U.lib_list, icon, U.br_entries[i].name);
        (void)btn;
    }
    browser_apply_cursor();
}

static void update_mini_playlist(void)
{
    if (!U.np_mini_list) return;
    lv_obj_clean(U.np_mini_list);
    if (!U.lib_tracks || U.lib_count == 0) {
        lv_obj_t *empty = lv_label_create(U.np_mini_list);
        lv_label_set_text(empty, "(no tracks)");
        lv_obj_set_style_text_color(empty, COL_TEXT_DIM, 0);
        return;
    }

    int start = U.lib_current - 1;
    if (start < 0) start = 0;
    int end = start + 3;
    if (end > U.lib_count) end = U.lib_count;

    for (int i = start; i < end; i++) {
        bool is_playing = (i == U.lib_current);

        lv_obj_t *row = lv_obj_create(U.np_mini_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_ver(row, 3, 0);
        lv_obj_set_style_pad_hor(row, 8, 0);
        if (is_playing) {
            // A currently-playing track: 3px bal oldali accent stripe + tompa
            // háttércsík. Az accent stripe biztosítja a WCAG 1.4.11 3:1
            // non-text kontrasztot a panel ellenében (11:1), a háttér csík
            // csak finom hangsúly. Nincs PLAY ikon, hogy ne legyen
            // összetéveszthető a state ikonnal a panel fölött.
            lv_obj_set_style_bg_color(row, COL_BG_PANEL_2, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(row, 4, 0);
            lv_obj_set_style_border_color(row, COL_ACCENT, 0);
            lv_obj_set_style_border_width(row, 3, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
        }

        lv_obj_t *l = lv_label_create(row);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_text(l, U.lib_tracks[i].name);
        lv_obj_set_style_text_color(l, is_playing ? COL_ACCENT : COL_TEXT, 0);
    }
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------
void ui_show_screen(ui_screen_t s)
{
    if (s >= UI_SCREEN_COUNT) return;
    lvgl_port_lock(0);
    U.current = s;
    lv_screen_load(U.scr[s]);
    update_screen_chip();
    lvgl_port_unlock();
}

void ui_next_screen(void)
{
    ui_screen_t n = (ui_screen_t)((U.current + 1) % UI_SCREEN_COUNT);
    ui_show_screen(n);
}

ui_screen_t ui_current_screen(void)
{
    return U.current;
}

void ui_browser_show(const char *path, const dir_entry_t *entries,
                     int count, int cursor)
{
    lvgl_port_lock(0);
    U.br_entries = entries;
    U.br_count   = count;
    U.br_cursor  = cursor;
    if (U.lib_path) {
        // A /sdcard prefixet elhagyjuk, hogy rövidebb legyen; gyökérnél "/"
        const char *disp = path;
        size_t mlen = strlen(SD_MOUNT_POINT);
        if (strncmp(path, SD_MOUNT_POINT, mlen) == 0) {
            disp = path + mlen;
            if (disp[0] == 0) disp = "/";
        }
        lv_label_set_text(U.lib_path, disp);
    }
    browser_rebuild_list();
    lvgl_port_unlock();
}

void ui_browser_set_cursor(int cursor)
{
    lvgl_port_lock(0);
    U.br_cursor = cursor;
    browser_apply_cursor();
    lvgl_port_unlock();
}

void ui_set_track_count(int count)
{
    lvgl_port_lock(0);
    if (U.set_val_tracks) {
        lv_label_set_text_fmt(U.set_val_tracks, "%d", count);
    }
    lvgl_port_unlock();
}

// --- A korábbi API ugyanazokkal a signature-ökkel -------------------------

void ui_show_track(const track_t *tr)
{
    lvgl_port_lock(0);
    if (U.np_lbl_title) {
        // A meglévő hívók a track_t-ben album és name mezőket adnak.
        // Az új layoutban külön sorba kerül az artist/album.
        lv_label_set_text(U.np_lbl_title, tr->name);
        if (U.np_lbl_subtitle) {
            lv_label_set_text(U.np_lbl_subtitle, tr->album[0] ? tr->album : "");
        }
    }

    // Album art keresése: cover.jpg / folder.jpg / azonos nevű .jpg
    char art[MAX_PATH_LEN];
    extern bool sd_find_album_art(const char *mp3_path, char *out_path, int out_path_len);
    if (sd_find_album_art(tr->path, art, sizeof(art))) {
        snprintf(s_cover_path, sizeof(s_cover_path), "S:%s",
                 art + strlen(SD_MOUNT_POINT));
        lv_image_set_src(U.np_img_cover, s_cover_path);
        lv_obj_remove_flag(U.np_img_cover, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(U.np_img_cover, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

void ui_show_no_track(void)
{
    lvgl_port_lock(0);
    if (U.np_lbl_title)    lv_label_set_text(U.np_lbl_title, "...");
    if (U.np_lbl_subtitle) lv_label_set_text(U.np_lbl_subtitle, "Válassz a Library-ből");
    if (U.np_img_cover)    lv_obj_add_flag(U.np_img_cover, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void ui_set_state(audio_state_t st)
{
    lvgl_port_lock(0);
    if (U.np_lbl_state) {
        const char *sym;
        switch (st) {
            case AUDIO_STATE_PLAYING: sym = LV_SYMBOL_PLAY;  break;
            case AUDIO_STATE_PAUSED:  sym = LV_SYMBOL_PAUSE; break;
            default:                  sym = LV_SYMBOL_STOP;  break;  // STOPPED, FINISHED
        }
        lv_label_set_text(U.np_lbl_state, sym);
    }
    lvgl_port_unlock();
}

bool ui_user_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
    if (s_disp_off && s_panel) {
        lvgl_port_lock(0);
        esp_lcd_panel_disp_on_off(s_panel, true);
        s_disp_off = false;
        lvgl_port_unlock();
        return true;     // ezzel a hívással ébresztettünk
    }
    return false;
}

void ui_idle_check(void)
{
    if (s_disp_off || !s_panel) return;
    int64_t elapsed_ms = (esp_timer_get_time() - s_last_activity_us) / 1000;
    if (elapsed_ms >= UI_IDLE_TIMEOUT_MS) {
        lvgl_port_lock(0);
        esp_lcd_panel_disp_on_off(s_panel, false);
        s_disp_off = true;
        lvgl_port_unlock();
    }
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
    if (U.np_bar_progress) lv_bar_set_value(U.np_bar_progress, pct, LV_ANIM_OFF);
    if (U.np_lbl_time) {
        char a[16], b[16], line[40];
        fmt_mmss(pos_ms, a, sizeof(a));
        fmt_mmss(dur_ms, b, sizeof(b));
        snprintf(line, sizeof(line), "%s / %s", a, b);
        lv_label_set_text(U.np_lbl_time, line);
    }
    // Periodikusan frissítjük a heap kijelzőt is a settings képernyőn.
    if (U.current == UI_SCREEN_SETTINGS) settings_refresh_heap();
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
    if (U.np_lbl_volume)  lv_label_set_text(U.np_lbl_volume, s);
    if (U.set_val_volume) lv_label_set_text_fmt(U.set_val_volume, "%u%%", vol);
    lvgl_port_unlock();
}

void ui_set_battery(uint16_t mv, uint8_t percent)
{
    lvgl_port_lock(0);
    const char *sym = (percent > 80) ? LV_SYMBOL_BATTERY_FULL :
                      (percent > 55) ? LV_SYMBOL_BATTERY_3 :
                      (percent > 30) ? LV_SYMBOL_BATTERY_2 :
                      (percent > 10) ? LV_SYMBOL_BATTERY_1 :
                                       LV_SYMBOL_BATTERY_EMPTY;
    if (U.ovr_battery) {
        lv_label_set_text_fmt(U.ovr_battery, "%s %u%%", sym, percent);
    }
    if (U.set_val_battery) {
        lv_label_set_text_fmt(U.set_val_battery, "%u%% (%u mV)", percent, mv);
    }
    lvgl_port_unlock();
}

void ui_set_playlist(const track_t *tracks, int count, int current_idx)
{
    // Ez most CSAK a Now Playing mini-listát (épp játszott album) frissíti.
    // A Library képernyő külön böngésző (ui_browser_show).
    lvgl_port_lock(0);
    U.lib_tracks  = tracks;
    U.lib_count   = count;
    U.lib_current = current_idx;
    update_mini_playlist();
    lvgl_port_unlock();
}

void ui_spi_lock(void)   { lvgl_port_lock(0); }
void ui_spi_unlock(void) { lvgl_port_unlock(); }

// -----------------------------------------------------------------------------
// Internal: settings heap frissítés (csak SETTINGS képernyőn érdekes)
// -----------------------------------------------------------------------------
static void settings_refresh_heap(void)
{
    if (!U.set_val_heap) return;
    size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    lv_label_set_text_fmt(U.set_val_heap, "%lu / %lu k",
                          (unsigned long)(internal / 1024),
                          (unsigned long)(psram / 1024));
}
