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
#include "driver/gpio.h"

#include "mp3dec.h"      // libhelix-mp3 publikus API
#include "mp3common.h"   // MP3DecInfo struktúra (sub-struct pointerek + mainBuf)
#include "coder.h"       // FrameHeader / SideInfo / IMDCTInfo / SubbandInfo sizeof-ok

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
// CMD_PLAY mute-olja a DAC-ot (XSMT=LOW). Az unmute (XSMT=HIGH) az első
// sikeres i2s_channel_write után történik, hogy a DAC már stabil mintákat
// kapjon a fade-in alatt.
static volatile bool     s_pending_unmute = false;

// Szoftveres fade-in: a CMD_PLAY után az első N sample-en lineáris
// 0 → 1 gain rámpa. Ezzel az XSMT unmute pillanata (~2.4 ms DAC belső
// rámpa) sosem esik nagy amplitúdójú audiora — a digitális szint pont
// 0 közelében van, így a két rámpa simán összesimul.
//   100 ms @ 44.1k = 4410 sample / csatorna. Más sample rate-en kicsit
//   más időhossz, de a nagyságrend marad.
static int s_fade_in_remaining = 0;
static int s_fade_in_total     = 0;
#define FADE_IN_SAMPLES_PER_CHAN  (100 * 44100 / 1000)

// Fájlformátum
typedef enum {
    FMT_MP3,
    FMT_WAV,
} audio_format_t;

static audio_format_t s_format         = FMT_MP3;
// WAV state (csak FMT_WAV-on használt)
static uint32_t       s_wav_sample_rate = 44100;
static int            s_wav_channels    = 2;
static int            s_wav_bits        = 16;
static uint32_t       s_wav_data_total  = 0;
static uint32_t       s_wav_data_played = 0;

// MP3 resilience: ha N egymás utáni decode/sync fail után sincs jó frame,
// feladjuk és STOPPED-ba kerülünk (különben sosem érne véget a tight loop).
static int s_mp3_fail_streak = 0;
#define MP3_FAIL_STREAK_MAX  64

static inline void xsmt_mute(void)   { gpio_set_level(PIN_XSMT, 0); }
static inline void xsmt_unmute(void) { gpio_set_level(PIN_XSMT, 1); }

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

// Helix MP3 dekóder állapot reset alloc/free nélkül.
// MP3InitDecoder az AllocateBuffers-ben memset-eli a MP3DecInfo-t ÉS minden
// sub-struktúrát — mi pontosan ugyanezt csináljuk, csak a meglévő bufferek
// pointereit megőrizve, hogy ne fragmentáljuk a heap-et 8 db malloc/free-vel.
//
// Miért kell: a Helix dekóder belső állapotot (IMDCT overlap-add delay line,
// polyphase subband filterbank delay, mainBuf bit reservoir) tart fenn track-
// ek között. Új trackre váltáskor az első ~1 frame audiója az ELŐZŐ track
// állapotát is "magával hozza" — pont ez a "kicsi ismétlés a régi végéből".
static void mp3_reset_state(HMP3Decoder hMP3)
{
    if (!hMP3) return;
    MP3DecInfo *d = (MP3DecInfo *)hMP3;

    // Sub-struct pointereket lementjük; a MP3DecInfo memset után visszaírjuk.
    void *fh  = d->FrameHeaderPS;
    void *si  = d->SideInfoPS;
    void *sfi = d->ScaleFactorInfoPS;
    void *hi  = d->HuffmanInfoPS;
    void *di  = d->DequantInfoPS;
    void *mi  = d->IMDCTInfoPS;
    void *sbi = d->SubbandInfoPS;

    memset(d, 0, sizeof(MP3DecInfo));   // mainBuf + összes scalar állapot

    d->FrameHeaderPS     = fh;
    d->SideInfoPS        = si;
    d->ScaleFactorInfoPS = sfi;
    d->HuffmanInfoPS     = hi;
    d->DequantInfoPS     = di;
    d->IMDCTInfoPS       = mi;
    d->SubbandInfoPS     = sbi;

    // Sub-structok tartalmát kinullázzuk — IMDCT/Subband delay line itt él.
    memset(fh,  0, sizeof(FrameHeader));
    memset(si,  0, sizeof(SideInfo));
    memset(sfi, 0, sizeof(ScaleFactorInfo));
    memset(hi,  0, sizeof(HuffmanInfo));
    memset(di,  0, sizeof(DequantInfo));
    memset(mi,  0, sizeof(IMDCTInfo));
    memset(sbi, 0, sizeof(SubbandInfo));
}

