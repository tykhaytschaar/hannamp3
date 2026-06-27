#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "app_config.h"
#include "chip8.h"
#include "game.h"
#include "gb.h"     // gbmode_is_active (a picker közös)
#include "io.h"
#include "sd.h"
#include "ui.h"
#include "player.h"
#include "mp3_fonts.h"

static const char *TAG = "game";

// Theme tokenek — az ui.c-vel egyező értékek (ott file-local makrók).
#define COL_BG          lv_color_hex(0x0E1116)
#define COL_BG_PANEL    lv_color_hex(0x161B22)
#define COL_BG_PANEL_2  lv_color_hex(0x1F2630)
#define COL_TEXT        lv_color_hex(0xE6EDF3)
#define COL_TEXT_DIM    lv_color_hex(0x8B98A5)
#define COL_ACCENT      lv_color_hex(0x2EE6D6)

// -----------------------------------------------------------------------------
// Layout 480×320:
//   y  0..63   : fejléc-sáv — Exit gomb (bal), játéknév (közép), bíp-jelző (jobb)
//   y 64..319  : játéktér-zóna; a 448×224-es (64×32 × 7) framebuffer közepén
// A meglévő top-layer overlay (battery/volume/lock + swipe-sáv) a játék alatt
// rejtve van — a swipe-gesztus képernyőváltása bekavarna a játékba.
// -----------------------------------------------------------------------------
#define GAME_SCALE   7
#define GAME_W       (CHIP8_W * GAME_SCALE)
#define GAME_H       (CHIP8_H * GAME_SCALE)
#define HEADER_H     64
#define GAME_X       ((LCD_H_RES - GAME_W) / 2)
#define GAME_Y       (HEADER_H + (LCD_V_RES - HEADER_H - GAME_H) / 2)

// RGB565 pixel-színek (a flush swap_bytes flagje intézi a byte-sorrendet,
// mint minden más renderelt tartalomnál).
#define PX_FG  0x2F3A   // COL_ACCENT (0x2EE6D6)
#define PX_BG  0x0882   // COL_BG (0x0E1116)

// Játék-loop: 16 ms-os lv_timer, de a 60 Hz-es időalap az eltelt VALÓS időből
// jön (catch-up): ha egy redraw miatt a timer csúszik, a következő lefutás
// több 16,67 ms-os adagot pótol — a játéksebesség így a render ingadozásától
// független, csak a megjelenítési frame-ráta döccen.
//
// Az adagonkénti utasítás-büdzsé (G.ips) futásidőben hangolható (CLI `gips`):
// a ROM-ok nem definiálnak órajelet, és a "jó" érték játékfüggő. Alacsony
// büdzsével a sok-sprite-os rajzolás-burstök (pl. az Invaders teljes rácsa)
// több frame-en át húzódnak — a ROM logikája (lövedék!) addig áll.
#define TICK_MS         16
#define FRAME_US        16667   // egy 60 Hz-es adag valós ideje
#define MAX_CATCHUP     4       // extrém csúszás után ennyi adagnál többet nem pótlunk
#define GAME_IPS_DEFAULT 20
#define GAME_IPS_MIN     5
#define GAME_IPS_MAX     200

// Game mode alatt az LVGL display-refresh 16 ms (60 fps) — a default
// CONFIG_LV_DEF_REFR_PERIOD (33 ms) 30 fps-re plafonozná a mozgást,
// hiába invalidálunk 60 Hz-en. Kilépéskor visszaáll.
#define GAME_REFR_PERIOD_MS  16

// Fizikai gomb → CHIP-8 kulcs. A klasszikusok zöme a 4/6 (bal/jobb) + 5
// (tűz/akció) hármast használja; a 2/8 a fel/le. A gombokat nyersen pollozzuk
// (io_button_down) — az eseményalapú út csak press-t ad, a CHIP-8-nak
// tartás-állapot kell (EX9E/EXA1).
static const struct { btn_event_t evt; int key; } KEYMAP[] = {
    { BTN_EVT_LEFT,       0x4 },
    { BTN_EVT_RIGHT,       0x6 },
    { BTN_EVT_A, 0x5 },
    { BTN_EVT_UP,     0x2 },
    { BTN_EVT_DOWN,   0x8 },
};

