#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   // xTaskCreatePinnedToCoreWithCaps (PSRAM stack)
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "app_config.h"
#include "gb.h"
#include "io.h"
#include "ui.h"
#include "mp3_fonts.h"

// Peanut-GB: single-header emulátor, az implementáció EBBE a fordítási
// egységbe kerül (máshol nem includolható implementációval).
// HIGH_LCD_ACCURACY=0: a soronkénti sprite-rendezés kihagyása — mérhetően
// gyorsabb render, az ára ritka sprite-prioritási pontatlanság (10+ sprite
// egy sorban). A nehéz (GB Studio) játékok valós idejűségéhez kell.
#define ENABLE_LCD   1
#define ENABLE_SOUND 0
#define PEANUT_GB_HIGH_LCD_ACCURACY 0
#include "peanut_gb.h"

static const char *TAG = "gb";

// Theme tokenek — az ui.c-vel egyező értékek.
#define COL_BG          lv_color_hex(0x0E1116)
#define COL_BG_PANEL    lv_color_hex(0x161B22)
#define COL_BG_PANEL_2  lv_color_hex(0x1F2630)
#define COL_TEXT        lv_color_hex(0xE6EDF3)
#define COL_TEXT_DIM    lv_color_hex(0x8B98A5)
#define COL_ACCENT      lv_color_hex(0x2EE6D6)

// -----------------------------------------------------------------------------
// Layout 480×320: bal oldalsáv (Exit + cím) | 320×288 játéktér | jobb
// oldalsáv (B / Start / Select touch-gombok — a fizikai gombok bekötéséig
// és mellett is használhatók).
// -----------------------------------------------------------------------------
#define GB_SCALE   2
#define GB_OUT_W   (LCD_WIDTH * GB_SCALE)    // 320
#define GB_OUT_H   (LCD_HEIGHT * GB_SCALE)   // 288
#define GB_OUT_X   80
#define GB_OUT_Y   ((LCD_V_RES - GB_OUT_H) / 2)
#define GB_PANEL_W 80

// Klasszikus DMG "pea green" paletta RGB565-ben, shade 0 = legvilágosabb.
// (A flush swap_bytes flagje intézi a byte-sorrendet.)
static const uint16_t GB_PAL[4] = { 0x9DE1, 0x8D61, 0x3306, 0x09C1 };

// 59,73 Hz — egy GB-frame valós ideje mikroszekundumban.
#define GB_FRAME_US  16742
// ROM-plafon: MBC5-ig elvileg 8 MB, de a PSRAM-keret miatt 4 MB-ot engedünk
// (a valódi GB-játékok gyakorlatilag mind beleférnek).
#define GB_ROM_MAX   (4 * 1024 * 1024)
// CLI-injektált gomb-tap hossza frame-ben (~130 ms).
#define GB_INJECT_FRAMES 8

static struct {
    bool             active;
    volatile bool    exit_req;     // kilépés-kérés (bármely kontextusból)
    volatile bool    task_done;    // az emulációs task leállt, jöhet a lebontás
    struct gb_s     *gb;           // emulátor-állapot (PSRAM, ~17 KB)
    uint8_t         *rom;          // teljes ROM (PSRAM)
    uint8_t         *cart_ram;     // cart RAM, ha a játéknak van (PSRAM)
    uint8_t         *fb[2];        // 320×288 RGB565 dupla buffer (PSRAM)
    volatile int     front;        // a kész, megjeleníthető buffer indexe
    int              back;         // amibe a scanline-ok íródnak
    volatile bool    frame_ready;
    lv_obj_t        *scr;
    lv_obj_t        *img;
    lv_image_dsc_t   dsc;
    lv_timer_t      *timer;        // LVGL-oldali present-timer
    gbmode_exit_cb_t on_exit;
    volatile uint8_t touch_mask;   // touch-gombok nyomva-tartott JOYPAD bitjei
    volatile uint8_t inject[8];    // CLI-tap: JOYPAD bitenként hátralévő frame-ek
} G;

// -----------------------------------------------------------------------------
// Peanut-GB callbackek (az emulációs task kontextusában futnak)
// -----------------------------------------------------------------------------
static uint8_t cb_rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;
    return G.rom[addr];
}

static uint8_t cb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;
    return G.cart_ram ? G.cart_ram[addr] : 0xFF;
}

static void cb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr,
                              const uint8_t val)
{
    (void)gb;
    if (G.cart_ram) G.cart_ram[addr] = val;
}

