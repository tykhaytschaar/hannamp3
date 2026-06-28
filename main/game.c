#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_heap_caps.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "app_config.h"
#include "game.h"
#include "gb.h"     // gbmode_is_active (a picker innen indít GB ROM-ot)
#include "io.h"
#include "sd.h"
#include "ui.h"
#include "player.h"
#include "mp3_fonts.h"

// A Settings → Games képernyő: a GAMES_DIR Game Boy (.gb) ROM-jainak
// választólistája. Tap egy soron → player_launch_game() → gb.c. Maga az
// emuláció és a játékképernyő a gb.c-ben él; itt csak a picker van.

// Theme tokenek — az ui.c-vel egyező értékek (ott file-local makrók).
#define COL_BG          lv_color_hex(0x0E1116)
#define COL_BG_PANEL    lv_color_hex(0x161B22)
#define COL_BG_PANEL_2  lv_color_hex(0x1F2630)
#define COL_TEXT        lv_color_hex(0xE6EDF3)
#define COL_TEXT_DIM    lv_color_hex(0x8B98A5)
#define COL_ACCENT      lv_color_hex(0x2EE6D6)

#define HEADER_H     64

// --- Game picker állapot ---
static struct {
    lv_obj_t    *scr;
    dir_entry_t *entries;          // csak a .gb sorok (előre tömörítve)
    int          count;
    char         pending[MAX_PATH_LEN];   // tap után indítandó ROM útvonala
} P;

// -----------------------------------------------------------------------------
// Közös UI-helperek
// -----------------------------------------------------------------------------

// Fejléc-sáv: bal gomb + középre igazított cím.
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
// (zene-leállítás ott történik).
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
    if (gbmode_is_active() || P.scr) return;

    P.entries = heap_caps_calloc(MAX_DIR_ENTRIES, sizeof(dir_entry_t),
                                 MALLOC_CAP_SPIRAM);
    if (!P.entries) return;
    int n = sd_list_dir(GAMES_DIR, P.entries, MAX_DIR_ENTRIES);
    P.count = 0;
    for (int i = 0; i < n; i++) {
        if (P.entries[i].is_gb) {
            P.entries[P.count++] = P.entries[i];
        }
    }

    lvgl_port_lock(0);
    lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);

    P.scr = lv_obj_create(NULL);
    apply_dark_bg(P.scr);
    build_header(P.scr, LV_SYMBOL_LEFT "  Vissza", picker_back_click, "JÁTÉKOK");

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
            "Nincs játék.\nMásolj .gb ROM-okat ide: " GAMES_DIR " az SD kártyán.");
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
