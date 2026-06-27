#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"

#include "app_config.h"
#include "sd.h"
#include "audio.h"
#include "ui.h"
#include "io.h"
#include "player.h"
#include "game.h"
#include "gb.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "driver/gpio.h"

static const char *TAG = "player";

// Lejátszási kontextus: az ÉPP játszott mappa MP3-jai (auto-next ezen megy végig).
static track_t *s_tracks = NULL;
static int      s_count  = 0;
static int      s_idx    = 0;

// Böngésző navigációs állapot (Library képernyő) — független a lejátszástól.
static dir_entry_t *s_bentries = NULL;
static int          s_bcount   = 0;
static int          s_bcursor  = 0;
static char         s_bpath[384];   // aktuális könyvtár, SD_MOUNT_POINT-ról indul


// --- NVS perzisztencia: böngészett könyvtár + utoljára nyitott fájl ---
#define NVS_NS  "player"

static void persist_set_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, val ? val : "");
    nvs_commit(h);
    nvs_close(h);
}

static bool persist_get_str(const char *key, char *out, size_t out_sz)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = out_sz;
    esp_err_t e = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return e == ESP_OK;
}

static void persist_set_i32(const char *key, int32_t val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

static bool persist_get_i32(const char *key, int32_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t e = nvs_get_i32(h, key, out);
    nvs_close(h);
    return e == ESP_OK;
}

// Háttérvilágítás fényerő: alkalmazás (LEDC PWM az ui.c-ben) + azonnali
// NVS-mentés. A Settings-edit és a CLI `bl` parancs is ezt hívja, így a
// beállítás forrástól függetlenül perzisztens. Boot-kor a player_start
// olvassa vissza és állítja be (mentés nélkül).
void player_set_backlight(uint8_t pct)
{
    if (pct > 100) pct = 100;
    ui_set_backlight(pct);
    persist_set_i32("bl_pct", (int32_t)pct);
}

// Hangerő: alkalmazás (audio + UI/header chip + slider) + NVS-mentés. A Settings
// volume slider release-e hívja; boot-kor a player_start olvassa vissza.
void player_set_volume(uint8_t vol)
{
    if (vol > 100) vol = 100;
    audio_set_volume(vol);
    ui_set_volume(vol);
    persist_set_i32("volume", (int32_t)vol);
}

// Settings "Display off" sor tap: a timeout ciklikus léptetése + NVS-mentés.
void player_cycle_idle_timeout(void)
{
    int v = ui_cycle_idle_timeout(+1);   // 10→15→30→Never→10, frissíti a labelt
    persist_set_i32("idle_s", v);
}

// Settings "Sleep" sor tap: on/off váltás + NVS-mentés.
void player_toggle_sleep(void)
{
    bool en = ui_toggle_sleep_enabled();
    persist_set_i32("sleep_en", en ? 1 : 0);
}

// Settings "Album end" sor tap: Stop→Repeat→Next album léptetés + NVS-mentés.
void player_cycle_album_end(void)
{
    int m = ui_cycle_album_end_mode();
    persist_set_i32("alb_end", m);
}

static void browser_refresh(void)
{
    // FONTOS: itt NINCS NVS-írás. Régen minden mappa-navigáció persist_set_str-t
    // hívott, de a flash-commit kikapcsolja az instrukció-cache-t → mindkét mag
    // megáll pár-tíz ms-ra → az audio megakad lejátszás közben. A böngészett
    // mappát csak fájl-indításkor mentjük (browser_activate), ami úgyis
    // track-váltás → a pici stall ott elrejtve.
    s_bcount = sd_list_dir(s_bpath, s_bentries, MAX_DIR_ENTRIES);
    if (s_bcursor >= s_bcount) s_bcursor = s_bcount > 0 ? s_bcount - 1 : 0;
    if (s_bcursor < 0) s_bcursor = 0;
    ui_browser_show(s_bpath, s_bentries, s_bcount, s_bcursor);
}

static void browser_move_cursor(int delta)
{
    if (s_bcount == 0) return;
    int c = s_bcursor + delta;
    while (c < 0)         c += s_bcount;
    while (c >= s_bcount) c -= s_bcount;
    s_bcursor = c;
    ui_browser_set_cursor(s_bcursor);
}

static void browser_enter(void)
{
    if (s_bcount == 0 || !s_bentries[s_bcursor].is_dir) return;
    size_t len = strlen(s_bpath);
    snprintf(s_bpath + len, sizeof(s_bpath) - len, "/%s", s_bentries[s_bcursor].name);
    s_bcursor = 0;
    browser_refresh();
}

static void browser_up(void)
{
    // A böngésző gyökere a MUSIC_DIR — a kártya többi része (games, egyéb)
    // nem tartozik a Library-re.
    if (strcmp(s_bpath, MUSIC_DIR) == 0) return;
    char *slash = strrchr(s_bpath, '/');
    if (slash) {
        *slash = 0;
        if (strlen(s_bpath) < strlen(MUSIC_DIR)) {
            strcpy(s_bpath, MUSIC_DIR);
        }
    }
    s_bcursor = 0;
    browser_refresh();
}

static void select_current(bool autoplay)
{
    if (s_count == 0) {
        ui_show_no_track();
        return;
    }
    if (s_idx < 0) s_idx = s_count - 1;
    if (s_idx >= s_count) s_idx = 0;

    // Prev/next közben a régi track még szól, és az ui_show_track album-cover-
    // betöltése foglalja a SPI buszt → DMA underrun → glitch. Mute-oljuk a
    // DAC-ot azonnal, a CMD_PLAY úgyis ezt teszi pár ms múlva.
    if (autoplay) audio_dac_mute();

    ui_show_track(&s_tracks[s_idx]);
    ui_set_playlist(s_tracks, s_count, s_idx);
    persist_set_str("last_file", s_tracks[s_idx].path);   // utoljára nyitott fájl
    if (autoplay) {
        ui_set_state(AUDIO_STATE_PLAYING);
        // Az audio task CMD_PLAY indulásakor letiltja az I2S-t (lásd audio.c) —
        // ezzel instant csend lesz az átmenet alatt, mute hack nem kell.
        audio_play(s_tracks[s_idx].path);
    } else {
        ui_set_state(AUDIO_STATE_STOPPED);
    }
}

static void play_current(void)
{
    select_current(true);
}

// A betöltött lista "album-szerű"-e: minden track ugyanabból a mappából való.
// Mappa-betöltésnél triviálisan igaz; m3u-nál akkor, ha a playlist egyetlen
// album számait sorolja — ilyenkor a "Next album" mód arra is értelmezett
// (a mappa testvérei közt lép). Albumokon átívelő m3u-nál false → a lista
// végén stop, a szélein a Next/Prev no-op.
static bool list_is_single_folder(void)
{
    if (s_count <= 0) return false;
    const char *p0 = s_tracks[0].path;
    const char *s0 = strrchr(p0, '/');
    if (!s0) return false;
    size_t dlen = (size_t)(s0 - p0);
    for (int i = 1; i < s_count; i++) {
        const char *pi = s_tracks[i].path;
        const char *si = strrchr(pi, '/');
        if (!si || (size_t)(si - pi) != dlen || strncmp(pi, p0, dlen) != 0) {
            return false;
        }
    }
    return true;
}

// Szomszéd album betöltése (definíció a player_task előtt) — a gombkezelő is
// hívja a lista határán, "Next album" módban.
static bool advance_album(int dir, bool autoplay);

// Játék indításakor a zene leáll (stop, nem pause — nincs automatikus
// folytatás kilépéskor): a játék után a Play gomb a kiválasztott tracket
// elölről indítja. Betöltési hibánál a lejátszáshoz nem nyúlunk.
// Kiterjesztés szerint dönt: .gb → Game Boy mód (gb.c), egyébként CHIP-8.
void player_launch_game(const char *path)
{
    if (game_is_active() || gbmode_is_active()) return;
    const char *ext = strrchr(path, '.');
    bool is_gb = ext && strcasecmp(ext, ".gb") == 0;
    bool started = is_gb ? gbmode_start(path, NULL)
                         : game_start(path, NULL);
    if (started) {
        audio_stop();
        ui_set_state(AUDIO_STATE_STOPPED);
        ui_set_progress(0, 0);
    }
}

// Play a böngészőben: mappán állva belép; m3u-n a playlist lesz a lejátszási
// lista (a lista sorrendjében); fájlon a mappa MP3-jai, a kiválasztottól indítva.
static void browser_activate(void)
{
    if (s_bcount == 0) return;
    const dir_entry_t *ent = &s_bentries[s_bcursor];
    if (ent->is_dir) {
        browser_enter();
        return;
    }

    char target[512];
    snprintf(target, sizeof(target), "%.383s/%.127s", s_bpath, ent->name);

    if (ent->is_ch8 || ent->is_gb) {
        // Játék-ROM: game mode (CHIP-8 / GB) — a lejátszási listához nem
        // nyúlunk, a zene leállítását a player_launch_game intézi.
        player_launch_game(target);
        return;
    }

    if (ent->is_m3u) {
        // Playlist: a lejátszási lista az m3u sorrendje — a prev/next és az
        // auto-advance is ezt követi (s_tracks feltöltési sorrend = sorrend).
        s_count = sd_load_m3u_tracks(target, s_tracks, MAX_TRACKS);
        s_idx = 0;
        if (s_count == 0) {
            ui_show_no_track();   // üres/hibás lista — nincs mit indítani
            return;
        }
    } else {
        // Fájl: a böngészett mappa összes MP3-ja lesz a lejátszási lista (album).
        s_count = sd_load_dir_tracks(s_bpath, s_tracks, MAX_TRACKS);
        s_idx = 0;
        for (int i = 0; i < s_count; i++) {
            if (strcmp(s_tracks[i].path, target) == 0) { s_idx = i; break; }
        }
    }
    play_current();
    ui_show_screen(UI_SCREEN_NOW_PLAYING);
    // A böngészett mappát itt mentjük (nem minden navigációnál) — track-váltás
    // van, a flash-commit cache-stall-ja a frissen induló lejátszásban elrejtve.
    persist_set_str("br_dir", s_bpath);
}

// Library "Play all" sor (m3u nélküli mappák): a mappa fájljainak betöltése
// név-sorrendben + lejátszás az elsőtől + váltás Now Playingre. Ekvivalens
// az első fájlra tapeléssel.
void player_browser_play_all(void)
{
    if (ui_user_activity()) return;        // alvó kijelző: a tap csak ébreszt
    s_count = sd_load_dir_tracks(s_bpath, s_tracks, MAX_TRACKS);
    s_idx = 0;
    if (s_count == 0) {
        ui_show_no_track();
        return;
    }
    play_current();
    ui_show_screen(UI_SCREEN_NOW_PLAYING);
    persist_set_str("br_dir", s_bpath);
}

void player_do_action(player_action_t a)
{
    switch (a) {
    case PLAYER_ACTION_PREV:       player_handle_button(BTN_EVT_LEFT);       break;
    case PLAYER_ACTION_PLAY_PAUSE: player_handle_button(BTN_EVT_A); break;
    case PLAYER_ACTION_NEXT:       player_handle_button(BTN_EVT_RIGHT);       break;
    case PLAYER_ACTION_STOP:
        if (ui_user_activity()) return;     // alvó kijelző: a tap csak ébreszt
        audio_stop();
        ui_set_state(AUDIO_STATE_STOPPED);
        ui_set_progress(0, 0);
        break;
    }
}

void player_play_index(int idx)
{
    if (ui_user_activity()) return;         // alvó kijelző: a tap csak ébreszt
    if (idx < 0 || idx >= s_count) return;
    if (idx == s_idx) {
        audio_status_t st;
        audio_get_status(&st);
        if (st.state == AUDIO_STATE_PLAYING) return;   // már ezt játssza → no-op
    }
    s_idx = idx;
    play_current();
}

void player_browser_tap(int idx)
{
    if (ui_user_activity()) return;        // alvó kijelző: a tap csak ébreszt
    if (idx < 0 || idx >= s_bcount) return;
    s_bcursor = idx;
    browser_activate();                    // mappa → belép; fájl → load+play+NOW
}

void player_browser_up(void)
{
    if (ui_user_activity()) return;        // alvó kijelző: a tap csak ébreszt
    browser_up();
}

void player_handle_button(btn_event_t evt)
{
    // Lock tolókapcsoló LOW: minden gombnyomást eldobunk (kivéve magát a
    // lock-eseményt, de az nem button — az slide switch a polling task-on át
    // megy az UI-ra). Az idle timer-t sem nullázzuk, hogy a display-off
    // megtörténjen pocket-ban is.
    if (io_is_locked()) return;

    // Ha sötét volt a kijelző, ez a gombnyomás CSAK ébresztés — a tényleges
    // funkciót (play/next/menu/stb.) nem hajtjuk végre. A felhasználónak még
    // egy gombnyomás kell, hogy érvényesüljön.
    if (ui_user_activity()) return;

    // Game mode (CHIP-8 vagy GB): a fizikai gombokat a játék nyersen
    // pollozza (tartás-állapot kell neki) — itt MINDEN eseményt ELDOBUNK.
    // Kulcs-injektálás ide nem való: az esemény a felengedéskor sül el
    // (SINGLE_CLICK), így a polling által már látott lenyomás UTÁN adna még
    // egy szimulált tartást — duplázott mozgás. A CLI a cli.c-ből injektál.
    // Kilépés: egyelőre a fejléc touch "Exit" gombjával (game.c/gb.c).
    // TODO: fizikai gombos kilépés (gomb-kombó) — lásd a backlog-ot.
    if (game_is_active() || gbmode_is_active()) {
        return;
    }

    audio_status_t st;
    audio_get_status(&st);

    ui_screen_t scr = ui_current_screen();

    switch (evt) {
    case BTN_EVT_A:
        if (scr == UI_SCREEN_SETTINGS) {
            // A Settings oldalon a PLAY semmit nem csinál — ne indítson zenét
            // és ne legyen mentés-funkció sem (az érték állítás auto-save).
            break;
        }
        if (scr == UI_SCREEN_LIBRARY) {
            // Library: fájlon = lejátszás, mappán = belépés.
            browser_activate();
        } else if (st.state == AUDIO_STATE_PLAYING) {
            audio_pause();
            ui_set_state(AUDIO_STATE_PAUSED);
        } else if (st.state == AUDIO_STATE_PAUSED) {
            audio_resume();
            ui_set_state(AUDIO_STATE_PLAYING);
        } else {
            play_current();
        }
        break;

    case BTN_EVT_RIGHT:
    case BTN_EVT_LEFT: {
        if (scr == UI_SCREEN_SETTINGS) {
            // Settings touch-vezérelt (sliderek + tap-cycle sorok) — a
            // Next/Prev gombnak itt nincs funkciója.
            break;
        }
        if (scr == UI_SCREEN_LIBRARY) {
            // Library böngésző: Next = be a mappába, Prev = ki a szülőbe.
            if (evt == BTN_EVT_RIGHT) browser_enter();
            else                     browser_up();
            break;
        }
        bool was_playing = (st.state == AUDIO_STATE_PLAYING);
        bool fwd = (evt == BTN_EVT_RIGHT);
        // "Next album" módban a lista határán a léptetés albumhatáron lép át:
        // Next az utolsó számon → következő album első száma, Prev az elsőn →
        // előző album utolsó száma (a track-vége szabállyal azonos logika).
        // Ha a szabály szerint nincs szomszéd album (albumokon átívelő m3u /
        // gyökér-szintű lista), a léptetés itt NEM értelmezett: no-op, nincs
        // wrap. Stop és Repeat módban marad a listán belüli körbefordulás.
        bool at_edge = fwd ? (s_idx + 1 >= s_count) : (s_idx <= 0);
        if (at_edge && s_count > 0 &&
            ui_get_album_end_mode() == UI_ALBUM_END_NEXT) {
            if (list_is_single_folder()) {
                advance_album(fwd ? +1 : -1, was_playing);
            } else {
                ESP_LOGI(TAG, "lista szélén: több-mappás m3u — next/prev no-op");
            }
            break;
        }
        if (fwd) s_idx++;
        else     s_idx--;
        if (was_playing) {
            // Folyamatos lejátszás: új track azonnal indul.
            play_current();
        } else {
            // Csak navigáció: az esetleg szüneteltetett régi state-et
            // eldobjuk, hogy a Play gomb a friss kiválasztást indítsa,
            // ne az előzőt folytassa.
            audio_stop();
            select_current(false);
        }
        break;
    }

    case BTN_EVT_B:
    case BTN_EVT_X:
    case BTN_EVT_Y:
        // Akciógombok — a player módban (egyelőre) nincs funkciójuk.
        break;

    case BTN_EVT_UP:
    case BTN_EVT_DOWN: {
        int dir = (evt == BTN_EVT_UP) ? +1 : -1;
        if (scr == UI_SCREEN_LIBRARY) {
            browser_move_cursor(-dir);   // VolUp = kurzor fel
            break;
        }
        // Now Playing ÉS Settings: globális hangerő. (A Settings touch-vezérelt:
        // a sliderek a sorokon; a gombok itt is csak a hangerőt állítják. A
        // gombbal NEM perzisztálunk — a hold-repeat sok flash-írása megakasztaná
        // az audiót; a slider release-e menti a hangerőt.)
        uint8_t v = audio_get_volume();
        if (dir > 0) v = (v + 2 > 100) ? 100 : v + 2;
        else         v = (v < 2)       ? 0   : v - 2;
        audio_set_volume(v);
        ui_set_volume(v);
        break;
    }
    }
}

static void on_battery(uint16_t mv, uint8_t pct)
{
    ui_set_battery(mv, pct);
}

// Deep sleep: a Fel (Up) gombról ébredünk (RTC GPIO 17, EXT1 wake = LOW level).
// Boot oldalon (main.c) a wake-after 500 ms-os hold-check validálja.
#define SLEEP_IDLE_MS  (60 * 1000)   // 1 perc tétlenség + nincs lejátszás → sleep

static void enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "deep sleep — wake source UP (GPIO %d)", PIN_BTN_UP);
    esp_sleep_enable_ext1_wakeup_io(1ULL << PIN_BTN_UP, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}