static void cb_error(struct gb_s *gb, const enum gb_error_e err,
                     const uint16_t addr)
{
    (void)gb;
    ESP_LOGE(TAG, "emulátor-hiba %d @0x%04X — kilépés", (int)err, addr);
    G.exit_req = true;
}

// Egy GB scanline → két 320 px-es sor a back-bufferben (2× nearest).
static void cb_lcd_line(struct gb_s *gb, const uint8_t *pixels,
                        const uint_fast8_t line)
{
    (void)gb;
    uint16_t *dst = (uint16_t *)G.fb[G.back] + (size_t)line * 2 * GB_OUT_W;
    for (int x = 0; x < LCD_WIDTH; x++) {
        uint16_t c = GB_PAL[pixels[x] & 3];
        dst[2 * x]     = c;
        dst[2 * x + 1] = c;
    }
    memcpy(dst + GB_OUT_W, dst, GB_OUT_W * sizeof(uint16_t));
}

// -----------------------------------------------------------------------------
// Input
// -----------------------------------------------------------------------------
// JOYPAD bit ← btn_event_t. 0, ha az esemény nem game-gomb.
static uint8_t evt_to_joypad(btn_event_t evt)
{
    switch (evt) {
    case BTN_EVT_A:     return JOYPAD_A;
    case BTN_EVT_B:     return JOYPAD_B;
    case BTN_EVT_X:     return JOYPAD_START;    // X = Start
    case BTN_EVT_Y:     return JOYPAD_SELECT;   // Y = Select
    case BTN_EVT_UP:    return JOYPAD_UP;
    case BTN_EVT_DOWN:  return JOYPAD_DOWN;
    case BTN_EVT_LEFT:  return JOYPAD_LEFT;
    case BTN_EVT_RIGHT: return JOYPAD_RIGHT;
    default:            return 0;
    }
}

void gbmode_handle_button(btn_event_t evt)
{
    if (!G.active) return;
    uint8_t bit = evt_to_joypad(evt);
    for (int i = 0; i < 8; i++) {
        if (bit & (1u << i)) G.inject[i] = GB_INJECT_FRAMES;
    }
}

// Frame-enként: fizikai gombok + touch-gombok + CLI-injekt → joypad.
// A Peanut-GB joypad-regisztere aktív-LOW (törölt bit = lenyomva).
static void poll_input(void)
{
    uint8_t down = 0;
    if (io_button_down(BTN_EVT_A))     down |= JOYPAD_A;
    if (io_button_down(BTN_EVT_B))     down |= JOYPAD_B;
    if (io_button_down(BTN_EVT_X))     down |= JOYPAD_START;    // X = Start
    if (io_button_down(BTN_EVT_Y))     down |= JOYPAD_SELECT;   // Y = Select
    if (io_button_down(BTN_EVT_UP))    down |= JOYPAD_UP;
    if (io_button_down(BTN_EVT_DOWN))  down |= JOYPAD_DOWN;
    if (io_button_down(BTN_EVT_LEFT))  down |= JOYPAD_LEFT;
    if (io_button_down(BTN_EVT_RIGHT)) down |= JOYPAD_RIGHT;
    down |= G.touch_mask;
    for (int i = 0; i < 8; i++) {
        if (G.inject[i]) {
            down |= (1u << i);
            G.inject[i]--;
        }
    }
    G.gb->direct.joypad = (uint8_t)~down;
}

