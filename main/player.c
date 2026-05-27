#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "app_config.h"
#include "sd.h"
#include "audio.h"
#include "ui.h"
#include "io.h"
#include "player.h"

static const char *TAG = "player";

static track_t *s_tracks = NULL;
static int      s_count  = 0;
static int      s_idx    = 0;

static void select_current(bool autoplay)
{
    if (s_count == 0) {
        ui_show_no_track();
        return;
    }
    if (s_idx < 0) s_idx = s_count - 1;
    if (s_idx >= s_count) s_idx = 0;

    ui_show_track(&s_tracks[s_idx]);
    ui_set_playlist(s_tracks, s_count, s_idx);
    if (autoplay) {
        ui_set_playing(true);
        audio_play(s_tracks[s_idx].path);
    } else {
        ui_set_playing(false);
    }
}

static void play_current(void)
{
    select_current(true);
}

void player_handle_button(btn_event_t evt)
{
    audio_status_t st;
    audio_get_status(&st);

    switch (evt) {
    case BTN_EVT_PLAY_PAUSE:
        if (st.state == AUDIO_STATE_PLAYING) {
            audio_pause();
            ui_set_playing(false);
        } else if (st.state == AUDIO_STATE_PAUSED) {
            audio_resume();
            ui_set_playing(true);
        } else {
            play_current();
        }
        break;
    case BTN_EVT_NEXT:
    case BTN_EVT_PREV: {
        bool was_playing = (st.state == AUDIO_STATE_PLAYING);
        if (evt == BTN_EVT_NEXT) s_idx++;
        else                     s_idx--;
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
    case BTN_EVT_MENU:
        // Most: re-scan + play
        s_count = sd_scan_tracks(s_tracks, MAX_TRACKS);
        s_idx = 0;
        play_current();
        break;
    case BTN_EVT_VOL_UP: {
        uint8_t v = audio_get_volume();
        v = (v + 2 > 100) ? 100 : v + 2;
        audio_set_volume(v);
        ui_set_volume(v);
        break;
    }
    case BTN_EVT_VOL_DOWN: {
        uint8_t v = audio_get_volume();
        v = (v < 2) ? 0 : v - 2;
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

static void player_task(void *arg)
{
    audio_state_t last_state = AUDIO_STATE_STOPPED;
    uint32_t last_pos_print = 0;

    while (1) {
        audio_status_t st;
        audio_get_status(&st);

        // Auto-next ha végzett
        if (st.state == AUDIO_STATE_FINISHED && last_state != AUDIO_STATE_FINISHED) {
            s_idx++;
            play_current();
        } else if (st.state == AUDIO_STATE_PLAYING) {
            if (st.position_ms / 500 != last_pos_print / 500) {
                ui_set_progress(st.position_ms, st.duration_ms);
                last_pos_print = st.position_ms;
            }
        }
        last_state = st.state;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void player_start(void)
{
    s_tracks = heap_caps_calloc(MAX_TRACKS, sizeof(track_t), MALLOC_CAP_SPIRAM);
    if (!s_tracks) {
        ESP_LOGE(TAG, "track buffer alloc failed");
        return;
    }
    s_count = sd_scan_tracks(s_tracks, MAX_TRACKS);
    s_idx   = 0;

    io_register_button_cb(player_handle_button);
    io_register_battery_cb(on_battery);

    // UI alapállapot
    audio_set_volume(70);
    ui_set_volume(70);
    uint16_t mv = io_read_battery_mv();
    ui_set_battery(mv, io_battery_percent_from_mv(mv));

    if (s_count > 0) {
        ui_show_track(&s_tracks[0]);
        ui_set_playlist(s_tracks, s_count, 0);
        ui_set_playing(false);
    } else {
        ui_show_no_track();
    }

    xTaskCreate(player_task, "player", 4096, NULL, 4, NULL);
}
