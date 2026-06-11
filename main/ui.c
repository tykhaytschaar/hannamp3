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
#include "esp_lcd_st7796.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch_ft5x06.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "app_config.h"
#include "ui.h"
#include "player.h"   // player_do_action / player_play_index (touch transport)
#include "mp3_fonts.h"
#include "boot_splash.h"
#include "usb_msc.h"  // usb_msc_request_reboot (Settings → USB Storage gomb)

static const char *TAG = "ui";

// -----------------------------------------------------------------------------
// Theme tokens (sötét/cyan design, 480×320-ra hangolva)
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

// Idle / energy management — N másodperc után a panel DISPOFF, user-eseményre vissza.
// A timeout dinamikus, a Settings képernyőről állítható.
//   értékek (másodperc): 10, 15, 30, 0=never
static int                    s_idle_timeout_s = 30;
static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_touch_handle_t s_touch = NULL;   // FT6336 kapacitív touch (polling)
static int64_t                s_last_activity_us = 0;
// Boot: a kijelző logikailag "off" (a backlight duty 0), így a player_start
// mentett-fényerő visszaállítása csak tárol — a tényleges felkapcsolás az
// ui_display_ready-ben történik, a tartalom betöltése után.
static bool                   s_disp_off = true;

// Boot splash állapota. A splash külön (a valódi képernyők felett) screen-en
// fut; az utolsó frame kint marad, amíg az ui_display_ready le nem cseréli a
// kész UI-ra és fel nem szabadítja ezeket.
static lv_obj_t       *s_splash_scr = NULL;   // splash képernyő
static uint8_t        *s_splash_buf = NULL;   // RGB888 dekód-buffer (PSRAM)
static lv_image_dsc_t  s_splash_dsc;          // a frame-et LVGL-nek leíró dsc
// Amíg a splash aktív (a lejátszástól a kész UI-ra váltásig), a háttér-
// világítás FIX 100% — a beállított fényerő csak tárolódik (s_bl_percent), és
// az ui_display_ready alkalmazza, amikor a kész UI kikerül. Így a kimerevített
// utolsó frame nem halványul el, ha a mentett fényerő < 100%.
static bool            s_splash_active = false;
// Frame-tempó cél: a tényleges sebesség max(dekód+flush, ennyi). 33 ms ≈ 30 fps
// felső korlát; a dekód jellemzően ennél lassabb, így ez ritkán fékez.
#define BOOT_SPLASH_FRAME_MS  33

// Háttérvilágítás PWM (LEDC). A GPIO 16-ot duty-val hajtjuk: 0 = sötét,
// max = teljes fényerő. A beállított fényerő százalékban (boot: 100%);
// idle/boot alatt a tényleges duty 0, de s_bl_percent megmarad.
#define BL_LEDC_MODE      LEDC_LOW_SPEED_MODE   // ESP32-S3: csak low-speed
#define BL_LEDC_TIMER     LEDC_TIMER_0
#define BL_LEDC_CHANNEL   LEDC_CHANNEL_0
#define BL_LEDC_RES       LEDC_TIMER_10_BIT     // 1024 lépés — bőven elég
#define BL_LEDC_RES_BITS  10
#define BL_LEDC_FREQ_HZ   20000                 // 20 kHz: villódzás- és zajmentes
#define BL_PCT_DEFAULT    100
static uint8_t s_bl_percent = BL_PCT_DEFAULT;

static void backlight_apply(void);

// Settings képernyő kurzor — csak vizuális (a legutóbb érintett sor), touch óta
// nincs edit-mód.
static int  s_set_cursor  = 0;

// Sleep enable — a player_task használja a deep sleep döntéshez.
// Default false: a felhasználó kapcsolja be a Settings-ből.
static bool s_sleep_enabled = false;

typedef struct {
    // Screens
    lv_obj_t *scr[UI_SCREEN_COUNT];
    ui_screen_t current;

    // Persistent overlay (battery + screen chip rendered on lv_layer_top)
    lv_obj_t *ovr_screen_chip;
    lv_obj_t *ovr_battery;
    lv_obj_t *ovr_volume;   // hangerő chip a battery előtt (minden képernyőn)
    lv_obj_t *ovr_lock;     // lakat ikon (closed=lock / open=unlocked)

    // Now Playing widgets
    lv_obj_t *np_img_cover;
    lv_obj_t *np_lbl_title;
    lv_obj_t *np_lbl_subtitle;
    lv_obj_t *np_lbl_time;
    lv_obj_t *np_bar_progress;
    lv_obj_t *np_btn_pp_lbl;   // play/pause transport gomb ikon-labelje
    lv_obj_t *np_mini_list;

    // Library (fájlböngésző) widgetek
    lv_obj_t *lib_list;
    lv_obj_t *lib_path;   // aktuális könyvtár fejléc

    // Settings widgets — value labelek
    lv_obj_t *set_val_volume;
    lv_obj_t *set_val_backlight;
    lv_obj_t *set_val_battery;
    lv_obj_t *set_val_idle;
    lv_obj_t *set_val_sleep;
    lv_obj_t *set_sld_volume;     // Settings volume slider
    lv_obj_t *set_sld_backlight;  // Settings brightness slider
    // Settings widgets — szerkeszthető sorok (kurzor + edit highlight ide kerül)
    lv_obj_t *set_row[UI_SETTING_COUNT];   // [VOLUME], [IDLE_TIMEOUT]

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
static void ui_touch_init(void);
static void screen_gesture_cb(lv_event_t *e);
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
static void idle_label_refresh_locked(void);
static void settings_render_cursor_locked(void);

// -----------------------------------------------------------------------------
// LCD bring-up + LVGL port — VÁLTOZATLAN a korábbi kódhoz képest
// -----------------------------------------------------------------------------
void ui_init(void)
{
    ESP_LOGI(TAG, "ST7796 + LVGL init");

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
        // Ez a ST7796 modul BGR sorrendben várja a színt — RGB-vel a cyan
        // sárgászöldként jött (R↔B csere). EMPIRIKUS KNOB #3.
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(io_handle, &dev_cfg, &panel));
    esp_lcd_panel_reset(panel);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_lcd_panel_init(panel);
    // EMPIRIKUS KNOB #1: szín-inverzió. IPS ST7796-on általában true kell, de
    // panelfüggő — ha negatív/fordított a kép, állítsd false-ra.
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_disp_on_off(panel, true);
    s_panel = panel;                                  // idle/wake-hez
    s_last_activity_us = esp_timer_get_time();

    // Háttérvilágítás PWM (LEDC) a GPIO 16-on. Boot alatt duty 0 (OFF) marad —
    // hogy a panel power-on fehér flash-e és az adat-nélküli placeholder fázis
    // ne látsszon — az ui_display_ready() rámpázza a beállított fényerőre.
    ledc_timer_config_t bl_timer = {
        .speed_mode      = BL_LEDC_MODE,
        .timer_num       = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_RES,
        .freq_hz         = BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_timer));
    ledc_channel_config_t bl_ch = {
        .gpio_num   = PIN_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = 0,            // boot: OFF
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_ch));

    // ---- LVGL port ----
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    // 16 KB: a 8 KB kevés volt — a Now Playing teljes-albumos track-listájának
    // görgetése mély render/layout hívásláncot csinál → stack overflow a
    // taskLVGL-ben. 16 KB bőven fedi.
    lvgl_cfg.task_stack = 16384;
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
        // ST7796 fekvő (swap_xy). 180°-ra forgatva: mindkét mirror flag false.
        // (BGR-fix a dev_cfg-ben.) EMPIRIKUS KNOB #2.
        .rotation = { .swap_xy = true, .mirror_x = false, .mirror_y = false },
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

    ui_touch_init();
}