// -----------------------------------------------------------------------------
// Emulációs task (1-es mag, PSRAM-stack — flash-műveletet nem végez)
// -----------------------------------------------------------------------------
static void gb_task(void *arg)
{
    (void)arg;
    int64_t next = esp_timer_get_time();
    int64_t stat_t0 = next;
    int     stat_frames = 0;
    int64_t stat_emu_us = 0;
    while (!G.exit_req) {
        poll_input();
        int64_t t0 = esp_timer_get_time();
        gb_run_frame(G.gb);
        stat_emu_us += esp_timer_get_time() - t0;
        stat_frames++;

        // Kész frame: buffer-csere + jelzés a present-timernek. (Dupla
        // bufferrel az épp flusholódó frame-be elvétve beleírhatunk — az
        // esetleges apró tearing v1-ben vállalt kompromisszum.)
        G.front = G.back;
        G.back ^= 1;
        G.frame_ready = true;

        // Valós idejű 59,73 Hz pacing: ms-pontos vTaskDelay (1 kHz tick) +
        // a maradék finomvárás. Nagy csúszásnál az adósságot eldobjuk.
        // Lemaradásban is KÖTELEZŐ az 1 ms yield: enélkül a task sosem
        // adná át az 1-es magot, az IDLE1 kiéhezne → task-wdt spam.
        next += GB_FRAME_US;
        int64_t now = esp_timer_get_time();
        if (next - now < -50000) next = now;
        int64_t wait_ms = (next - now) / 1000;
        vTaskDelay(wait_ms > 0 ? pdMS_TO_TICKS(wait_ms) : 1);
        while (esp_timer_get_time() < next) { /* <1 ms finomvárás */ }

        // Másodpercenkénti teljesítmény-log: emulált fps + átlagos
        // frame-emulációs idő (>16742 us = nem megy valós időben).
        if (now - stat_t0 >= 1000000) {
            ESP_LOGI(TAG, "fps=%d emu=%d us/frame", stat_frames,
                     (int)(stat_emu_us / stat_frames));
            stat_t0 = now;
            stat_frames = 0;
            stat_emu_us = 0;
        }
    }
    G.task_done = true;
    vTaskDelete(NULL);
}

// -----------------------------------------------------------------------------
// LVGL-oldal: present + lebontás
// -----------------------------------------------------------------------------
static void free_buffers(void)
{
    heap_caps_free(G.fb[0]);    G.fb[0] = NULL;
    heap_caps_free(G.fb[1]);    G.fb[1] = NULL;
    heap_caps_free(G.rom);      G.rom = NULL;
    heap_caps_free(G.cart_ram); G.cart_ram = NULL;
    heap_caps_free(G.gb);       G.gb = NULL;
}

// Lebontás — KIZÁRÓLAG LVGL task kontextusból, és csak miután az emulációs
// task leállt (task_done): a bufferek felszabadítása futó emulátor mellett
// use-after-free lenne.
static void do_exit(void)
{
    if (!G.active) return;
    G.active = false;

    lvgl_port_lock(0);
    lv_timer_delete(G.timer);
    G.timer = NULL;
    lv_timer_set_period(lv_display_get_refr_timer(lv_display_get_default()),
                        CONFIG_LV_DEF_REFR_PERIOD);
    lv_obj_remove_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
    ui_show_screen(ui_current_screen());
    lv_image_cache_drop(&G.dsc);
    lv_obj_delete(G.scr);
    G.scr = NULL;
    lvgl_port_unlock();

    free_buffers();
    ui_set_idle_inhibit(false);

    ESP_LOGI(TAG, "GB exit");
    if (G.on_exit) G.on_exit();
}

static void present_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (G.task_done) {
        do_exit();
        return;
    }
    if (G.frame_ready) {
        G.frame_ready = false;
        G.dsc.data = G.fb[G.front];
        lv_image_cache_drop(&G.dsc);
        lv_obj_invalidate(G.img);
    }
}

// -----------------------------------------------------------------------------
// UI build
// -----------------------------------------------------------------------------
static void exit_btn_click(lv_event_t *e)
{
    (void)e;
    gbmode_request_exit();   // a lebontást a present-timer intézi (task_done)
}

// Touch-gomb (jobb oldalsáv): nyomva tartásig tartó joypad-bit.
static void touch_btn_event(lv_event_t *e)
{
    uint8_t bit = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED)
        G.touch_mask |= bit;
    else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST)
        G.touch_mask &= (uint8_t)~bit;
}

static lv_obj_t *side_button(lv_obj_t *parent, const char *text, int x, int y,
                             int w, int h, uint8_t joypad_bit)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, COL_BG_PANEL_2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, touch_btn_event, LV_EVENT_PRESSED,
                        (void *)(uintptr_t)joypad_bit);
    lv_obj_add_event_cb(btn, touch_btn_event, LV_EVENT_RELEASED,
                        (void *)(uintptr_t)joypad_bit);
    lv_obj_add_event_cb(btn, touch_btn_event, LV_EVENT_PRESS_LOST,
                        (void *)(uintptr_t)joypad_bit);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_color(lbl, COL_ACCENT, 0);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    return btn;
}

