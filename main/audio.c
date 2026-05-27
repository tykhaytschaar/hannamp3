#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"

#include "mp3dec.h"   // libhelix-mp3

#include "app_config.h"
#include "audio.h"
#include "ui.h"           // ui_spi_lock/unlock — közös SPI busz szerializációhoz

static const char *TAG = "audio";

typedef enum {
    CMD_NONE,
    CMD_PLAY,
    CMD_STOP,
    CMD_PAUSE,
    CMD_RESUME,
} audio_cmd_t;

typedef struct {
    audio_cmd_t cmd;
    char        path[MAX_PATH_LEN];
} audio_msg_t;

static QueueHandle_t    s_cmd_q;
static SemaphoreHandle_t s_status_mux;
static audio_status_t   s_status = { .volume = 70 };

static i2s_chan_handle_t s_tx_chan = NULL;
static uint32_t          s_cur_sample_rate = 0;
static bool              s_i2s_enabled = false;

// Az I2S csatorna ki/be kapcsolása. Pause/stop-nál letiltjuk, hogy a DMA
// ne ismételje az utolsó mintát (kattogás), resume/play-nél visszakapcsoljuk.
static void i2s_set_enabled(bool en)
{
    if (!s_tx_chan) return;
    if (en && !s_i2s_enabled) {
        i2s_channel_enable(s_tx_chan);
        s_i2s_enabled = true;
    } else if (!en && s_i2s_enabled) {
        i2s_channel_disable(s_tx_chan);
        s_i2s_enabled = false;
    }
}

// Quadratic (vol^2) taper: az emberi fül logaritmikusan érzékel, a négyzetes
// görbe ezt jól közelíti. Példák:
//   vol=100 → gain 1.00 (0 dB)
//   vol=70  → gain 0.49 (~ -6 dB)
//   vol=50  → gain 0.25 (~ -12 dB)
//   vol=25  → gain 0.06 (~ -24 dB)
//   vol=10  → gain 0.01 (~ -40 dB)
//   vol=2   → gain 0.0004 (~ -68 dB) — alig hallható, nem berobban a hang
static void apply_volume(int16_t *pcm, int samples, uint8_t vol)
{
    if (vol >= 100) return;
    // gain_q15 = (vol^2 / 100) * 32768 / 100 = vol * vol * 32768 / 10000
    int32_t gain_q15 = (int32_t)vol * vol * 32768 / 10000;
    for (int i = 0; i < samples; i++) {
        pcm[i] = (int16_t)(((int32_t)pcm[i] * gain_q15) >> 15);
    }
}

static esp_err_t setup_i2s(uint32_t sample_rate)
{
    if (s_tx_chan && s_cur_sample_rate == sample_rate) return ESP_OK;

    if (s_tx_chan) {
        i2s_set_enabled(false);
        i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
        ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(s_tx_chan, &clk));
        i2s_set_enabled(true);
        s_cur_sample_rate = sample_rate;
        return ESP_OK;
    }

    i2s_chan_config_t ch_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // ~93 ms buffer (8 × 512 frame @ 44.1k) — tartalék a display-frissítések
    // alatti SPI-busz blokkolásra, hogy ne legyen audio underrun.
    ch_cfg.dma_desc_num   = 8;
    ch_cfg.dma_frame_num  = 512;
    ESP_ERROR_CHECK(i2s_new_channel(&ch_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCK,
            .ws   = PIN_I2S_LRCK,
            .dout = PIN_I2S_DIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { 0 },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std));
    i2s_channel_enable(s_tx_chan);
    s_i2s_enabled = true;
    s_cur_sample_rate = sample_rate;
    return ESP_OK;
}

static void status_set(audio_state_t st)
{
    xSemaphoreTake(s_status_mux, portMAX_DELAY);
    s_status.state = st;
    xSemaphoreGive(s_status_mux);
}

static void status_update(uint32_t pos, uint32_t dur, uint32_t sr, uint8_t ch)
{
    xSemaphoreTake(s_status_mux, portMAX_DELAY);
    s_status.position_ms = pos;
    s_status.duration_ms = dur;
    s_status.sample_rate = sr;
    s_status.channels    = ch;
    xSemaphoreGive(s_status_mux);
}