// "Album end: Next album" mód: a játszott album szomszédos testvérmappája
// abc-sorrendben (dir = +1 következő / -1 előző, körbefordul), amiben van
// lejátszható fájl — betölti, és előre lépve az ELSŐ, visszafelé az UTOLSÓ
// trackre áll. Teljes kör után a saját album jön újra (egyetlen albumnál ez
// ismétlésként viselkedik). False, ha nincs jelölt (pl. gyökér-szintű lista).
// autoplay=false (kézi léptetés álló lejátszásnál): csak kijelöl, nem indít —
// mint a sima Next/Prev. Az album mappáját a track útvonalából vesszük, NEM
// az s_bpath-ból: a böngésző közben máshol járhat.
static bool advance_album(int dir, bool autoplay)
{
    if (s_count == 0) return false;

    char album[sizeof(s_bpath)];
    strncpy(album, s_tracks[0].path, sizeof(album) - 1);
    album[sizeof(album) - 1] = 0;
    char *slash = strrchr(album, '/');
    if (!slash) return false;
    *slash = 0;                                   // album mappa útvonala
    const char *alb_name = strrchr(album, '/');
    if (!alb_name || alb_name == album) return false;   // gyökér-szintű lista
    size_t plen = (size_t)(alb_name - album);
    alb_name++;                                   // a mappa neve

    char parent[sizeof(s_bpath)];
    memcpy(parent, album, plen);
    parent[plen] = 0;

    // Saját lista a testvérmappákról — az s_bentries a böngészőé, nem nyúlunk hozzá.
    dir_entry_t *sibs = heap_caps_calloc(MAX_DIR_ENTRIES, sizeof(dir_entry_t),
                                         MALLOC_CAP_SPIRAM);
    if (!sibs) return false;
    int n = sd_list_dir(parent, sibs, MAX_DIR_ENTRIES);

    int cur = -1;
    for (int i = 0; i < n; i++) {
        if (sibs[i].is_dir && strcasecmp(sibs[i].name, alb_name) == 0) { cur = i; break; }
    }

    bool ok = false;
    for (int step = 1; step <= n && !ok; step++) {
        int i = (cur >= 0) ? ((cur + dir * step) % n + n) % n
                           : (dir > 0 ? step - 1 : n - step);
        if (!sibs[i].is_dir) continue;
        char next_path[sizeof(s_bpath)];
        snprintf(next_path, sizeof(next_path), "%.255s/%.127s", parent, sibs[i].name);
        // Üres mappa nem bántja a betöltött listát (0 track = nem ír semmit).
        int cnt = sd_load_dir_tracks(next_path, s_tracks, MAX_TRACKS);
        if (cnt > 0) {
            s_count = cnt;
            s_idx   = (dir > 0) ? 0 : cnt - 1;
            ESP_LOGI(TAG, "%s album: %s (%d track)",
                     dir > 0 ? "Next" : "Prev", next_path, cnt);
            // Mint a kézi léptetésnél: állóban az esetleg paused state-et
            // eldobjuk, hogy a Play a friss kiválasztást indítsa.
            if (!autoplay) audio_stop();
            select_current(autoplay);
            ok = true;
        }
    }
    heap_caps_free(sibs);
    return ok;
}