// -----------------------------------------------------------------------------
// Touch bring-up (0. fázis) — FT6336 kapacitív, külön I2C busz, polling.
// Egyelőre csak az LVGL pointer-indev-et regisztrálja + debug-logolja a
// press-koordinátákat. A widget-ekre kötött viselkedés az 1. fázis.
// -----------------------------------------------------------------------------
#define TOUCH_I2C_PORT   I2C_NUM_0
#define TOUCH_I2C_HZ     400000

// Ellenőrző press-log (transzformált, LVGL-koordináta). Bal-felső ~(0,0),
// jobb-alsó ~(480,320). Az 1. fázis (widget-viselkedés) után törölhető.
static void touch_log_event_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    ESP_LOGI(TAG, "touch press @ (%d, %d)", (int)p.x, (int)p.y);
}

static void ui_touch_init(void)
{
    ESP_LOGI(TAG, "FT6336 touch init (I2C %d, SDA %d, SCL %d, RST %d, polling)",
             TOUCH_I2C_PORT, PIN_TOUCH_SDA, PIN_TOUCH_SCL, PIN_TOUCH_RST);

    // ---- I2C master busz (új driver) ----
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = PIN_TOUCH_SDA,
        .scl_io_num = PIN_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   // ha a modulon nincs külső pull-up
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    // ---- esp_lcd panel IO az FT5x06/FT6336 fölé ----
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_io_cfg.scl_speed_hz = TOUCH_I2C_HZ;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(bus, &tp_io_cfg, &tp_io));

    // ---- FT5x06/FT6336 touch driver ----
    // x_max/y_max a panel natív felbontása (320×480 portré, a mirror ezzel
    // számol). Tájolás a kijelzőhöz igazítva (panelen mérve):
    //   swap_xy=1  — a nyers x (függőleges) az LVGL-Y-ba
    //   mirror_x=1 — a függőleges fordított volt (fent 320 → 0)
    //   mirror_y=0 — a vízszintes jó
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = 320,
        .y_max = 480,
        .rst_gpio_num = PIN_TOUCH_RST,
        .int_gpio_num = GPIO_NUM_NC,            // polling — nincs INT láb
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 1, .mirror_x = 1, .mirror_y = 0 },
    };
    if (esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &s_touch) != ESP_OK) {
        ESP_LOGE(TAG, "touch init failed — ellenőrizd a bekötést / pull-upokat");
        return;
    }

    // ---- LVGL pointer indev ----
    lvgl_port_lock(0);
    const lvgl_port_touch_cfg_t touch_lv_cfg = {
        .disp   = s_disp,
        .handle = s_touch,
    };
    lvgl_port_add_touch(&touch_lv_cfg);

    for (int i = 0; i < UI_SCREEN_COUNT; i++) {
        lv_obj_add_event_cb(U.scr[i], touch_log_event_cb, LV_EVENT_PRESSED, NULL);
        // Swipe gesture: bal/jobb húzás váltogatja a képernyőket (a MENU
        // gomb short press-ét helyettesíti). A gesture esemény a screen-en
        // sül el, akkor is ha a touch egy belső widget (pl. lib_list)
        // területén kezdődött — LVGL a domináns irányt nézi.
        lv_obj_add_event_cb(U.scr[i], screen_gesture_cb, LV_EVENT_GESTURE, NULL);
    }
    lvgl_port_unlock();

    ESP_LOGI(TAG, "touch ready — bal-felső ~(0,0), jobb-alsó ~(480,320). Swipe: ←/→ képernyőváltás.");
}

// -----------------------------------------------------------------------------
// Swipe gesture → képernyőváltás (a MENU gomb short press-ét helyettesíti)
// LV_DIR_LEFT  : ujj balra húzott → következő képernyő
// LV_DIR_RIGHT : ujj jobbra húzott → előző képernyő
// Fel/le gesture ignorálva — azt a lib_list scroll használja.
// -----------------------------------------------------------------------------
static void screen_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    // Ne váltsunk képernyőt, ha a gesztus egy slider húzásából (Settings) vagy
    // egy lista görgetéséből származik — különben a vízszintes slider-drag
    // átugrana az előző/következő oldalra.
    lv_obj_t *act = lv_indev_get_active_obj();
    if (act && lv_obj_check_type(act, &lv_slider_class)) return;
    if (lv_indev_get_scroll_obj(indev)) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
        if (dir == LV_DIR_LEFT) ui_next_screen();
        else                    ui_prev_screen();
        // A touch hátralévő részét (a felengedésig) eldobjuk, különben az ujj
        // felengedése már az ÚJ képernyő egy során click-et generálna → swipe
        // után véletlen track-kiválasztás.
        lv_indev_wait_release(indev);
    }
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
    // explicit font van beállítva (title 24, subtitle 18).
    lv_obj_set_style_text_font(scr, &mp3_inter_14, LV_PART_MAIN);
    // A screen ne scrollozzon — különben a vízszintes touch swipe-ot a
    // scroll-engine fogja el, és nem érne el a LV_EVENT_GESTURE-höz.
    // A lib_list és más belső scrollozható widgetek ettől függetlenek.
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
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

    // Default font az overlay layerre — az itt létrehozott labelek (battery,
    // screen chip, lock indikátor stb.) NEM öröklik a screen text fontját,
    // így itt is explicit be kell állítani. Különben az LVGL beépített
    // lv_font_montserrat_14-et használnák, amiben nincsenek a saját
    // ikonjaink (MP3_SYMBOL_LOCK / MP3_SYMBOL_UNLOCK) — négyzet jelenne meg.
    lv_obj_set_style_text_font(top, &mp3_inter_14, LV_PART_MAIN);

    U.ovr_screen_chip = lv_label_create(top);
    lv_obj_set_style_text_color(U.ovr_screen_chip, COL_ACCENT, LV_PART_MAIN);
    lv_label_set_text(U.ovr_screen_chip, "NOW PLAYING");
    lv_obj_align(U.ovr_screen_chip, LV_ALIGN_TOP_LEFT, 8, 6);

    U.ovr_battery = lv_label_create(top);
    lv_obj_set_style_text_color(U.ovr_battery, COL_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text(U.ovr_battery, LV_SYMBOL_BATTERY_FULL " ---");
    lv_obj_align(U.ovr_battery, LV_ALIGN_TOP_RIGHT, -8, 6);

    // Hangerő chip a battery előtt (balra). align_to dinamikusan a battery
    // bal széléhez igazítja, így a battery szövegszélességtől függetlenül jó.
    U.ovr_volume = lv_label_create(top);
    lv_obj_set_style_text_color(U.ovr_volume, COL_TEXT, LV_PART_MAIN);
    lv_label_set_text(U.ovr_volume, LV_SYMBOL_VOLUME_MAX " ---");
    lv_obj_align_to(U.ovr_volume, U.ovr_battery, LV_ALIGN_OUT_LEFT_MID, -14, 0);

    // Lakat ikon a center-top-on. Default: open () — io_init beolvassa
    // a switch valódi állapotát és frissíti, ha kell.
    U.ovr_lock = lv_label_create(top);
    lv_obj_set_style_text_color(U.ovr_lock, COL_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text(U.ovr_lock, "\xEF\x82\x9C");   // 0xF09C — lock-open
    lv_obj_align(U.ovr_lock, LV_ALIGN_TOP_MID, 0, 6);
}

static void update_screen_chip(void)
{
    static const char *labels[UI_SCREEN_COUNT] = { "NOW PLAYING", "LIBRARY", "SETTINGS" };
    if (!U.ovr_screen_chip) return;
    lv_label_set_text(U.ovr_screen_chip, labels[U.current]);
}

// Transport gomb tap → a megfelelő player akció (user_data = player_action_t).
static void np_transport_click(lv_event_t *e)
{
    int action = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    player_do_action((player_action_t)action);
}

// Egy 64×48 transport gomb létrehozása ikon-labellel. Visszaadja a labelt
// (a play/pause gombnál ezt eltároljuk, hogy az ikon state szerint váltson).
static lv_obj_t *transport_btn(lv_obj_t *parent, const char *icon,
                               player_action_t action, bool accent)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 64, 48);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, accent ? COL_ACCENT : COL_BG_PANEL_2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_user_data(btn, (void *)(intptr_t)action);
    lv_obj_add_event_cb(btn, np_transport_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, &mp3_inter_18, 0);
    lv_obj_set_style_text_color(lbl, accent ? COL_BG : COL_TEXT, 0);
    lv_label_set_text(lbl, icon);
    lv_obj_center(lbl);
    return lbl;
}

