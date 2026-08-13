#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_heap_caps.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "app_config.h"
#include "game.h"
#include "sd.h"
#include "ui.h"
#include "player.h"
#include "mp3_fonts.h"

// A Játékok oldal tartalma: a GAMES_DIR Game Boy / Game Boy Color (.gb/.gbc)
// ROM-jainak választólistája. Az oldal maga az ui.c screen-rotáció része
// (UI_SCREEN_GAMES) — az ui_init hívja a game_screen_create-et, az oldalra
// belépéskor pedig az ui_show_screen a game_screen_refresh-t (SD-újraszkennelés).
// Tap egy soron → player_launch_game() → gb.c. Maga az emuláció és a
// játékképernyő a gb.c-ben él; kilépéskor a gb.c ide (UI_SCREEN_GAMES) tér vissza.

// Theme tokenek — az ui.c-vel egyező értékek (ott file-local makrók).
#define COL_BG_PANEL    lv_color_hex(0x161B22)
#define COL_TEXT        lv_color_hex(0xE6EDF3)
#define COL_TEXT_DIM    lv_color_hex(0x8B98A5)

// --- Játékok oldal állapota ---
static struct {
    lv_obj_t    *list;             // a ROM-lista widget (a UI_SCREEN_GAMES screenen)
    dir_entry_t *entries;          // csak a .gb/.gbc sorok (előre tömörítve);
                                   // életben marad — a tap-async ebből olvas
    int          count;
    char         pending[MAX_PATH_LEN];   // tap után indítandó ROM útvonala
} P;

// Fájlnév → kijelzett név (kiterjesztés nélkül; a csonkolás szándékos).
static void rom_display_name(const char *fname, char *out, size_t n)
{
    strlcpy(out, fname, n);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

// ROM-sor tap (async): útvonal kimásolása + indítás a player-en át
// (zene-leállítás és gbmode_is_active-guard ott történik).
static void row_play_async(void *p)
{
    int idx = (int)(intptr_t)p;
    if (!P.entries || idx < 0 || idx >= P.count) return;
    snprintf(P.pending, sizeof(P.pending), GAMES_DIR "/%s", P.entries[idx].name);
    player_launch_game(P.pending);
}

static void row_click(lv_event_t *e)
{
    if (ui_user_activity()) return;   // alvó kijelzőn a tap csak ébreszt
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    lv_async_call(row_play_async, (void *)(intptr_t)idx);
}

// Az oldal statikus váza — ui_init hívja (LVGL lock alatt), a screen stílusát
// (háttér, default font) az ui.c apply_screen_bg-je már beállította.
void game_screen_create(lv_obj_t *scr)
{
    P.list = lv_list_create(scr);
    lv_obj_set_size(P.list, 456, LCD_V_RES - 32 - 12);
    lv_obj_align(P.list, LV_ALIGN_TOP_LEFT, 12, 32);
    lv_obj_set_style_bg_color(P.list, COL_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_border_width(P.list, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(P.list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(P.list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(P.list, 2, LV_PART_MAIN);
    lv_obj_set_style_text_font(P.list, &mp3_inter_14, LV_PART_MAIN);
}

// ROM-lista újraépítés az SD-ről. Az ui_show_screen hívja az oldalra
// belépéskor, a portlock alatt (kötelező is: az sd_list_dir a közös SPI
// buszt használja, miközben LVGL-flush futhatna).
void game_screen_refresh(void)
{
    if (!P.list) return;
    if (!P.entries) {
        P.entries = heap_caps_calloc(MAX_DIR_ENTRIES, sizeof(dir_entry_t),
                                     MALLOC_CAP_SPIRAM);
        if (!P.entries) return;
    }

    int n = sd_list_dir(GAMES_DIR, P.entries, MAX_DIR_ENTRIES);
    P.count = 0;
    for (int i = 0; i < n; i++) {
        if (P.entries[i].is_gb) {
            P.entries[P.count++] = P.entries[i];
        }
    }

    lvgl_port_lock(0);
    lv_obj_clean(P.list);

    if (P.count == 0) {
        lv_obj_t *empty = lv_list_add_text(P.list,
            "Nincs játék.\nMásolj .gb/.gbc ROM-okat ide: " GAMES_DIR " az SD kártyán.");
        lv_obj_set_style_text_color(empty, COL_TEXT_DIM, 0);
    }
    for (int i = 0; i < P.count; i++) {
        char name[64];
        rom_display_name(P.entries[i].name, name, sizeof(name));
        lv_obj_t *btn = lv_list_add_button(P.list, LV_SYMBOL_PLAY, name);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_text_color(btn, COL_TEXT, LV_PART_MAIN);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, row_click, LV_EVENT_CLICKED, NULL);
    }
    lvgl_port_unlock();
}