static void player_task(void *arg)
{
    audio_state_t last_state = AUDIO_STATE_STOPPED;
    uint32_t last_pos_print = 0;
    int64_t  not_playing_since_us = esp_timer_get_time();

    while (1) {
        audio_status_t st;
        audio_get_status(&st);

        // Auto-next ha végzett. A lista végén az "Album end" beállítás dönt.
        if (st.state == AUDIO_STATE_FINISHED && last_state != AUDIO_STATE_FINISHED) {
            if (s_idx + 1 < s_count) {
                s_idx++;
                play_current();
            } else {
                int mode = ui_get_album_end_mode();
                if (mode == UI_ALBUM_END_REPEAT && s_count > 0) {
                    ESP_LOGI(TAG, "End of list — repeat");
                    s_idx = 0;
                    play_current();
                } else if (mode == UI_ALBUM_END_NEXT && list_is_single_folder() &&
                           advance_album(+1, true)) {
                    // advance_album betöltötte és elindította a következő
                    // albumot. (Albumokon átívelő m3u-ra nem értelmezett →
                    // lent stop; az egy-mappás m3u albumként viselkedik.)
                } else {
                    ESP_LOGI(TAG, "End of list (%d tracks), stopping", s_count);
                    audio_stop();
                    ui_set_state(AUDIO_STATE_STOPPED);
                    ui_set_progress(0, 0);
                }
            }
        } else if (st.state == AUDIO_STATE_PLAYING) {
            if (st.position_ms / 500 != last_pos_print / 500) {
                ui_set_progress(st.position_ms, st.duration_ms);
                last_pos_print = st.position_ms;
            }
        }
        last_state = st.state;
        ui_idle_check();   // tétlenség → DISPOFF + BL off

        // Sleep döntés: ha enabled, és nincs lejátszás SLEEP_IDLE_MS óta.
        // Playing alatt a timer folyamatosan resettelődik (sose alszik el).
        // A futó játék is aktivitás — játék közben nem mehetünk deep sleep-be.
        if (st.state == AUDIO_STATE_PLAYING || game_is_active() ||
            gbmode_is_active()) {
            not_playing_since_us = esp_timer_get_time();
        } else if (ui_get_sleep_enabled()) {
            int64_t idle_ms = (esp_timer_get_time() - not_playing_since_us) / 1000;
            if (idle_ms >= SLEEP_IDLE_MS) {
                enter_deep_sleep();   // nem tér vissza
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void on_lock_changed(bool locked)
{
    ui_set_locked(locked);
}

void player_start(void)
{
    s_tracks = heap_caps_calloc(MAX_TRACKS, sizeof(track_t), MALLOC_CAP_SPIRAM);
    s_bentries = heap_caps_calloc(MAX_DIR_ENTRIES, sizeof(dir_entry_t), MALLOC_CAP_SPIRAM);
    if (!s_tracks || !s_bentries) {
        ESP_LOGE(TAG, "buffer alloc failed");
        return;
    }
    s_count = 0;
    s_idx   = 0;

    io_register_button_cb(player_handle_button);
    io_register_battery_cb(on_battery);
    io_register_lock_cb(on_lock_changed);
    ui_set_locked(io_is_locked());   // induló állapot a header ikonon

    // UI alapállapot
    int32_t saved_vol = 70;
    if (persist_get_i32("volume", &saved_vol)) {
        if (saved_vol < 0 || saved_vol > 100) saved_vol = 70;
    }
    audio_set_volume((uint8_t)saved_vol);
    ui_set_volume((uint8_t)saved_vol);
    uint16_t mv = io_read_battery_mv();
    ui_set_battery(mv, io_battery_percent_from_mv(mv));

    // Idle-timeout visszaolvasása NVS-ből (default 30 s ha még nem volt mentve).
    int32_t saved_idle = 30;
    if (persist_get_i32("idle_s", &saved_idle)) {
        // Csak elfogadott értékek (10/15/30/0). Bármi más → 30.
        if (saved_idle != 10 && saved_idle != 15 && saved_idle != 30 && saved_idle != 0) {
            saved_idle = 30;
        }
    }
    ui_set_idle_timeout_s(saved_idle);

    // Sleep enabled visszaolvasása (default 0 = kikapcsolva).
    int32_t saved_sleep_en = 0;
    persist_get_i32("sleep_en", &saved_sleep_en);
    ui_set_sleep_enabled(saved_sleep_en != 0);

    // Album végi viselkedés visszaolvasása (default Stop).
    int32_t saved_alb_end = UI_ALBUM_END_STOP;
    if (persist_get_i32("alb_end", &saved_alb_end)) {
        if (saved_alb_end < 0 || saved_alb_end >= UI_ALBUM_END_COUNT) {
            saved_alb_end = UI_ALBUM_END_STOP;
        }
    }
    ui_set_album_end_mode(saved_alb_end);

    // Háttérvilágítás fényerő visszaolvasása (default 100%). Csak beállítjuk
    // (nem mentjük újra); a tényleges felkapcsolás az ui_display_ready-ben.
    int32_t saved_bl = 100;
    if (persist_get_i32("bl_pct", &saved_bl)) {
        if (saved_bl < 0 || saved_bl > 100) saved_bl = 100;
    }
    ui_set_backlight((uint8_t)saved_bl);

    // Böngésző: ha van mentett könyvtár, még létezik ÉS a MUSIC_DIR alatt
    // van (a gyökér-váltás előtti mentések kieshetnek), oda térünk vissza,
    // különben a music mappából indulunk.
    strcpy(s_bpath, MUSIC_DIR);
    char saved_dir[sizeof(s_bpath)];
    if (persist_get_str("br_dir", saved_dir, sizeof(saved_dir)) &&
        strncmp(saved_dir, MUSIC_DIR, strlen(MUSIC_DIR)) == 0) {
        DIR *d = opendir(saved_dir);
        if (d) { closedir(d); strcpy(s_bpath, saved_dir); }
    }
    s_bcursor = 0;
    browser_refresh();

    // Utoljára nyitott fájl visszaállítása (ha még létezik): betöltjük a
    // mappáját lejátszási listának és a Now Playing-en mutatjuk — de NEM
    // indítjuk el automatikusan.
    bool restored = false;
    char last_file[384];
    if (persist_get_str("last_file", last_file, sizeof(last_file)) && last_file[0]) {
        struct stat stt;
        if (stat(last_file, &stt) == 0) {
            char folder[384];
            strncpy(folder, last_file, sizeof(folder) - 1);
            folder[sizeof(folder) - 1] = 0;
            char *slash = strrchr(folder, '/');
            if (slash) *slash = 0;
            s_count = sd_load_dir_tracks(folder, s_tracks, MAX_TRACKS);
            for (int i = 0; i < s_count; i++) {
                if (strcmp(s_tracks[i].path, last_file) == 0) {
                    s_idx = i;
                    select_current(false);   // mutat, de nem játszik
                    restored = true;
                    break;
                }
            }
        }
    }

    // Ha nincs mit visszaállítani (nincs SD / törölt fájl), üres Now Playing.
    if (!restored) {
        ui_show_no_track();
        ui_set_state(AUDIO_STATE_STOPPED);
    }

    xTaskCreate(player_task, "player", 4096, NULL, 4, NULL);
}