// -----------------------------------------------------------------------------
// Screen 1: Now Playing
// Layout 480×320:
//   y   0..28    : overlay header (screen chip + lock + battery)
//   y  32..192   : cover 160×160 (bal) + info blokk (jobb, x=184, w=284)
//   y 204..308   : mini playlist panel (4 sor)
// -----------------------------------------------------------------------------
static void build_now_playing(void)
{
    lv_obj_t *scr = U.scr[UI_SCREEN_NOW_PLAYING];

    // Cover (bal oldal) — 160×160 rounded panel, JPG fallback hely
    U.np_img_cover = lv_image_create(scr);
    lv_obj_set_size(U.np_img_cover, 160, 160);
    lv_obj_align(U.np_img_cover, LV_ALIGN_TOP_LEFT, 12, 32);
    lv_obj_set_style_bg_color(U.np_img_cover, COL_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(U.np_img_cover, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(U.np_img_cover, 8, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(U.np_img_cover, true, LV_PART_MAIN);

    // Info blokk (jobb oldal): title, subtitle, progress, time, state + volume
    // Egységes baseline x=184, szélesség 284 (480 - 184 - 12)
    U.np_lbl_title = lv_label_create(scr);
    lv_label_set_long_mode(U.np_lbl_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(U.np_lbl_title, 284);
    lv_obj_align(U.np_lbl_title, LV_ALIGN_TOP_LEFT, 184, 36);
    lv_obj_set_style_text_font(U.np_lbl_title, &mp3_inter_24, 0);
    lv_obj_set_style_text_color(U.np_lbl_title, COL_TEXT, 0);
    lv_label_set_text(U.np_lbl_title, "—");

    U.np_lbl_subtitle = lv_label_create(scr);
    lv_label_set_long_mode(U.np_lbl_subtitle, LV_LABEL_LONG_DOT);
    lv_obj_set_width(U.np_lbl_subtitle, 284);
    lv_obj_align(U.np_lbl_subtitle, LV_ALIGN_TOP_LEFT, 184, 74);
    lv_obj_set_style_text_font(U.np_lbl_subtitle, &mp3_inter_18, 0);
    lv_obj_set_style_text_color(U.np_lbl_subtitle, COL_TEXT_DIM, 0);
    lv_label_set_text(U.np_lbl_subtitle, "");

    U.np_bar_progress = lv_bar_create(scr);
    lv_obj_set_size(U.np_bar_progress, 284, 8);
    lv_obj_align(U.np_bar_progress, LV_ALIGN_TOP_LEFT, 184, 104);
    lv_bar_set_range(U.np_bar_progress, 0, 1000);
    lv_bar_set_value(U.np_bar_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(U.np_bar_progress, COL_BG_PANEL_2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(U.np_bar_progress, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(U.np_bar_progress, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(U.np_bar_progress, 4, LV_PART_INDICATOR);

    U.np_lbl_time = lv_label_create(scr);
    lv_obj_align(U.np_lbl_time, LV_ALIGN_TOP_LEFT, 184, 116);
    lv_obj_set_style_text_color(U.np_lbl_time, COL_TEXT_DIM, 0);
    lv_label_set_text(U.np_lbl_time, "0:00 / 0:00");

    // Transport sor: prev / play-pause / stop / next (4 × 64×48), y=140..188.
    // A play/pause kitöltött accent; a többi tompa panel-bg. A volume + state
    // ikon a headerbe / a play-pause gombra került (np_lbl_state megszűnt).
    lv_obj_t *trow = lv_obj_create(scr);
    lv_obj_remove_style_all(trow);
    lv_obj_set_size(trow, 284, 48);
    lv_obj_align(trow, LV_ALIGN_TOP_LEFT, 184, 140);
    lv_obj_set_flex_flow(trow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(trow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    transport_btn(trow, LV_SYMBOL_PREV, PLAYER_ACTION_PREV, false);
    U.np_btn_pp_lbl = transport_btn(trow, LV_SYMBOL_PLAY, PLAYER_ACTION_PLAY_PAUSE, true);
    transport_btn(trow, LV_SYMBOL_STOP, PLAYER_ACTION_STOP, false);
    transport_btn(trow, LV_SYMBOL_NEXT, PLAYER_ACTION_NEXT, false);

    // Mini playlist (alul, 4 sor)
    // Track lista — scrollozható lv_list az album ÖSSZES számával (tap = play).
    U.np_mini_list = lv_list_create(scr);
    lv_obj_set_size(U.np_mini_list, 456, 116);
    lv_obj_align(U.np_mini_list, LV_ALIGN_TOP_LEFT, 12, 196);
    lv_obj_set_style_bg_color(U.np_mini_list, COL_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_border_width(U.np_mini_list, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(U.np_mini_list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(U.np_mini_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(U.np_mini_list, 2, LV_PART_MAIN);
    lv_obj_set_style_text_font(U.np_mini_list, &mp3_inter_14, LV_PART_MAIN);
}

// -----------------------------------------------------------------------------
// Screen 2: Library — full-screen scrollozó lista
// -----------------------------------------------------------------------------
static void build_library(void)
{
    lv_obj_t *scr = U.scr[UI_SCREEN_LIBRARY];

    // Aktuális könyvtár fejléc (a "LIB" chip alatt) — 480×320-on bővebb sáv
    U.lib_path = lv_label_create(scr);
    lv_label_set_long_mode(U.lib_path, LV_LABEL_LONG_DOT);
    lv_obj_set_width(U.lib_path, 456);
    lv_obj_align(U.lib_path, LV_ALIGN_TOP_LEFT, 12, 30);
    lv_obj_set_style_text_color(U.lib_path, COL_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text(U.lib_path, "/");

    U.lib_list = lv_list_create(scr);
    lv_obj_set_size(U.lib_list, 456, 254);
    lv_obj_align(U.lib_list, LV_ALIGN_TOP_LEFT, 12, 56);
    lv_obj_set_style_bg_color(U.lib_list, COL_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_border_width(U.lib_list, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(U.lib_list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(U.lib_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(U.lib_list, 2, LV_PART_MAIN);
}

// -----------------------------------------------------------------------------
// Screen 3: Settings — read-only status panel
// -----------------------------------------------------------------------------
// Volume slider: drag közben realtime (NEM perzisztál — a mid-drag flash-írás
// megakasztaná az audiót), elengedéskor NVS-mentés.
static void sld_volume_cb(lv_event_t *e)
{
    lv_obj_t *sld = lv_event_get_target_obj(e);
    int v = lv_slider_get_value(sld);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        audio_set_volume((uint8_t)v);
        ui_set_volume((uint8_t)v);             // header chip + value label
        s_set_cursor = UI_SETTING_VOLUME;
        settings_render_cursor_locked();
    } else {  // LV_EVENT_RELEASED
        player_set_volume((uint8_t)v);         // perzisztens
    }
}

// Brightness slider: drag közben realtime fényerő, elengedéskor NVS-mentés.
static void sld_backlight_cb(lv_event_t *e)
{
    lv_obj_t *sld = lv_event_get_target_obj(e);
    int v = lv_slider_get_value(sld);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        ui_set_backlight((uint8_t)v);
        s_set_cursor = UI_SETTING_BACKLIGHT;
        settings_render_cursor_locked();
    } else {  // LV_EVENT_RELEASED
        player_set_backlight((uint8_t)v);      // perzisztens
    }
}

// Settings sor sliderrel: bal label (fix szélesség, hogy a sliderek igazodjanak)
// + flex-grow slider + jobb érték-label.
static lv_obj_t *settings_slider_row(lv_obj_t *parent, const char *icon,
                                     const char *label, int32_t init_val,
                                     lv_event_cb_t cb,
                                     lv_obj_t **out_slider, lv_obj_t **out_value)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(row, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 10, LV_PART_MAIN);

    lv_obj_t *left = lv_label_create(row);
    char buf[64];
    if (icon && *icon) snprintf(buf, sizeof(buf), "%s  %s", icon, label);
    else               snprintf(buf, sizeof(buf), "%s", label);
    lv_label_set_text(left, buf);
    lv_obj_set_width(left, 120);              // fix → a sliderek egy vonalban
    lv_obj_set_style_text_color(left, COL_TEXT_DIM, 0);

    lv_obj_t *sld = lv_slider_create(row);
    lv_obj_set_flex_grow(sld, 1);
    lv_slider_set_range(sld, 0, 100);
    lv_slider_set_value(sld, init_val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sld, COL_BG_PANEL_2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sld, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sld, COL_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(sld, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sld, cb, LV_EVENT_RELEASED, NULL);
    if (out_slider) *out_slider = sld;

    lv_obj_t *val = lv_label_create(row);
    lv_obj_set_width(val, 50);
    lv_label_set_text_fmt(val, "%d%%", (int)init_val);
    lv_obj_set_style_text_color(val, COL_TEXT, 0);
    if (out_value) *out_value = val;
    return row;
}

// "Display off" sor tap → timeout léptetés (player.c menti NVS-be). Alvó
// kijelzőn csak ébreszt. (Nincs lista-rebuild, így nem kell lv_async_call.)
static void display_off_row_click(lv_event_t *e)
{
    (void)e;
    if (ui_user_activity()) return;
    s_set_cursor = UI_SETTING_IDLE_TIMEOUT;
    player_cycle_idle_timeout();
    settings_render_cursor_locked();   // event-ctx: a lock már a miénk
}

// "Sleep" sor tap → on/off váltás (player.c menti NVS-be). Alvón csak ébreszt.
static void sleep_row_click(lv_event_t *e)
{
    (void)e;
    if (ui_user_activity()) return;
    s_set_cursor = UI_SETTING_SLEEP;
    player_toggle_sleep();
    settings_render_cursor_locked();
}

// USB Storage sor tap → újraindulás USB MSC módba. Alvó kijelzőn csak ébreszt
// (mint a többi akció-sor), különben usb_msc_request_reboot() — ami beállítja a
// boot-flaget és azonnal újraindít (nem tér vissza).
static void usb_storage_row_click(lv_event_t *e)
{
    (void)e;
    if (ui_user_activity()) return;
    usb_msc_request_reboot();   // sosem tér vissza
}

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
    if (icon && *icon) snprintf(buf, sizeof(buf), "%s  %s", icon, label);
    else               snprintf(buf, sizeof(buf), "%s", label);
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
    lv_obj_set_size(panel, 456, 276);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 12, 32);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, 6, LV_PART_MAIN);

    // Sorrend: fent a nem-szerkeszthetők, alul a szerkeszthetők (kurzor csak
    // az utóbbiakon mozog). Egy blokk, vizuális szeparátor nélkül.

    // "Info" sor — nincs value oldal, csak a szöveg balra.
    {
        lv_obj_t *info = lv_obj_create(panel);
        lv_obj_remove_style_all(info);
        lv_obj_set_size(info, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_ver(info, 2, LV_PART_MAIN);
        lv_obj_t *lbl = lv_label_create(info);
        lv_label_set_text(lbl, "HANNAMP3 2026");
        lv_obj_set_style_text_color(lbl, COL_TEXT_DIM, 0);
    }

    settings_row(panel, LV_SYMBOL_BATTERY_FULL, "Battery", &U.set_val_battery);

    // Kezdőérték placeholder (70) — a player_start ui_set_volume(saved)-del
    // szinkronizálja. FONTOS: itt NEM hívunk audio_get_volume()-ot, mert az
    // s_status_mux-ot venné, amit csak a később futó audio_init hoz létre.
    U.set_row[UI_SETTING_VOLUME] =
        settings_slider_row(panel, LV_SYMBOL_VOLUME_MAX, "Volume", 70,
                            sld_volume_cb, &U.set_sld_volume, &U.set_val_volume);
    U.set_row[UI_SETTING_BACKLIGHT] =
        settings_slider_row(panel, NULL, "Brightness", s_bl_percent,
                            sld_backlight_cb, &U.set_sld_backlight, &U.set_val_backlight);
    U.set_row[UI_SETTING_IDLE_TIMEOUT] =
        settings_row(panel, NULL, "Display off", &U.set_val_idle);
    lv_obj_add_flag(U.set_row[UI_SETTING_IDLE_TIMEOUT], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.set_row[UI_SETTING_IDLE_TIMEOUT], display_off_row_click,
                        LV_EVENT_CLICKED, NULL);
    U.set_row[UI_SETTING_SLEEP] =
        settings_row(panel, NULL, "Sleep", &U.set_val_sleep);
    lv_obj_add_flag(U.set_row[UI_SETTING_SLEEP], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.set_row[UI_SETTING_SLEEP], sleep_row_click,
                        LV_EVENT_CLICKED, NULL);

    // USB Storage akció-sor: koppintásra újraindul USB MSC módba (az SD kártya
    // külső meghajtóként a natív USB-n). Nem cursor-target (nincs az enum-ban).
    {
        lv_obj_t *usb_val = NULL;
        lv_obj_t *usb_row = settings_row(panel, NULL, "USB Storage", &usb_val);
        if (usb_val) lv_label_set_text(usb_val, "Tap");
        lv_obj_add_flag(usb_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(usb_row, usb_storage_row_click, LV_EVENT_CLICKED, NULL);
    }
    if (U.set_val_idle)  idle_label_refresh_locked();   // induló érték kiírása
    if (U.set_val_sleep) lv_label_set_text(U.set_val_sleep, s_sleep_enabled ? "On" : "Off");

    settings_render_cursor_locked();   // induló kurzor a Volume-on
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

// Library: nem-gyökér mappában egy UI-only ".." parent sor kerül a lista
// elejére (child 0). Ilyenkor az s_bentries[i] a (i+1). gyerek. Az offset:
static bool s_lib_show_parent = false;

static void browser_apply_cursor(void)
{
    if (!U.lib_list) return;
    int off = s_lib_show_parent ? 1 : 0;
    uint32_t n = lv_obj_get_child_count(U.lib_list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *btn = lv_obj_get_child(U.lib_list, i);
        lv_obj_t *lbl = list_btn_label(btn);
        bool is_parent = (off && i == 0);
        bool is_cursor = (!is_parent && (int)i - off == U.br_cursor);
        if (is_cursor) {
            lv_obj_set_style_bg_color(btn, COL_ACCENT, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_text_color(btn, lv_color_hex(0x06141A), LV_PART_MAIN);
            // Csak a kijelölt sor neve gördül, ha hosszú.
            if (lbl) lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        } else if (is_parent) {
            // Parent ".." sor: accent szöveg, átlátszó háttér (nem cursor-target).
            lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_text_color(btn, COL_ACCENT, LV_PART_MAIN);
            if (lbl) lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        } else {
            lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_text_color(btn, COL_TEXT, LV_PART_MAIN);
            // A többi "..."-tal levágva, nem gördül.
            if (lbl) lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        }
    }
    int cur_child = U.br_cursor + off;
    if (U.br_cursor >= 0 && cur_child < (int)n) {
        lv_obj_scroll_to_view(lv_obj_get_child(U.lib_list, cur_child), LV_ANIM_ON);
    }
}

// Library sor tap: parent (user_data -1) → fel egy szint; egyébként az adott
// entry aktiválása (mappa → belép; fájl → album-load + play + Now Playing).
// Mint a track listánál: a művelet újraépíti a lib_list-et (lv_obj_clean),
// ezért lv_async_call-lal halasztjuk az esemény-feldolgozáson kívülre.
static void lib_act_async(void *p)
{
    int idx = (int)(intptr_t)p;
    if (idx < 0) player_browser_up();
    else         player_browser_tap(idx);
}
static void lib_row_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    lv_async_call(lib_act_async, (void *)(intptr_t)idx);
}

static void browser_rebuild_list(void)
{
    if (!U.lib_list) return;
    lv_obj_clean(U.lib_list);

    // Parent ".." sor, ha nem gyökérben vagyunk (UI-only, user_data = -1).
    if (s_lib_show_parent) {
        lv_obj_t *pbtn = lv_list_add_button(U.lib_list, LV_SYMBOL_DIRECTORY, "..");
        lv_obj_set_style_border_width(pbtn, 0, LV_PART_MAIN);
        lv_obj_set_style_text_color(pbtn, COL_ACCENT, LV_PART_MAIN);
        lv_obj_set_user_data(pbtn, (void *)(intptr_t)-1);
        lv_obj_add_event_cb(pbtn, lib_row_click, LV_EVENT_CLICKED, NULL);
    }

    if (!U.br_entries || U.br_count == 0) {
        lv_obj_t *empty = lv_list_add_text(U.lib_list, "(empty)");
        lv_obj_set_style_text_color(empty, COL_TEXT_DIM, 0);
        return;
    }
    for (int i = 0; i < U.br_count; i++) {
        // Mappa = folder ikon, fájl = audio ikon
        const char *icon = U.br_entries[i].is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_AUDIO;
        lv_obj_t *btn = lv_list_add_button(U.lib_list, icon, U.br_entries[i].name);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);   // divider-csík ki
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, lib_row_click, LV_EVENT_CLICKED, NULL);
    }
    browser_apply_cursor();
}

// Track lista sor tap → az adott album-index lejátszása. A tényleges műveletet
// lv_async_call-lal HALASZTJUK: a player_play_index újraépíti a listát
// (lv_obj_clean), ami a click-esemény forrás-gombját is törli — ezt tilos a
// saját eseménye közben tenni (use-after-free / fagyás). Async → a következő
// LVGL tickben fut, már az esemény-feldolgozáson kívül.
static void np_play_async(void *p)
{
    player_play_index((int)(intptr_t)p);
}
static void np_tlist_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    lv_async_call(np_play_async, (void *)(intptr_t)idx);
}

static void update_mini_playlist(void)
{
    if (!U.np_mini_list) return;
    lv_obj_clean(U.np_mini_list);
    if (!U.lib_tracks || U.lib_count == 0) {
        lv_obj_t *empty = lv_list_add_text(U.np_mini_list, "(no tracks)");
        lv_obj_set_style_text_color(empty, COL_TEXT_DIM, 0);
        return;
    }

    // ÖSSZES track (nem ablakos) — a lista scrollozható. A currently-playing
    // sor: 3px bal accent stripe + tompa bg, és scroll-to-view rá.
    lv_obj_t *cur_row = NULL;
    for (int i = 0; i < U.lib_count; i++) {
        bool is_playing = (i == U.lib_current);
        lv_obj_t *btn = lv_list_add_button(U.np_mini_list, LV_SYMBOL_AUDIO,
                                           U.lib_tracks[i].name);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, np_tlist_click, LV_EVENT_CLICKED, NULL);
        // Az lv_list gombok alapból alsó keretet (divider) kapnak a témától —
        // sötét háttéren fehér csíkként látszik. Kikapcsoljuk; a játszott sor
        // bal oldali accent-csíkját külön rakjuk vissza.
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
        if (is_playing) {
            lv_obj_set_style_bg_color(btn, COL_BG_PANEL_2, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(btn, 4, LV_PART_MAIN);
            lv_obj_set_style_border_color(btn, COL_ACCENT, LV_PART_MAIN);
            lv_obj_set_style_border_width(btn, 3, LV_PART_MAIN);
            lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
            lv_obj_set_style_text_color(btn, COL_ACCENT, LV_PART_MAIN);
            cur_row = btn;
        } else {
            lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_text_color(btn, COL_TEXT, LV_PART_MAIN);
        }
    }
    if (cur_row) lv_obj_scroll_to_view(cur_row, LV_ANIM_OFF);
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

void ui_prev_screen(void)
{
    ui_screen_t p = (ui_screen_t)((U.current + UI_SCREEN_COUNT - 1) % UI_SCREEN_COUNT);
    ui_show_screen(p);
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
    // Parent ".." sor csak ha nem a gyökérben vagyunk (pontos egyezés kell).
    s_lib_show_parent = (strcmp(path, SD_MOUNT_POINT) != 0);
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


// --- A korábbi API ugyanazokkal a signature-ökkel -------------------------

void ui_show_track(const track_t *tr)
{
    lvgl_port_lock(0);
    if (U.np_lbl_title) {
        // Title: ID3 TIT2 preferált, fallback filename.
        const char *title = tr->title[0] ? tr->title : tr->name;
        lv_label_set_text(U.np_lbl_title, title);

        if (U.np_lbl_subtitle) {
            // Subtitle: "Artist · Album" ha mindkettő van; egyik ha csak az;
            // mappa-név fallback ha ID3 nincs.
            char line[256];
            if (tr->artist[0] && tr->album[0]) {
                snprintf(line, sizeof(line), "%s \xC2\xB7 %s", tr->artist, tr->album);
            } else if (tr->artist[0]) {
                snprintf(line, sizeof(line), "%s", tr->artist);
            } else {
                snprintf(line, sizeof(line), "%s", tr->album[0] ? tr->album : "");
            }
            lv_label_set_text(U.np_lbl_subtitle, line);
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
    // A play/pause transport gomb az AKCIÓT mutatja: ha játszik → PAUSE ikon
    // (tap = pause), egyébként → PLAY ikon (tap = play).
    if (U.np_btn_pp_lbl) {
        lv_label_set_text(U.np_btn_pp_lbl,
                          (st == AUDIO_STATE_PLAYING) ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    lvgl_port_unlock();
}

// A LEDC duty beállítása az aktuális állapot szerint: ha a kijelző idle/boot
// miatt le van kapcsolva (s_disp_off), a duty 0; különben a beállított %.
static void backlight_apply(void)
{
    uint32_t max = (1u << BL_LEDC_RES_BITS) - 1u;
    // Splash alatt fix 100% — a mentett fényerő csak az ui_display_ready-ben lép
    // életbe, hogy a kimerevített utolsó frame ne halványuljon el.
    uint32_t pct  = s_splash_active ? 100u : s_bl_percent;
    uint32_t duty = s_disp_off ? 0u : (max * pct) / 100u;
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

// -----------------------------------------------------------------------------
// Boot splash — a firmware-be ágyazott JPEG frame-szett lejátszása cold boot-kor.
// Az ui_init UTÁN, de a player_start (SD-szkennelés) ELŐTT hívandó: a frame-ek
// flash-ből jönnek, így NINCS SD-olvasás a kirajzolással egy időben (a közös
// SPI buszon nem ütköznek). A frame-eket az esp_new_jpeg dekódolja RGB888-ba,
// majd egy teljes képernyős lv_image rajzolja ki — a szín/tájolás az LVGL +
// panel pipeline-on garantáltan helyes (ugyanaz az út, mint az album-artnál).
//
// Az utolsó frame a képernyőn marad (a háttérvilágítás bekapcsolva), amíg az
// ui_display_ready le nem cseréli a kész UI-ra. A háttérvilágítást az első
// sikeresen kirajzolt frame után kapcsoljuk fel — így a panel power-on fehér
// flash-e és az üres fázis rejtve marad, ahogy eddig is.
// -----------------------------------------------------------------------------
void ui_play_boot_splash(void)
{
    int n = boot_splash_frame_count();
    if (n <= 0 || !s_disp) return;

    const int buflen = BOOT_SPLASH_W * BOOT_SPLASH_H * 3;   // RGB888
    // 16-byte igazítás az esp_new_jpeg kimenetéhez; PSRAM-ból (460 KB).
    s_splash_buf = heap_caps_aligned_alloc(16, buflen, MALLOC_CAP_SPIRAM);
    if (!s_splash_buf) {
        ESP_LOGW(TAG, "boot splash: %d B buffer alloc failed — kihagyva", buflen);
        return;
    }

    // Splash alatt fix 100% háttérvilágítás (lásd backlight_apply); a mentett
    // fényerőt a player_start csak eltárolja, az ui_display_ready alkalmazza.
    s_splash_active = true;

    memset(&s_splash_dsc, 0, sizeof(s_splash_dsc));
    s_splash_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_splash_dsc.header.cf     = LV_COLOR_FORMAT_RGB888;
    s_splash_dsc.header.w      = BOOT_SPLASH_W;
    s_splash_dsc.header.h      = BOOT_SPLASH_H;
    s_splash_dsc.header.stride = BOOT_SPLASH_W * 3;
    s_splash_dsc.data          = s_splash_buf;
    s_splash_dsc.data_size     = buflen;

    lvgl_port_lock(0);
    // Az overlay (battery/volume/lock) a lv_layer_top()-on van, ami MINDEN
    // screen fölé rajzol — a splash idejére elrejtjük, az ui_display_ready
    // hozza vissza.
    lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);

    s_splash_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_splash_scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_splash_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_splash_scr, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_splash_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *img = lv_image_create(s_splash_scr);
    lv_obj_set_pos(img, 0, 0);
    lv_image_set_src(img, &s_splash_dsc);
    lv_screen_load(s_splash_scr);
    lvgl_port_unlock();

    bool bl_on = false;
    for (int i = 0; i < n; i++) {
        int64_t t0 = esp_timer_get_time();
        if (!boot_splash_decode_rgb888(i, s_splash_buf, buflen)) continue;

        lvgl_port_lock(0);
        // A dsc data-ja ugyanaz a buffer, csak a tartalma változott — a cache-t
        // eldobjuk, hogy a friss frame jelenjen meg, és kikényszerítjük a redraw-t.
        lv_image_cache_drop(&s_splash_dsc);
        lv_obj_invalidate(img);
        lv_refr_now(s_disp);
        lvgl_port_unlock();

        if (!bl_on) {
            // Első frame kint van — DMA settle, majd háttérvilágítás fel.
            vTaskDelay(pdMS_TO_TICKS(40));
            s_disp_off = false;
            backlight_apply();
            bl_on = true;
        }

        int wait = BOOT_SPLASH_FRAME_MS - (int)((esp_timer_get_time() - t0) / 1000);
        if (wait > 0) vTaskDelay(pdMS_TO_TICKS(wait));
    }
    ESP_LOGI(TAG, "boot splash done (%d frames)", n);
    // Az utolsó frame kint marad; a takarítást az ui_display_ready végzi.
}

void ui_display_ready(void)
{
    bool had_splash = (s_splash_scr != NULL);

    // Splash után: a valódi UI-t a player_start már feltöltötte (a háttér-
    // screen-ek widgetjeibe). Lecseréljük a splash-t a kész képernyőre, és
    // visszahozzuk az overlay-t. A háttérvilágítás ilyenkor még a splash
    // fix 100%-án ég — az utolsó frame → kész UI váltás után kapcsolunk a
    // beállított fényerőre.
    if (had_splash && lvgl_port_lock(200)) {
        lv_obj_remove_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
        lv_screen_load(U.scr[U.current]);
        lvgl_port_unlock();
    }

    // Kész UI kirajzoltatása + flush bevárása. (A teljes 480×320 flush ~31 ms
    // @ 80 MHz.) Így a fehér flash / üres placeholder fázis rejtve marad.
    if (s_disp && lvgl_port_lock(200)) {
        lv_refr_now(s_disp);
        lvgl_port_unlock();
    }

    if (had_splash) {
        // A kész UI kint van — most lép életbe a beállított fényerő (a splash
        // és a kimerevített utolsó frame végig fix 100%-on volt).
        s_splash_active = false;
        backlight_apply();
    } else {
        // Hidegindítás splash nélkül: a backlight eddig OFF volt — flush DMA
        // bevárása, majd felkapcsolás a beállított fényerőre.
        vTaskDelay(pdMS_TO_TICKS(50));
        s_disp_off = false;
        backlight_apply();
    }
    s_last_activity_us = esp_timer_get_time();   // idle-számláló nullázása

    // Splash erőforrások felszabadítása — a kész UI már kint van. A cache-ből
    // is dobjuk a dsc-t, mielőtt a buffert felszabadítjuk (különben dangling).
    if (s_splash_scr) {
        lvgl_port_lock(0);
        lv_image_cache_drop(&s_splash_dsc);
        lv_obj_delete(s_splash_scr);
        s_splash_scr = NULL;
        lvgl_port_unlock();
    }
    if (s_splash_buf) {
        heap_caps_free(s_splash_buf);
        s_splash_buf = NULL;
    }
}

// -----------------------------------------------------------------------------
// USB MSC mód kijelző — egy statikus "USB Storage Mode" képernyő. A custom
// fontok szimbólum-tartománya nem tartalmaz USB-ikont (0xF287), ezért csak
// szöveg. Az overlay-t (battery/volume/lock) elrejtjük, ahogy a splash-nél.
//
// Az "Exit" gomb CSAK vizuális: MSC alatt az LVGL fagyasztva van (nem futnak
// az indev/click eventek), ezért az érintést az usb_msc_run pollozza
// közvetlenül a touch driverből (ui_usb_exit_touched — az FT6336 külön I2C
// buszon van, nem érinti a közös SPI-t). A gomb képernyő-koordinátáit itt
// tároljuk el a hit-teszthez.
// -----------------------------------------------------------------------------
static lv_area_t s_usb_exit_area;          // Exit gomb képernyő-koordinátái
static bool      s_usb_exit_valid = false;

void ui_show_usb_mode_screen(bool have_card)
{
    lvgl_port_lock(0);
    lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *scr = lv_obj_create(NULL);
    apply_screen_bg(scr);   // sötét háttér + no-scroll + default font

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, &mp3_inter_24, 0);
    lv_obj_set_style_text_color(title, COL_ACCENT, 0);
    lv_label_set_text(title, "USB Storage Mode");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -78);

    lv_obj_t *sub = lv_label_create(scr);
    lv_obj_set_style_text_font(sub, &mp3_inter_14, 0);
    lv_obj_set_style_text_color(sub, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(sub, have_card
        ? "Eject the drive on your computer before exit."
        : "No SD card.");
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, -34);

    // Exit gomb (vizuális — lásd a fenti blokk-kommentet)
    lv_obj_t *btn = lv_obj_create(scr);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 180, 56);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 56);
    lv_obj_set_style_bg_color(btn, COL_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_t *bl = lv_label_create(btn);
    lv_obj_set_style_text_font(bl, &mp3_inter_18, 0);
    lv_obj_set_style_text_color(bl, COL_BG, 0);
    lv_label_set_text(bl, "Exit");
    lv_obj_center(bl);

    lv_screen_load(scr);
    // Layout kiszámoltatása, hogy a gomb végleges koordinátáit eltehessük a
    // touch hit-teszthez; +12 px kényelmi margó a tap-célnak.
    lv_obj_update_layout(scr);
    lv_obj_get_coords(btn, &s_usb_exit_area);
    lv_area_increase(&s_usb_exit_area, 12, 12);
    s_usb_exit_valid = true;
    lvgl_port_unlock();
}

// MSC alatt hívható (LVGL fagyasztva): közvetlenül a touch drivert olvassa
// (I2C — a közös SPI buszt nem érinti), és igazat ad, ha az aktuális érintés
// az Exit gomb területére esik. Az esp_lcd_touch a configban megadott
// swap/mirror flagekkel már LVGL-koordinátát ad vissza.
bool ui_usb_exit_touched(void)
{
    if (!s_touch || !s_usb_exit_valid) return false;
    esp_lcd_touch_read_data(s_touch);
    uint16_t x, y;
    uint8_t cnt = 0;
    if (!esp_lcd_touch_get_coordinates(s_touch, &x, &y, NULL, &cnt, 1) || cnt == 0)
        return false;
    return x >= s_usb_exit_area.x1 && x <= s_usb_exit_area.x2 &&
           y >= s_usb_exit_area.y1 && y <= s_usb_exit_area.y2;
}

// LVGL befagyasztása az MSC mód idejére: a port-lockot megszerezzük és NEM
// engedjük el. A taskLVGL ettől kezdve blokkol a lv_timer_handler előtt
// (esp_lvgl_port: lvgl_port_lock(0) = portMAX_DELAY), így nem flushel a közös
// SPI buszra, amíg az MSC az SD-t használja. A lock-tulajdonos a hívó task
// (app_main), amit az usb_msc_run életben tart.
void ui_suspend_for_msc(void)
{
    lvgl_port_lock(0);
    // szándékosan nincs unlock
}

void ui_set_backlight(uint8_t pct)
{
    if (pct > 100) pct = 100;
    lvgl_port_lock(0);
    s_bl_percent = pct;
    if (!s_disp_off) backlight_apply();   // ha alszik a kijelző, csak tároljuk
    if (U.set_val_backlight) lv_label_set_text_fmt(U.set_val_backlight, "%u%%", pct);
    if (U.set_sld_backlight) lv_slider_set_value(U.set_sld_backlight, pct, LV_ANIM_OFF);
    lvgl_port_unlock();
}

uint8_t ui_get_backlight(void)
{
    return s_bl_percent;
}

bool ui_user_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
    if (s_disp_off && s_panel) {
        lvgl_port_lock(0);
        esp_lcd_panel_disp_on_off(s_panel, true);
        s_disp_off = false;
        backlight_apply();           // vissza a beállított fényerőre
        lvgl_port_unlock();
        return true;     // ezzel a hívással ébresztettünk
    }
    return false;
}

void ui_force_wake_pending(void)
{
    // Csak a flag-et állítjuk; a kijelző fizikailag már bekapcsolt az
    // ui_init-ben, az első user-event ui_user_activity-ja no-op-ként
    // beletalál (DISPON/BL újra HIGH, érdektelen), és true-t ad → a
    // player_handle_button korai return-be megy.
    s_disp_off = true;
}

void ui_idle_check(void)
{
    if (s_disp_off || !s_panel) return;
    if (s_idle_timeout_s <= 0) return;        // never
    // A gomb/CLI aktivitás (s_last_activity_us) ÉS a touch közül a frissebb
    // számít — a touchot az LVGL tartja nyilván (lv_display_get_inactive_time),
    // így az érintés (slider, tap, scroll) is ébren tartja a kijelzőt.
    int64_t elapsed_ms = (esp_timer_get_time() - s_last_activity_us) / 1000;
    if (s_disp) {
        int64_t touch_ms = (int64_t)lv_display_get_inactive_time(s_disp);
        if (touch_ms < elapsed_ms) elapsed_ms = touch_ms;
    }
    if (elapsed_ms >= (int64_t)s_idle_timeout_s * 1000) {
        lvgl_port_lock(0);
        esp_lcd_panel_disp_on_off(s_panel, false);
        s_disp_off = true;
        backlight_apply();           // duty 0 — háttérvilágítás le
        lvgl_port_unlock();
    }
}

// Belső: a Settings sor szöveg-frissítése (LVGL lockon belül hívható).
static void idle_label_refresh_locked(void)
{
    if (!U.set_val_idle) return;
    char buf[16];
    if (s_idle_timeout_s <= 0) snprintf(buf, sizeof(buf), "Never");
    else                       snprintf(buf, sizeof(buf), "%d s", s_idle_timeout_s);
    lv_label_set_text(U.set_val_idle, buf);
}

void ui_set_idle_timeout_s(int seconds)
{
    s_idle_timeout_s = seconds;
    s_last_activity_us = esp_timer_get_time();   // ne triggereljen azonnali off-ot
    lvgl_port_lock(0);
    idle_label_refresh_locked();
    lvgl_port_unlock();
}

int ui_get_idle_timeout_s(void)
{
    return s_idle_timeout_s;
}

// Vol+/Vol- edit módban: dir > 0 → következő érték (10→15→30→Never), <= 0 → előző.
int ui_cycle_idle_timeout(int dir)
{
    int next;
    if (dir > 0) {
        switch (s_idle_timeout_s) {
            case 10: next = 15; break;
            case 15: next = 30; break;
            case 30: next = 0;  break;
            default: next = 10; break;     // 0 (Never) → 10
        }
    } else {
        switch (s_idle_timeout_s) {
            case 10: next = 0;  break;     // 10 → Never
            case 15: next = 10; break;
            case 30: next = 15; break;
            default: next = 30; break;     // 0 (Never) → 30
        }
    }
    ui_set_idle_timeout_s(next);
    return next;
}

// -----------------------------------------------------------------------------
// Settings kurzor + edit rendering
// -----------------------------------------------------------------------------
// Egy sor három állapotban lehet:
//   - sima:   nincs háttér, nincs border, pad_left 12 (állandó szöveg-pozíció)
//   - kurzor: 3 px accent bal-csík + pad_left 9 (3 border + 9 = 12 belül,
//             így a szöveg ugyanott marad mint kurzor nélkül; a csík és a
//             szöveg közt ~9 px térköz)
//   - edit:   teljes COL_ACCENT háttér, fekete szöveg — maximális kontraszt
// Touch-átállás óta nincs edit-mód: a kurzor csak vizuális (a legutóbb
// érintett soron egy 3px bal accent-csík). Nincs gyerek-átszínezés, így a
// slider sorokra (3 gyerek) is biztonságos.
static void settings_render_cursor_locked(void)
{
    for (int i = 0; i < UI_SETTING_COUNT; i++) {
        lv_obj_t *row = U.set_row[i];
        if (!row) continue;
        if (i == s_set_cursor) {
            lv_obj_set_style_border_color(row, COL_ACCENT, 0);
            lv_obj_set_style_border_width(row, 3, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_radius(row, 4, 0);
            lv_obj_set_style_pad_left(row, 9, 0);   // 3 + 9 = 12 a tartalomig
        } else {
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_pad_left(row, 12, 0);
        }
    }
}

void ui_set_sleep_enabled(bool enabled)
{
    s_sleep_enabled = enabled;
    lvgl_port_lock(0);
    if (U.set_val_sleep) lv_label_set_text(U.set_val_sleep, enabled ? "On" : "Off");
    lvgl_port_unlock();
}

bool ui_get_sleep_enabled(void) { return s_sleep_enabled; }

bool ui_toggle_sleep_enabled(void)
{
    ui_set_sleep_enabled(!s_sleep_enabled);
    return s_sleep_enabled;
}

void ui_set_locked(bool locked)
{
    lvgl_port_lock(0);
    if (U.ovr_lock) {
        // 0xF023 = lock (closed) ; 0xF09C = lock-open
        lv_label_set_text(U.ovr_lock, locked ? "\xEF\x80\xA3" : "\xEF\x82\x9C");
        lv_obj_set_style_text_color(U.ovr_lock,
                                    locked ? COL_ACCENT : COL_TEXT_DIM, LV_PART_MAIN);
    }
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
    if (U.np_bar_progress) lv_bar_set_value(U.np_bar_progress, pct, LV_ANIM_OFF);
    if (U.np_lbl_time) {
        char a[16], b[16], line[40];
        fmt_mmss(pos_ms, a, sizeof(a));
        fmt_mmss(dur_ms, b, sizeof(b));
        snprintf(line, sizeof(line), "%s / %s", a, b);
        lv_label_set_text(U.np_lbl_time, line);
    }
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
    if (U.ovr_volume)     lv_label_set_text(U.ovr_volume, s);   // header chip
    if (U.set_val_volume) lv_label_set_text_fmt(U.set_val_volume, "%u%%", vol);
    if (U.set_sld_volume) lv_slider_set_value(U.set_sld_volume, vol, LV_ANIM_OFF);
    // A header chip pozíciója a battery szélességéhez igazodik (változhat a %).
    if (U.ovr_volume && U.ovr_battery)
        lv_obj_align_to(U.ovr_volume, U.ovr_battery, LV_ALIGN_OUT_LEFT_MID, -14, 0);
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