// --- Game screen állapot ---
static struct {
    bool            active;
    volatile bool   exit_req;      // kilépés-kérés (touch Exit / külső) — async lebontás
    lv_obj_t       *scr;
    lv_obj_t       *img;           // a játéktér (lv_image a fbuf fölött)
    lv_obj_t       *beep_dot;      // bíp-jelző a fejlécben (hang helyett villan)
    lv_timer_t     *timer;
    uint8_t        *fbuf;          // GAME_W×GAME_H RGB565 (PSRAM)
    lv_image_dsc_t  dsc;
    bool            beep_shown;
    bool            halted_logged;
    game_exit_cb_t  on_exit;
    // CLI-ből injektált kulcs-tapek: kulcsonként hátralévő "lenyomva" tickek
    // (game_handle_button tölti, a poll_keys csorgatja le).
    uint8_t         inject[CHIP8_KEYS];
    // A CHIP-8 VM munkaterülete (PSRAM, lásd chip8_attach) — első indításkor
    // allokáljuk, utána megtartjuk (~6 KB, nem éri meg ciklikusan szabadítani).
    void           *vm;
    int64_t         last_us;       // a 60 Hz-es időalap órája (catch-up)
    int             ips;           // utasítás / 60 Hz-es adag (CLI `gips`)
} G;

// --- Game picker állapot ---
static struct {
    lv_obj_t    *scr;
    dir_entry_t *entries;          // csak a .ch8 sorok (előre tömörítve)
    int          count;
    char         pending[MAX_PATH_LEN];   // tap után indítandó ROM útvonala
} P;

// -----------------------------------------------------------------------------
// Megjelenítés
// -----------------------------------------------------------------------------

// A 64×32-es 0/1 framebuffer adott (zárt) téglalapjának expandálása a
// 448×224-es RGB565 bufferbe. Pixelenként GAME_SCALE széles futam, majd a
// kész sáv (GAME_SCALE-1)-szeri másolása — a PSRAM-írás így zömmel memcpy.
static void render_fb_rect(int cx0, int cy0, int cx1, int cy1)
{
    const uint8_t *fb = chip8_fb();
    uint16_t *dst = (uint16_t *)G.fbuf;
    size_t off = (size_t)cx0 * GAME_SCALE;
    size_t len = (size_t)(cx1 - cx0 + 1) * GAME_SCALE * sizeof(uint16_t);
    for (int y = cy0; y <= cy1; y++) {
        uint16_t *row0 = dst + (size_t)y * GAME_SCALE * GAME_W;
        for (int x = cx0; x <= cx1; x++) {
            uint16_t c = fb[y * CHIP8_W + x] ? PX_FG : PX_BG;
            uint16_t *p = row0 + x * GAME_SCALE;
            for (int i = 0; i < GAME_SCALE; i++) p[i] = c;
        }
        for (int i = 1; i < GAME_SCALE; i++) {
            memcpy(row0 + (size_t)i * GAME_W + off, row0 + off, len);
        }
    }
}