static void build_screen(const char *title)
{
    G.scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(G.scr, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(G.scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(G.scr, COL_TEXT, LV_PART_MAIN);
    lv_obj_set_style_pad_all(G.scr, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(G.scr, &mp3_inter_14, LV_PART_MAIN);
    lv_obj_remove_flag(G.scr, LV_OBJ_FLAG_SCROLLABLE);

    // Bal sáv: Exit + virtuális D-pad (IDEIGLENES, a fizikai gombok
    // bekötéséig — utána a sáv visszaegyszerűsödik Exit + címre).
    // Megkötés: az LVGL pointer-indev egy érintést követ → egyszerre egy
    // virtuális gomb nyomható (irány + A kombó nem megy touchról).
    lv_obj_t *left = lv_obj_create(G.scr);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, GB_PANEL_W, LCD_V_RES);
    lv_obj_set_pos(left, 0, 0);
    lv_obj_set_style_bg_color(left, COL_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(left, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *exit_btn = lv_button_create(left);
    lv_obj_set_size(exit_btn, GB_PANEL_W - 12, 48);
    lv_obj_set_pos(exit_btn, 6, 6);
    lv_obj_set_ext_click_area(exit_btn, 8);   // könnyebb eltalálni
    lv_obj_set_style_bg_color(exit_btn, COL_BG_PANEL_2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(exit_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(exit_btn, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(exit_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(exit_btn, exit_btn_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(exit_btn);
    lv_obj_set_style_text_color(xl, COL_ACCENT, 0);
    lv_label_set_text(xl, LV_SYMBOL_LEFT " Kilépés");
    lv_obj_center(xl);

    // D-pad: fel / (balra|jobbra) / le. ASCII feliratok — a saját fontok
    // szimbólum-lefedettsége a fel/le chevronokra nem garantált.
    side_button(left, "^", 6,  78, GB_PANEL_W - 12, 56, JOYPAD_UP);
    side_button(left, "<", 4,  140, 34, 56, JOYPAD_LEFT);
    side_button(left, ">", 42, 140, 34, 56, JOYPAD_RIGHT);
    side_button(left, "v", 6,  202, GB_PANEL_W - 12, 56, JOYPAD_DOWN);

    lv_obj_t *lbl = lv_label_create(left);
    lv_obj_set_width(lbl, GB_PANEL_W - 12);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl, title);
    lv_obj_set_pos(lbl, 6, 286);

    // Jobb sáv: A / B / Start / Select touch-gombok (szintén ideiglenes
    // teljes kiosztás — a fizikai gombok után A/B lekerülhet innen).
    lv_obj_t *right = lv_obj_create(G.scr);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, GB_PANEL_W, LCD_V_RES);
    lv_obj_set_pos(right, LCD_H_RES - GB_PANEL_W, 0);
    lv_obj_set_style_bg_color(right, COL_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(right, LV_OPA_COVER, LV_PART_MAIN);
    side_button(right, "A",      6, 8,   GB_PANEL_W - 12, 64, JOYPAD_A);
    side_button(right, "B",      6, 80,  GB_PANEL_W - 12, 64, JOYPAD_B);
    side_button(right, "Start",  6, 152, GB_PANEL_W - 12, 48, JOYPAD_START);
    side_button(right, "Select", 6, 208, GB_PANEL_W - 12, 48, JOYPAD_SELECT);

    // Játéktér
    G.img = lv_image_create(G.scr);
    lv_obj_set_pos(G.img, GB_OUT_X, GB_OUT_Y);
    lv_image_set_src(G.img, &G.dsc);
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------
bool gbmode_is_active(void)
{
    return G.active;
}

void gbmode_request_exit(void)
{
    if (G.active) G.exit_req = true;
}

// ROM beolvasása PSRAM-ba — 64 KB-os chunkokban, hogy az SPI-lock (flush-
// kizárás) ne egyben fogja a buszt egy nagy ROM-nál.
static uint8_t *load_rom(const char *path, size_t *out_size)
{
    ui_spi_lock();
    FILE *f = fopen(path, "rb");
    ui_spi_unlock();
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0x150 || size > GB_ROM_MAX) {   // 0x150 = GB header vége
        fclose(f);
        ESP_LOGW(TAG, "érvénytelen ROM-méret: %ld", size);
        return NULL;
    }

    uint8_t *rom = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
    if (!rom) {
        fclose(f);
        return NULL;
    }
    size_t got = 0;
    while (got < (size_t)size) {
        size_t chunk = (size_t)size - got;
        if (chunk > 65536) chunk = 65536;
        ui_spi_lock();
        size_t n = fread(rom + got, 1, chunk, f);
        ui_spi_unlock();
        if (n == 0) break;
        got += n;
    }
    fclose(f);
    if (got != (size_t)size) {
        heap_caps_free(rom);
        return NULL;
    }
    *out_size = (size_t)size;
    return rom;
}

bool gbmode_start(const char *rom_path, gbmode_exit_cb_t on_exit)
{
    if (G.active) return false;

    size_t rom_size = 0;
    G.rom = load_rom(rom_path, &rom_size);
    if (!G.rom) {
        ESP_LOGW(TAG, "ROM betöltés sikertelen: %s", rom_path);
        return false;
    }

    G.gb = heap_caps_calloc(1, sizeof(struct gb_s), MALLOC_CAP_SPIRAM);
    G.fb[0] = heap_caps_malloc(GB_OUT_W * GB_OUT_H * 2, MALLOC_CAP_SPIRAM);
    G.fb[1] = heap_caps_malloc(GB_OUT_W * GB_OUT_H * 2, MALLOC_CAP_SPIRAM);
    if (!G.gb || !G.fb[0] || !G.fb[1]) {
        ESP_LOGE(TAG, "buffer alloc failed");
        free_buffers();
        return false;
    }

    enum gb_init_error_e err = gb_init(G.gb, cb_rom_read, cb_cart_ram_read,
                                       cb_cart_ram_write, cb_error, NULL);
    if (err != GB_INIT_NO_ERROR) {
        ESP_LOGW(TAG, "gb_init: %d (nem támogatott/hibás ROM?)", (int)err);
        free_buffers();
        return false;
    }

    size_t save_size = 0;
    if (gb_get_save_size_s(G.gb, &save_size) != 0) save_size = 0;
    if (save_size > 0) {
        G.cart_ram = heap_caps_calloc(1, save_size, MALLOC_CAP_SPIRAM);
        if (!G.cart_ram) {
            free_buffers();
            return false;
        }
    }
    gb_init_lcd(G.gb, cb_lcd_line);

    // Üres (legvilágosabb shade) első frame mindkét bufferbe.
    for (int b = 0; b < 2; b++) {
        uint16_t *p = (uint16_t *)G.fb[b];
        for (int i = 0; i < GB_OUT_W * GB_OUT_H; i++) p[i] = GB_PAL[0];
    }

    memset(&G.dsc, 0, sizeof(G.dsc));
    G.dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    G.dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    G.dsc.header.w      = GB_OUT_W;
    G.dsc.header.h      = GB_OUT_H;
    G.dsc.header.stride = GB_OUT_W * 2;
    G.dsc.data          = G.fb[0];
    G.dsc.data_size     = GB_OUT_W * GB_OUT_H * 2;

    char name[48];
    const char *base = strrchr(rom_path, '/');
    strlcpy(name, base ? base + 1 : rom_path, sizeof(name));   // csonkolás OK
    char *dot = strrchr(name, '.');
    if (dot) *dot = 0;

    lvgl_port_lock(0);
    lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
    build_screen(name);
    lv_screen_load(G.scr);

    G.exit_req    = false;
    G.task_done   = false;
    G.frame_ready = false;
    G.front       = 0;
    G.back        = 1;
    G.touch_mask  = 0;
    memset((void *)G.inject, 0, sizeof(G.inject));
    G.on_exit     = on_exit;
    G.active      = true;
    // Present 33 ms-onként (~30 fps megjelenítés): a teljes 320×288-as
    // render+flush ~18 ms — 16 ms-os ütemben az LVGL task telítődne, és a
    // touch (Exit / virtuális gombok) válaszideje romlana. Az emuláció
    // ettől függetlenül valós idejű 60 fps a saját taskjában.
    G.timer = lv_timer_create(present_tick_cb, 33, NULL);
    lv_timer_set_period(lv_display_get_refr_timer(lv_display_get_default()), 16);
    lvgl_port_unlock();

    ui_set_idle_inhibit(true);

    // Emulációs task az 1-es magon, PSRAM-stackkel (a szűkös belső RAM-ot
    // nem terheli; flash-műveletet nem végez, így a PSRAM-stack engedett).
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        gb_task, "gb", 16384, NULL, 5, NULL, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "gb task create failed");
        G.task_done = true;   // a present-timer lebontja a felépített UI-t
        return false;
    }

    ESP_LOGI(TAG, "GB start: %s (%u KB ROM, %u B cart RAM)",
             rom_path, (unsigned)(rom_size / 1024), (unsigned)save_size);
    return true;
}