// Track-váltáskor: teljes I2S csatorna teardown.
// Csak így garantálható, hogy a DMA descriptor-ok régi PCM tartalma eltűnjön.
// (i2s_channel_preload_data NEM nullázza ki az összes descriptort: a belső
// curr_ptr/rw_pos állapotból folytatja, és a wrap-around előtt megáll, így
// a már beírt régi descriptorok érintetlenek maradnak.)
static void i2s_destroy(void)
{
    if (!s_tx_chan) return;
    i2s_set_enabled(false);
    i2s_del_channel(s_tx_chan);
    s_tx_chan = NULL;
    s_cur_sample_rate = 0;
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
    if (s_tx_chan && s_cur_sample_rate == sample_rate) {
        // STOP után a csatorna létezik és a rate stimmel, de DISABLED — re-enable.
        i2s_set_enabled(true);
        return ESP_OK;
    }

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
                // Track-váltás takarítás:
                //  0) DAC mute (XSMT=LOW), 5 ms várakozás a belső rámpára.
                //  1) Helix dekóder state reset (IMDCT/Subband/reservoir).
                //  2) I2S csatorna teardown — szükséges, mert csak így
                //     tisztul ki a DMA descriptor-tartalom. (BCLK kis pop-ot
                //     ad a recreate-nél, de ezt a 100 ms-os mute lefedi.)
                xsmt_mute();
                vTaskDelay(pdMS_TO_TICKS(5));
                mp3_reset_state(hMP3);
                i2s_destroy();
                ui_spi_lock();
                if (fp) { fclose(fp); fp = NULL; }
                fp = fopen(msg.path, "rb");
                if (fp) {
                    fseek(fp, 0, SEEK_END);
                    file_size = ftell(fp);
                    fseek(fp, 0, SEEK_SET);

                    // Formátum-detekció a fájl elejéről: RIFF...WAVE → WAV,
                    // ID3 vagy MP3 sync → MP3. Egyébként reject.
                    uint8_t hdr[12];
                    int got = fread(hdr, 1, 12, fp);
                    s_format = FMT_MP3;   // default; lent felülírjuk ha WAV
                    if (got == 12 && memcmp(hdr, "RIFF", 4) == 0 &&
                        memcmp(hdr + 8, "WAVE", 4) == 0) {
                        // WAV: chunk-okat olvasunk, amíg fmt + data megvan.
                        s_format = FMT_WAV;
                        s_wav_data_total = 0;
                        s_wav_data_played = 0;
                        bool fmt_ok = false, data_ok = false, fmt_unsupported = false;
                        while (!fmt_unsupported && !(fmt_ok && data_ok)) {
                            uint8_t ck[8];
                            if (fread(ck, 1, 8, fp) != 8) break;
                            uint32_t sz = ck[4] | (ck[5]<<8) | (ck[6]<<16) | (ck[7]<<24);
                            if (memcmp(ck, "fmt ", 4) == 0) {
                                uint8_t f[16];
                                int fg = fread(f, 1, sz > 16 ? 16 : sz, fp);
                                if (fg >= 16) {
                                    int format = f[0] | (f[1] << 8);
                                    s_wav_channels    = f[2] | (f[3] << 8);
                                    s_wav_sample_rate = f[4] | (f[5]<<8) | (f[6]<<16) | (f[7]<<24);
                                    s_wav_bits        = f[14] | (f[15] << 8);
                                    if (format != 1 || s_wav_bits != 16 ||
                                        (s_wav_channels != 1 && s_wav_channels != 2)) {
                                        ESP_LOGE(TAG, "WAV unsupported: fmt=%d, %d-bit, %d ch (csak PCM 16-bit mono/stereo)",
                                                 format, s_wav_bits, s_wav_channels);
                                        fmt_unsupported = true;
                                    } else {
                                        fmt_ok = true;
                                    }
                                    if (sz > 16) fseek(fp, sz - 16, SEEK_CUR);
                                }
                            } else if (memcmp(ck, "data", 4) == 0) {
                                s_wav_data_total = sz;
                                data_ok = true;
                                break;   // fájlpozíció a PCM-adat elején
                            } else {
                                fseek(fp, sz, SEEK_CUR);   // egyéb chunk-okat átugorjuk
                            }
                        }
                        if (!fmt_ok || !data_ok) {
                            ESP_LOGE(TAG, "Invalid/unsupported WAV (fmt=%d data=%d)",
                                     fmt_ok, data_ok);
                            fclose(fp); fp = NULL;
                        } else {
                            ESP_LOGI(TAG, "Play WAV %s (%lu B data, %lu Hz %d ch %d-bit)",
                                     msg.path, (unsigned long)s_wav_data_total,
                                     (unsigned long)s_wav_sample_rate,
                                     s_wav_channels, s_wav_bits);
                        }
                    } else {
                        // MP3: ID3v2 tag a fájl elején? Ugorjuk át.
                        fseek(fp, 0, SEEK_SET);
                        uint8_t id3[10];
                        if (fread(id3, 1, 10, fp) == 10 &&
                            id3[0] == 'I' && id3[1] == 'D' && id3[2] == '3') {
                            uint32_t tag_size = ((id3[6] & 0x7F) << 21) |
                                                ((id3[7] & 0x7F) << 14) |
                                                ((id3[8] & 0x7F) << 7)  |
                                                 (id3[9] & 0x7F);
                            if (id3[5] & 0x10) tag_size += 10;
                            fseek(fp, tag_size, SEEK_CUR);
                            ESP_LOGI(TAG, "Skipped ID3v2 tag (%lu bytes)",
                                     (unsigned long)(tag_size + 10));
                        } else {
                            fseek(fp, 0, SEEK_SET);
                        }
                        ESP_LOGI(TAG, "Play MP3 %s (%lu B)", msg.path,
                                 (unsigned long)file_size);
                    }
                }
                ui_spi_unlock();
                if (!fp) {
                    ESP_LOGE(TAG, "fopen/parse %s failed", msg.path);
                    status_set(AUDIO_STATE_STOPPED);
                    break;
                }
                in_bytes = 0;
                in_ptr   = inbuf;
                total_samples = 0;
                s_mp3_fail_streak = 0;
                // I2S enable nem itt — setup_i2s() építi újra az első frame után.
                playing = true;
                s_pending_unmute = true;   // unmute az első új i2s_write után
                s_fade_in_total = FADE_IN_SAMPLES_PER_CHAN;
                s_fade_in_remaining = s_fade_in_total;
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

        int samples = 0;   // L+R interleaved sample-count az outbuf-ban
        uint32_t cur_samprate = 44100;
        uint8_t  cur_chans    = 2;
        uint32_t pos_ms = 0, dur_ms = 0;

        if (s_format == FMT_WAV) {
            // WAV: a fájlpozíció pont a PCM data chunk elején van a CMD_PLAY után.
            // Outbufba olvasunk legfeljebb annyit amennyi még a data chunk-ban van.
            uint32_t remain = (s_wav_data_total > s_wav_data_played)
                              ? (s_wav_data_total - s_wav_data_played) : 0;
            int want = MP3_OUT_BUF_BYTES;
            if (s_wav_channels == 1) want /= 2;   // mono után duplázunk → fél akkora kell
            if ((uint32_t)want > remain) want = remain;
            if (want == 0) {
                fclose(fp); fp = NULL;
                playing = false;
                i2s_set_enabled(false);
                status_set(AUDIO_STATE_FINISHED);
                continue;
            }
            ui_spi_lock();
            int got = fread(outbuf, 1, want, fp);
            ui_spi_unlock();
            if (got <= 0) {
                fclose(fp); fp = NULL;
                playing = false;
                i2s_set_enabled(false);
                status_set(AUDIO_STATE_FINISHED);
                continue;
            }
            s_wav_data_played += got;
            samples = got / 2;   // int16 / sample
            if (s_wav_channels == 1) {
                // Mono → stereo helyben, hátulról előre.
                for (int i = samples - 1; i >= 0; i--) {
                    outbuf[2*i]     = outbuf[i];
                    outbuf[2*i + 1] = outbuf[i];
                }
                samples *= 2;
            }
            cur_samprate = s_wav_sample_rate;
            cur_chans    = s_wav_channels;
            setup_i2s(cur_samprate);

            // Pozíció: data-bytes / (channels * 2) = samples / channel.
            uint32_t played_pc = s_wav_data_played / (s_wav_channels * 2);
            uint32_t total_pc  = s_wav_data_total  / (s_wav_channels * 2);
            pos_ms = (uint64_t)played_pc * 1000 / s_wav_sample_rate;
            dur_ms = (uint64_t)total_pc  * 1000 / s_wav_sample_rate;
        } else {
            // MP3: a Helix dekódolóra megyünk a buffer-bemenettel.
            if (in_bytes < 2 * 1024) {
                if (in_ptr != inbuf && in_bytes > 0) {
                    memmove(inbuf, in_ptr, in_bytes);
                }
                in_ptr = inbuf;
                int want = MP3_READ_BUF_SIZE - in_bytes;
                ui_spi_lock();
                int got  = fread(inbuf + in_bytes, 1, want, fp);
                ui_spi_unlock();
                in_bytes += got;
                if (got == 0 && in_bytes == 0) {
                    fclose(fp); fp = NULL;
                    playing = false;
                    i2s_set_enabled(false);
                    status_set(AUDIO_STATE_FINISHED);
                    continue;
                }
            }

            int offset = MP3FindSyncWord(in_ptr, in_bytes);
            if (offset < 0) {
                in_bytes = 0;
                if (++s_mp3_fail_streak > MP3_FAIL_STREAK_MAX) {
                    ESP_LOGE(TAG, "MP3 sync nem található %d próbálkozás után — abort",
                             s_mp3_fail_streak);
                    fclose(fp); fp = NULL;
                    playing = false;
                    i2s_set_enabled(false);
                    status_set(AUDIO_STATE_STOPPED);
                }
                continue;
            }
            in_ptr   += offset;
            in_bytes -= offset;

            int bytes_left = in_bytes;
            int res = MP3Decode(hMP3, &in_ptr, &bytes_left, outbuf, 0);
            in_bytes = bytes_left;

            if (res != ERR_MP3_NONE) {
                if (++s_mp3_fail_streak > MP3_FAIL_STREAK_MAX) {
                    ESP_LOGE(TAG, "MP3Decode %d egymás utáni hiba (utolsó err=%d) — abort",
                             s_mp3_fail_streak, res);
                    fclose(fp); fp = NULL;
                    playing = false;
                    i2s_set_enabled(false);
                    status_set(AUDIO_STATE_STOPPED);
                } else {
                    ESP_LOGW(TAG, "MP3Decode err=%d, skip", res);
                    if (in_bytes > 0) { in_ptr++; in_bytes--; }
                }
                continue;
            }
            s_mp3_fail_streak = 0;

            MP3FrameInfo info;
            MP3GetLastFrameInfo(hMP3, &info);
            setup_i2s(info.samprate);

            samples = info.outputSamps;
            if (info.nChans == 1) {
                for (int i = samples - 1; i >= 0; i--) {
                    outbuf[2 * i]     = outbuf[i];
                    outbuf[2 * i + 1] = outbuf[i];
                }
                samples *= 2;
            }
            cur_samprate = info.samprate;
            cur_chans    = info.nChans;
            total_samples += samples / 2;
            pos_ms = (uint64_t)total_samples * 1000 / info.samprate;
            if (info.bitrate > 0) bitrate_kbps = info.bitrate / 1000;
            dur_ms = bitrate_kbps ? (file_size * 8 / bitrate_kbps) : 0;
        }

        uint8_t vol = audio_get_volume();
        apply_volume(outbuf, samples, vol);

        // Szoftveres fade-in az új track első ~50 ms-én. Lineáris gain rámpa
        // 0 → 1 sample-szinten. Ezzel a XSMT unmute pillanat sosem esik
        // nagy digitális amplitúdójú audiora, a "pukk" eltűnik.
        if (s_fade_in_remaining > 0) {
            int n_per_chan = samples / 2;   // outbuf L+R interleaved
            for (int i = 0; i < n_per_chan && s_fade_in_remaining > 0; i++) {
                int32_t gain_q15 = (int32_t)(s_fade_in_total - s_fade_in_remaining)
                                   * 32768 / s_fade_in_total;
                outbuf[2*i    ] = (int16_t)(((int32_t)outbuf[2*i    ] * gain_q15) >> 15);
                outbuf[2*i + 1] = (int16_t)(((int32_t)outbuf[2*i + 1] * gain_q15) >> 15);
                s_fade_in_remaining--;
            }
        }

        size_t bw = 0;
        i2s_channel_write(s_tx_chan, outbuf, samples * sizeof(int16_t), &bw, portMAX_DELAY);

        // Az első új write után 100 ms várakozás unmute előtt:
        //   - track-váltáskor ez alatt fut ki a régi DMA-tartalom a DAC-on
        //     (8 descriptor × 11.6 ms ≈ 93 ms @ 44.1k, mind muted)
        //   - a Helix filterbank felfutása (~26 ms) szintén lefedve
        //   - az új audio fade-in-je (100 ms) felében jár, így az unmute
        //     pillanata közepes, simán változó digitális amplitúdóra esik
        if (s_pending_unmute) {
            vTaskDelay(pdMS_TO_TICKS(100));
            xsmt_unmute();
            s_pending_unmute = false;
        }

        status_update(pos_ms, dur_ms, cur_samprate, cur_chans);
    }
}

void audio_init(void)
{
    s_cmd_q = xQueueCreate(4, sizeof(audio_msg_t));
    s_status_mux = xSemaphoreCreateMutex();

    // XSMT GPIO: bootkor LOW (mute), hogy a BCLK elindulása ne klikkeljen.
    // Késleltetés után HIGH-ra állítjuk, amikor a DMA már stabil zerókat ad.
    gpio_config_t xsmt_io = {
        .pin_bit_mask = 1ULL << PIN_XSMT,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&xsmt_io);
    xsmt_mute();

    // Indulásra default I2S 44.1k — első keret után úgyis újrahangoljuk.
    setup_i2s(44100);

    // BCLK stabilizálódjon, majd unmute. A DMA bufferek zerók (calloc) →
    // a DAC csendben indul.
    vTaskDelay(pdMS_TO_TICKS(5));
    xsmt_unmute();

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

void audio_dac_mute(void) { xsmt_mute(); }

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
