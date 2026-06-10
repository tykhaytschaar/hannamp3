#include "boot_splash.h"

#include "esp_log.h"
#include "esp_jpeg_dec.h"

static const char *TAG = "splash";

// -----------------------------------------------------------------------------
// Beágyazott frame-ek. Az EMBED_FILES (CMakeLists.txt) minden
// assets/boot/frame_NNN.jpg-hez generál egy
// _binary_frame_NNN_jpg_start / _end szimbólumpárt (a basename alapján). A
// lejátszási sorrendet az s_frames[] tábla rögzíti, nem az embed-sorrend.
// -----------------------------------------------------------------------------
#define FRAME_DECL(id) \
    extern const uint8_t _binary_frame_##id##_jpg_start[]; \
    extern const uint8_t _binary_frame_##id##_jpg_end[];

#define FRAME_ENT(id) { _binary_frame_##id##_jpg_start, _binary_frame_##id##_jpg_end }

FRAME_DECL(001) FRAME_DECL(002) FRAME_DECL(003) FRAME_DECL(004) FRAME_DECL(005)
FRAME_DECL(006) FRAME_DECL(007) FRAME_DECL(008) FRAME_DECL(009) FRAME_DECL(010)
FRAME_DECL(011) FRAME_DECL(012) FRAME_DECL(013) FRAME_DECL(014) FRAME_DECL(015)
FRAME_DECL(016) FRAME_DECL(017) FRAME_DECL(018) FRAME_DECL(019) FRAME_DECL(020)
FRAME_DECL(021) FRAME_DECL(022) FRAME_DECL(023) FRAME_DECL(024) FRAME_DECL(025)
FRAME_DECL(026) FRAME_DECL(027) FRAME_DECL(028) FRAME_DECL(029) FRAME_DECL(030)
FRAME_DECL(031) FRAME_DECL(032) FRAME_DECL(033) FRAME_DECL(034) FRAME_DECL(035)
FRAME_DECL(036) FRAME_DECL(037)

static const struct {
    const uint8_t *start;
    const uint8_t *end;
} s_frames[] = {
    FRAME_ENT(001), FRAME_ENT(002), FRAME_ENT(003), FRAME_ENT(004), FRAME_ENT(005),
    FRAME_ENT(006), FRAME_ENT(007), FRAME_ENT(008), FRAME_ENT(009), FRAME_ENT(010),
    FRAME_ENT(011), FRAME_ENT(012), FRAME_ENT(013), FRAME_ENT(014), FRAME_ENT(015),
    FRAME_ENT(016), FRAME_ENT(017), FRAME_ENT(018), FRAME_ENT(019), FRAME_ENT(020),
    FRAME_ENT(021), FRAME_ENT(022), FRAME_ENT(023), FRAME_ENT(024), FRAME_ENT(025),
    FRAME_ENT(026), FRAME_ENT(027), FRAME_ENT(028), FRAME_ENT(029), FRAME_ENT(030),
    FRAME_ENT(031), FRAME_ENT(032), FRAME_ENT(033), FRAME_ENT(034), FRAME_ENT(035),
    FRAME_ENT(036), FRAME_ENT(037),
};

int boot_splash_frame_count(void)
{
    return (int)(sizeof(s_frames) / sizeof(s_frames[0]));
}

bool boot_splash_decode_rgb888(int idx, uint8_t *outbuf, int outbuf_len)
{
    if (idx < 0 || idx >= boot_splash_frame_count() || !outbuf) return false;

    const uint8_t *data = s_frames[idx].start;
    int len = (int)(s_frames[idx].end - s_frames[idx].start);

    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB888;

    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "frame %d: dec_open failed", idx);
        return false;
    }

    jpeg_dec_io_t io = {
        .inbuf     = (uint8_t *)data,
        .inbuf_len = len,
    };
    jpeg_dec_header_info_t info = {0};
    bool ok = false;

    if (jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK) {
        int need = 0;
        if (jpeg_dec_get_outbuf_len(dec, &need) == JPEG_ERR_OK && need <= outbuf_len) {
            io.outbuf = outbuf;
            if (jpeg_dec_process(dec, &io) == JPEG_ERR_OK) {
                // Csatorna-sorrend illesztése az LVGL-hez. Az esp_new_jpeg
                // RGB888-at R,G,B sorrendben ad ki (R a legalsó byte), de az
                // LVGL LV_COLOR_FORMAT_RGB888-at B,G,R-ként értelmezi (lásd a
                // tjpgd kimenetét: B,G,R). Csere nélkül R↔B fordul → a
                // cián/kék sárgaként jelenik meg. Pixelenként [0]↔[2] csere.
                int px = BOOT_SPLASH_W * BOOT_SPLASH_H * 3;
                if (px > outbuf_len) px = outbuf_len;
                for (int p = 0; p + 2 < px; p += 3) {
                    uint8_t t   = outbuf[p];
                    outbuf[p]   = outbuf[p + 2];
                    outbuf[p + 2] = t;
                }
                ok = true;
            } else {
                ESP_LOGW(TAG, "frame %d: decode process failed", idx);
            }
        } else {
            ESP_LOGW(TAG, "frame %d: outbuf %d > %d (or len query failed)",
                     idx, need, outbuf_len);
        }
    } else {
        ESP_LOGW(TAG, "frame %d: parse_header failed", idx);
    }

    jpeg_dec_close(dec);
    return ok;
}