static void audio_task(void *arg)
{
    HMP3Decoder hMP3 = MP3InitDecoder();
    assert(hMP3);

    // Bemeneti buffer (file → ringszerű előtöltés) + kimeneti PCM buffer.
    uint8_t *inbuf  = heap_caps_malloc(MP3_READ_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *outbuf = heap_caps_malloc(MP3_OUT_BUF_BYTES, MALLOC_CAP_DEFAULT);
    assert(inbuf && outbuf);

    FILE *fp = NULL;
    int   in_bytes = 0;        // bemeneti bufferben lévő érvényes byte
    uint8_t *in_ptr = inbuf;
    bool  playing = false;
    uint32_t total_samples = 0;
    uint32_t file_size = 0;
    uint32_t bitrate_kbps = 128;

    audio_msg_t msg;

    while (1) {
        // Parancs lekérdezés: ha nem szól semmi, blokkolva várunk, különben polling.
        TickType_t wait = playing ? 0 : portMAX_DELAY;
        if (xQueueReceive(s_cmd_q, &msg, wait) == pdTRUE) {
            switch (msg.cmd) {
            case CMD_PLAY:
                ui_spi_lock();
                if (fp) { fclose(fp); fp = NULL; }
                fp = fopen(msg.path, "rb");
                if (fp) {
                    fseek(fp, 0, SEEK_END);
                    file_size = ftell(fp);
                    fseek(fp, 0, SEEK_SET);
                    // ID3v2 tag a fájl elején? Ugorjuk át — a Helix nem érti.
                    uint8_t id3[10];
                    if (fread(id3, 1, 10, fp) == 10 &&
                        id3[0] == 'I' && id3[1] == 'D' && id3[2] == '3') {
                        // synchsafe 28-bit méret a 6..9 byte-on
                        uint32_t tag_size = ((id3[6] & 0x7F) << 21) |
                                            ((id3[7] & 0x7F) << 14) |
                                            ((id3[8] & 0x7F) << 7)  |
                                             (id3[9] & 0x7F);
                        if (id3[5] & 0x10) tag_size += 10;   // footer flag
                        fseek(fp, tag_size, SEEK_CUR);
                        ESP_LOGI(TAG, "Skipped ID3v2 tag (%lu bytes)",
                                 (unsigned long)(tag_size + 10));
                    } else {
                        fseek(fp, 0, SEEK_SET);              // nincs ID3, vissza az elejére
                    }
                }
                ui_spi_unlock();
                if (!fp) {
                    ESP_LOGE(TAG, "fopen %s failed", msg.path);
                    status_set(AUDIO_STATE_STOPPED);
                    break;
                }
                in_bytes = 0;
                in_ptr   = inbuf;
                total_samples = 0;
                i2s_set_enabled(true);
                playing = true;
                status_set(AUDIO_STATE_PLAYING);
                ESP_LOGI(TAG, "Play %s (%lu B)", msg.path, (unsigned long)file_size);
                break;
            case CMD_STOP:
                if (fp) { fclose(fp); fp = NULL; }
                playing = false;
                i2s_set_enabled(false);   // ne ismételje a DMA az utolsó mintát
                status_set(AUDIO_STATE_STOPPED);
                break;
            case CMD_PAUSE:
                playing = false;
                i2s_set_enabled(false);   // némítás, kattogás-mentes
                status_set(AUDIO_STATE_PAUSED);
                break;
            case CMD_RESUME:
                if (fp) {
                    i2s_set_enabled(true);
                    playing = true;
                    status_set(AUDIO_STATE_PLAYING);
                }
                break;
            default: break;
            }
        }

        if (!playing) continue;

        // Buffer feltöltése, ha kevés van benne.
        if (in_bytes < 2 * 1024) {
            if (in_ptr != inbuf && in_bytes > 0) {
                memmove(inbuf, in_ptr, in_bytes);
            }
            in_ptr = inbuf;
            int want = MP3_READ_BUF_SIZE - in_bytes;
            // SD + LCD közös SPI buszon — lockoljuk az LVGL-t a fread idejére,
            // különben a SPI HAL panikol a párhuzamos hozzáférés miatt.
            ui_spi_lock();
            int got  = fread(inbuf + in_bytes, 1, want, fp);
            ui_spi_unlock();
            in_bytes += got;
            if (got == 0 && in_bytes == 0) {
                // EOF
                fclose(fp); fp = NULL;
                playing = false;
                i2s_set_enabled(false);
                status_set(AUDIO_STATE_FINISHED);
                continue;
            }
        }

        int offset = MP3FindSyncWord(in_ptr, in_bytes);
        if (offset < 0) {
            // nincs sync — eldobjuk és újratöltünk
            in_bytes = 0;
            continue;
        }
        in_ptr   += offset;
        in_bytes -= offset;

        int bytes_left = in_bytes;
        int res = MP3Decode(hMP3, &in_ptr, &bytes_left, outbuf, 0);
        int consumed = in_bytes - bytes_left;
        in_bytes = bytes_left;

        if (res != ERR_MP3_NONE) {
            ESP_LOGW(TAG, "MP3Decode err=%d, skip", res);
            // egy byte-tal előrébb és újrapróbáljuk
            if (in_bytes > 0) { in_ptr++; in_bytes--; }
            continue;
        }

        MP3FrameInfo info;
        MP3GetLastFrameInfo(hMP3, &info);

        setup_i2s(info.samprate);

        int samples = info.outputSamps;   // L+R interleaved
        // Ha mono jönne, duplázunk stereo-vá (PCM5102 mindig stereo).
        if (info.nChans == 1) {
            // outputSamps mono frame-nél a mintaszám 1 csatornán → duplázunk helyben
            for (int i = samples - 1; i >= 0; i--) {
                outbuf[2 * i]     = outbuf[i];
                outbuf[2 * i + 1] = outbuf[i];
            }
            samples *= 2;
        }

        uint8_t vol = audio_get_volume();
        apply_volume(outbuf, samples, vol);

        size_t bw = 0;
        i2s_channel_write(s_tx_chan, outbuf, samples * sizeof(int16_t), &bw, portMAX_DELAY);

        // pozíció becslés
        total_samples += samples / 2;   // per channel
        uint32_t pos_ms = (uint64_t)total_samples * 1000 / info.samprate;
        if (info.bitrate > 0) bitrate_kbps = info.bitrate / 1000;
        uint32_t dur_ms = bitrate_kbps ? (file_size * 8 / bitrate_kbps) : 0;
        status_update(pos_ms, dur_ms, info.samprate, info.nChans);
    }
}

void audio_init(void)
{
    s_cmd_q = xQueueCreate(4, sizeof(audio_msg_t));
    s_status_mux = xSemaphoreCreateMutex();

    // Indulásra default I2S 44.1k — első keret után úgyis újrahangoljuk.
    setup_i2s(44100);

    // Core 1, magas prio: glitch-mentes audio
    xTaskCreatePinnedToCore(audio_task, "audio", 8192, NULL, 10, NULL, 1);
}

void audio_play(const char *path)
{
    audio_msg_t m = { .cmd = CMD_PLAY };
    strncpy(m.path, path, sizeof(m.path) - 1);
    xQueueSend(s_cmd_q, &m, 0);
}
void audio_stop(void)   { audio_msg_t m = { .cmd = CMD_STOP   }; xQueueSend(s_cmd_q, &m, 0); }
void audio_pause(void)  { audio_msg_t m = { .cmd = CMD_PAUSE  }; xQueueSend(s_cmd_q, &m, 0); }
void audio_resume(void) { audio_msg_t m = { .cmd = CMD_RESUME }; xQueueSend(s_cmd_q, &m, 0); }

void audio_set_volume(uint8_t v) {
    if (v > 100) v = 100;
    xSemaphoreTake(s_status_mux, portMAX_DELAY);
    s_status.volume = v;
    xSemaphoreGive(s_status_mux);
}
uint8_t audio_get_volume(void) {
    xSemaphoreTake(s_status_mux, portMAX_DELAY);
    uint8_t v = s_status.volume;
    xSemaphoreGive(s_status_mux);
    return v;
}

void audio_get_status(audio_status_t *out) {
    xSemaphoreTake(s_status_mux, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_status_mux);
}