// Közös fejléc-sáv a game/picker screenhez: bal gomb + cím. Visszaadja a
// sávot (a bíp-jelzőt a hívó teszi rá, ha kell).
static lv_obj_t *build_header(lv_obj_t *scr, const char *btn_text,
                              lv_event_cb_t btn_cb, const char *title_text)
{
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LCD_H_RES, HEADER_H);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, COL_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *btn = lv_button_create(hdr);
    lv_obj_set_size(btn, 110, 44);
    lv_obj_align(btn, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_bg_color(btn, COL_BG_PANEL_2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_obj_set_style_text_font(bl, &mp3_inter_18, 0);
    lv_obj_set_style_text_color(bl, COL_ACCENT, 0);
    lv_label_set_text(bl, btn_text);
    lv_obj_center(bl);

    lv_obj_t *title = lv_label_create(hdr);
    lv_obj_set_style_text_font(title, &mp3_inter_18, 0);
    lv_obj_set_style_text_color(title, COL_ACCENT, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, 220);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(title, title_text);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    return hdr;
}

static void apply_dark_bg(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(scr, COL_TEXT, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(scr, &mp3_inter_14, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

// Fájlnév → kijelzett név (kiterjesztés nélkül; a csonkolás szándékos).
static void rom_display_name(const char *fname, char *out, size_t n)
{
    strlcpy(out, fname, n);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

// -----------------------------------------------------------------------------
// Game screen életciklus
// -----------------------------------------------------------------------------

// Lebontás — KIZÁRÓLAG LVGL task kontextusból (timer cb / lv_async_call).
// Sorrend: loop le → overlay vissza → előző képernyő vissza → a game screen
// objektumai csak ezután törölhetők (nem az aktív screen) → buffer fel.
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
    ui_show_screen(ui_current_screen());   // U.current a játék alatt nem változott
    lv_image_cache_drop(&G.dsc);
    lv_obj_delete(G.scr);
    G.scr = NULL;
    lvgl_port_unlock();

    heap_caps_free(G.fbuf);
    G.fbuf = NULL;
    ui_set_idle_inhibit(false);

    ESP_LOGI(TAG, "game exit");
    if (G.on_exit) G.on_exit();            // player.c: zene folytatása
}

static void exit_async(void *p)
{
    (void)p;
    do_exit();
}

// Exit gomb tap: a screen törlését nem tehetjük a saját eseménye közben
// (use-after-free) — lv_async_call, mint a lib_row_click-nél.
static void exit_btn_click(lv_event_t *e)
{
    (void)e;
    lv_async_call(exit_async, NULL);
}

static void poll_keys(void)
{
    for (size_t i = 0; i < sizeof(KEYMAP) / sizeof(KEYMAP[0]); i++) {
        int key = KEYMAP[i].key;
        chip8_set_key(key, io_button_down(KEYMAP[i].evt) || G.inject[key] > 0);
        if (G.inject[key]) G.inject[key]--;
    }
}

// ~8 tick ≈ 128 ms szimulált lenyomás: az EX9E/EXA1 pollnak bőven elég, és
// a lejártával az FX0A press-then-release feltétele is teljesül.
#define INJECT_TICKS 8

void game_handle_button(btn_event_t evt)
{
    if (!G.active) return;
    for (size_t i = 0; i < sizeof(KEYMAP) / sizeof(KEYMAP[0]); i++) {
        if (KEYMAP[i].evt == evt) {
            G.inject[KEYMAP[i].key] = INJECT_TICKS;
            return;
        }
    }
}

static void game_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (G.exit_req) {
        do_exit();
        return;
    }

    // Catch-up: annyi 60 Hz-es adagot hajtunk végre, amennyi valós idő eltelt.
    // A tört maradék megmarad (last_us adagonként lép); a korlát feletti
    // adósságot eldobjuk, hogy egy extrém stall után ne pörögjön be a játék.
    int64_t now = esp_timer_get_time();
    int steps = (int)((now - G.last_us) / FRAME_US);
    if (steps <= 0) return;
    if (steps > MAX_CATCHUP) {
        steps = MAX_CATCHUP;
        G.last_us = now;
    } else {
        G.last_us += (int64_t)steps * FRAME_US;
    }

    // A burst-plafon az alap-büdzsé többszöröse, de legalább akkora, hogy egy
    // teljes rács-újrarajzolás (néhány száz utasítás) egy frame-be beférjen.
    int maxb = G.ips * 10;
    if (maxb < 600)  maxb = 600;
    if (maxb > 2000) maxb = 2000;

    for (int s = 0; s < steps; s++) {
        poll_keys();
        chip8_run_frame(G.ips, maxb);
        chip8_tick_60hz();
    }

    // Csak a változott területeket expandáljuk és invalidáljuk — KÜLÖN
    // rectekként: egy távoli kis változás (lövedék) és egy nagy blokk
    // (invader-rács) együtt sem uniózódik teljes képernyővé, a flush a
    // ténylegesen változott pixelekkel arányos.
    chip8_rect_t rects[CHIP8_DIRTY_RECTS_MAX];
    int nr = chip8_take_dirty_rects(rects, CHIP8_DIRTY_RECTS_MAX);
    if (nr > 0) {
        lv_image_cache_drop(&G.dsc);
        lv_area_t coords;
        lv_obj_get_coords(G.img, &coords);
        for (int i = 0; i < nr; i++) {
            const chip8_rect_t *r = &rects[i];
            render_fb_rect(r->x0, r->y0, r->x1, r->y1);
            lv_area_t a = {
                .x1 = coords.x1 + r->x0 * GAME_SCALE,
                .y1 = coords.y1 + r->y0 * GAME_SCALE,
                .x2 = coords.x1 + (r->x1 + 1) * GAME_SCALE - 1,
                .y2 = coords.y1 + (r->y1 + 1) * GAME_SCALE - 1,
            };
            lv_obj_invalidate_area(G.img, &a);
        }
    }

    // Bíp: hang helyett (1. fázis) a fejléc-jelző villan accent színre.
    bool beep = chip8_beeping();
    if (beep != G.beep_shown) {
        lv_obj_set_style_bg_color(G.beep_dot, beep ? COL_ACCENT : COL_BG_PANEL_2,
                                  LV_PART_MAIN);
        G.beep_shown = beep;
    }

    if (chip8_halted() && !G.halted_logged) {
        // Tipikus ok: nem CHIP-8 ROM (pl. SCHIP-only opcode-ok). A képernyőn
        // az utolsó frame marad, az Exit kivezet.
        ESP_LOGW(TAG, "VM halted (ismeretlen opcode / stack-hiba)");
        G.halted_logged = true;
    }
}

bool game_start(const char *rom_path, game_exit_cb_t on_exit)
{
    if (G.active) return false;

    // VM-állapot PSRAM-ból — a belső RAM-ot (LVGL DMA-bufferek) nem terheljük.
    if (!G.vm) {
        G.vm = heap_caps_malloc(chip8_state_bytes(), MALLOC_CAP_SPIRAM);
        if (!G.vm) {
            ESP_LOGE(TAG, "VM state alloc failed");
            return false;
        }
        chip8_attach(G.vm);
    }

    // ROM beolvasása RAM-ba — a játék alatt nincs több SD-hozzáférés, így a
    // közös SPI buszon sem versenyzünk a kijelzővel. Az fread a flush-okkal
    // a szokásos módon serializálva (ui_spi_lock).
    uint8_t *rom = malloc(CHIP8_MAX_ROM);
    if (!rom) return false;
    long n = 0;
    ui_spi_lock();
    FILE *f = fopen(rom_path, "rb");
    if (f) {
        n = (long)fread(rom, 1, CHIP8_MAX_ROM, f);
        if (!feof(f)) n = 0;               // nem értünk a végére → túl nagy, nem ROM
        fclose(f);
    }
    ui_spi_unlock();
    if (n <= 0) {
        ESP_LOGW(TAG, "ROM betöltés sikertelen: %s", rom_path);
        free(rom);
        return false;
    }

    chip8_reset(NULL);
    chip8_seed((uint32_t)esp_timer_get_time());
    chip8_load((const uint8_t *)rom, (size_t)n);
    free(rom);

    G.fbuf = heap_caps_malloc(GAME_W * GAME_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!G.fbuf) {
        ESP_LOGE(TAG, "framebuffer alloc failed");
        return false;
    }

    memset(&G.dsc, 0, sizeof(G.dsc));
    G.dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    G.dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    G.dsc.header.w      = GAME_W;
    G.dsc.header.h      = GAME_H;
    G.dsc.header.stride = GAME_W * sizeof(uint16_t);
    G.dsc.data          = G.fbuf;
    G.dsc.data_size     = GAME_W * GAME_H * sizeof(uint16_t);

    char name[64];
    const char *base = strrchr(rom_path, '/');
    rom_display_name(base ? base + 1 : rom_path, name, sizeof(name));

    lvgl_port_lock(0);
    // Az overlay-t (battery/volume/lock + swipe-sáv) elrejtjük — a fejléc a
    // játéké, és a swipe képernyőváltása bekavarna. A kilépés visszahozza.
    lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);

    G.scr = lv_obj_create(NULL);
    apply_dark_bg(G.scr);

    lv_obj_t *hdr = build_header(G.scr, LV_SYMBOL_LEFT "  Exit", exit_btn_click, name);
    G.beep_dot = lv_obj_create(hdr);
    lv_obj_remove_style_all(G.beep_dot);
    lv_obj_set_size(G.beep_dot, 14, 14);
    lv_obj_align(G.beep_dot, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_radius(G.beep_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(G.beep_dot, COL_BG_PANEL_2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(G.beep_dot, LV_OPA_COVER, LV_PART_MAIN);

    G.img = lv_image_create(G.scr);
    lv_obj_set_pos(G.img, GAME_X, GAME_Y);
    lv_image_set_src(G.img, &G.dsc);
    // Vékony keret a játéktér körül — látszódjon, hol ér véget a 64×32-es világ.
    lv_obj_set_style_border_color(G.img, COL_BG_PANEL_2, LV_PART_MAIN);
    lv_obj_set_style_border_width(G.img, 1, LV_PART_MAIN);

    render_fb_rect(0, 0, CHIP8_W - 1, CHIP8_H - 1);   // üres első frame
    lv_screen_load(G.scr);

    G.exit_req      = false;
    G.beep_shown    = false;
    G.halted_logged = false;
    G.on_exit       = on_exit;
    memset(G.inject, 0, sizeof(G.inject));
    G.last_us       = esp_timer_get_time();
    if (G.ips == 0) G.ips = GAME_IPS_DEFAULT;   // a CLI-vel állított érték megmarad
    G.active        = true;
    G.timer = lv_timer_create(game_tick_cb, TICK_MS, NULL);
    lv_timer_set_period(lv_display_get_refr_timer(lv_display_get_default()),
                        GAME_REFR_PERIOD_MS);
    lvgl_port_unlock();

    ui_set_idle_inhibit(true);   // játék alatt nincs display-off
    ESP_LOGI(TAG, "game start: %s (%ld byte)", rom_path, n);
    return true;
}

bool game_is_active(void)
{
    return G.active;
}

void game_set_ips(int ips)
{
    if (ips < GAME_IPS_MIN) ips = GAME_IPS_MIN;
    if (ips > GAME_IPS_MAX) ips = GAME_IPS_MAX;
    G.ips = ips;
    ESP_LOGI(TAG, "ips = %d (%d utasítás/s)", ips, ips * 60);
}

void game_request_exit(void)
{
    G.exit_req = true;           // a következő tick bontja le (LVGL kontextus)
}

// -----------------------------------------------------------------------------
// Game picker (Settings → Games)
// -----------------------------------------------------------------------------

// Bezárás — LVGL task kontextusból (async / saját esemény után).
static void picker_close(void)
{
    if (!P.scr) return;
    lvgl_port_lock(0);
    lv_obj_remove_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
    ui_show_screen(ui_current_screen());   // vissza a Settingsre
    lv_obj_delete(P.scr);
    P.scr = NULL;
    lvgl_port_unlock();
    heap_caps_free(P.entries);
    P.entries = NULL;
    P.count = 0;
}

static void picker_close_async(void *p)
{
    (void)p;
    picker_close();
}

static void picker_back_click(lv_event_t *e)
{
    (void)e;
    lv_async_call(picker_close_async, NULL);
}

// ROM-sor tap (async): útvonal kimásolása, picker le, indítás a player-en át
// (zene-szüneteltetés ott történik).
static void picker_play_async(void *p)
{
    int idx = (int)(intptr_t)p;
    if (!P.entries || idx < 0 || idx >= P.count) return;
    snprintf(P.pending, sizeof(P.pending), GAMES_DIR "/%s", P.entries[idx].name);
    picker_close();
    player_launch_game(P.pending);
}

static void picker_row_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    lv_async_call(picker_play_async, (void *)(intptr_t)idx);
}

void game_show_picker(void)
{
    if (G.active || gbmode_is_active() || P.scr) return;

    P.entries = heap_caps_calloc(MAX_DIR_ENTRIES, sizeof(dir_entry_t),
                                 MALLOC_CAP_SPIRAM);
    if (!P.entries) return;
    int n = sd_list_dir(GAMES_DIR, P.entries, MAX_DIR_ENTRIES);
    P.count = 0;
    for (int i = 0; i < n; i++) {
        if (P.entries[i].is_ch8 || P.entries[i].is_gb) {
            P.entries[P.count++] = P.entries[i];
        }
    }

    lvgl_port_lock(0);
    lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);

    P.scr = lv_obj_create(NULL);
    apply_dark_bg(P.scr);
    build_header(P.scr, LV_SYMBOL_LEFT "  Back", picker_back_click, "GAMES");

    lv_obj_t *list = lv_list_create(P.scr);
    lv_obj_set_size(list, 456, LCD_V_RES - HEADER_H - 24);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 12, HEADER_H + 12);
    lv_obj_set_style_bg_color(list, COL_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 2, LV_PART_MAIN);
    lv_obj_set_style_text_font(list, &mp3_inter_14, LV_PART_MAIN);

    if (P.count == 0) {
        lv_obj_t *empty = lv_list_add_text(list,
            "No games found.\nCopy .ch8 ROMs to " GAMES_DIR " on the SD card.");
        lv_obj_set_style_text_color(empty, COL_TEXT_DIM, 0);
    }
    for (int i = 0; i < P.count; i++) {
        char name[64];
        rom_display_name(P.entries[i].name, name, sizeof(name));
        lv_obj_t *btn = lv_list_add_button(list, LV_SYMBOL_PLAY, name);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_text_color(btn, COL_TEXT, LV_PART_MAIN);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, picker_row_click, LV_EVENT_CLICKED, NULL);
    }

    lv_screen_load(P.scr);
    lvgl_port_unlock();
}
