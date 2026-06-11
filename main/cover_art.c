#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "cover_art.h"

static const char *TAG = "cover";

// Beágyazott placeholder (EMBED_FILES a CMakeLists.txt-ben).
extern const uint8_t _binary_cover_placeholder_jpg_start[];
extern const uint8_t _binary_cover_placeholder_jpg_end[];

// Plafonok: ennél nagyobb fájlt / dekódolt képet nem vállalunk be. A kimeneti
// plafon RGB888-ban ~930×930 — a dekóder 1/2..1/8 downscale-lel ez alá hozza
// a nagyobb borítókat, csak a (>8× túlméretes) extrémeket dobjuk el.
#define COVER_MAX_FILE_BYTES  (3 * 1024 * 1024)
#define COVER_MAX_OUT_BYTES   (2560 * 1024)

typedef struct {
    lv_image_dsc_t dsc;
    uint8_t       *buf;   // 16-byte igazított PSRAM (esp_new_jpeg követelmény)
} cover_slot_t;

// Két slot forog (lásd cover_art.h): s_active az utoljára kiadott. Az írás
// mindig a másik slotba megy, így a widgeten lévő buffer sosem szabadul fel
// megjelenítés közben.
static cover_slot_t s_slot[2];
static int          s_active = -1;
static cover_slot_t s_placeholder;

// JPEG (memóriában) → RGB888 a slot friss PSRAM-bufferébe. A kimeneti
// szélesség/magasság mindig 8 többszöröse (scale vagy clipper) — így az
// MCU-határ egybeesik a sorhatárral, a stride egyértelműen width*3.
static bool decode_to_slot(const uint8_t *jpg, int len, cover_slot_t *slot)
{
    // 1. menet: csak header-parse a natív méretért.
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) return false;
    jpeg_dec_io_t io = { .inbuf = (uint8_t *)jpg, .inbuf_len = len };
    jpeg_dec_header_info_t info = {0};
    jpeg_error_t err = jpeg_dec_parse_header(dec, &io, &info);
    jpeg_dec_close(dec);
    if (err != JPEG_ERR_OK || info.width == 0 || info.height == 0) {
        ESP_LOGW(TAG, "parse_header failed (%d)", err);
        return false;
    }

    // Kimeneti méret: downscale (1/2, 1/4, 1/8), amíg a RGB888 a plafon alá
    // nem fér; az eredmény 8-ra lefelé kerekítve.
    int k = 1;
    while (k <= 8 &&
           ((info.width / k) * (info.height / k) * 3) > COVER_MAX_OUT_BYTES) {
        k *= 2;
    }
    if (k > 8) {
        ESP_LOGW(TAG, "tul nagy kep: %dx%d — placeholder lesz", info.width, info.height);
        return false;
    }
    int ow = ((info.width  / k) / 8) * 8;
    int oh = ((info.height / k) / 8) * 8;
    if (ow < 8 || oh < 8) return false;

    cfg = (jpeg_dec_config_t)DEFAULT_JPEG_DEC_CONFIG();
    if (k > 1) {
        cfg.scale.width  = (uint16_t)ow;
        cfg.scale.height = (uint16_t)oh;
    } else if (ow != info.width || oh != info.height) {
        // Nem 8-osztható natív méret → pár pixel levágása a szélről.
        cfg.clipper.width  = (uint16_t)ow;
        cfg.clipper.height = (uint16_t)oh;
    }

    // 2. menet: tényleges dekódolás a végleges configgal.
    dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) return false;
    io   = (jpeg_dec_io_t){ .inbuf = (uint8_t *)jpg, .inbuf_len = len };
    info = (jpeg_dec_header_info_t){0};
    bool ok = false;
    uint8_t *out = NULL;
    int need = 0;
    if (jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK &&
        jpeg_dec_get_outbuf_len(dec, &need) == JPEG_ERR_OK && need > 0) {
        out = heap_caps_aligned_alloc(16, need, MALLOC_CAP_SPIRAM);
        if (out) {
            io.outbuf = out;
            if (jpeg_dec_process(dec, &io) == JPEG_ERR_OK) {
                ok = true;
            } else {
                ESP_LOGW(TAG, "dec_process failed");
            }
        } else {
            ESP_LOGW(TAG, "outbuf alloc failed (%d B)", need);
        }
    } else {
        ESP_LOGW(TAG, "parse/outbuf_len failed");
    }
    jpeg_dec_close(dec);
    if (!ok) {
        if (out) heap_caps_free(out);
        return false;
    }

    // R↔B csere — az esp_new_jpeg R,G,B sorrendet ad, az LVGL a
    // LV_COLOR_FORMAT_RGB888-at B,G,R-ként értelmezi (lásd boot_splash.c).
    int px = ow * oh * 3;
    for (int p = 0; p + 2 < px; p += 3) {
        uint8_t t  = out[p];
        out[p]     = out[p + 2];
        out[p + 2] = t;
    }

    memset(&slot->dsc, 0, sizeof(slot->dsc));
    slot->dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    slot->dsc.header.cf     = LV_COLOR_FORMAT_RGB888;
    slot->dsc.header.w      = ow;
    slot->dsc.header.h      = oh;
    slot->dsc.header.stride = ow * 3;
    slot->dsc.data          = out;
    slot->dsc.data_size     = px;
    slot->buf               = out;
    return true;
}

const lv_image_dsc_t *cover_art_load(const char *path)
{
    // A teljes művelet a portlock alatt fut: ez sorosítja a slot-forgatást a
    // hívók (player task / CLI task / LVGL task async) és a renderelés között.
    // A lock rekurzív, így LVGL-kontextusból is hívható. A dekód ideje
    // (~50–150 ms) alatt a UI áll — track-váltáskor ez nem zavaró.
    if (!lvgl_port_lock(0)) return NULL;

    const lv_image_dsc_t *res = NULL;
    uint8_t *in = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "nem nyithato: %s", path);
        goto out;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > COVER_MAX_FILE_BYTES) {
        ESP_LOGW(TAG, "rossz meret (%ld B): %s", fsize, path);
        fclose(f);
        goto out;
    }
    in = heap_caps_malloc(fsize, MALLOC_CAP_SPIRAM);
    if (!in) {
        ESP_LOGW(TAG, "inbuf alloc failed (%ld B)", fsize);
        fclose(f);
        goto out;
    }
    long rd = (long)fread(in, 1, fsize, f);
    fclose(f);
    if (rd != fsize) {
        ESP_LOGW(TAG, "read %ld/%ld B: %s", rd, fsize, path);
        goto out;
    }

    // Cél a NEM aktív slot. Ha van még benne korábbi buffer, az már két
    // generációval ezelőtti — a widgeten biztosan nem ez van, felszabadítható.
    int target = (s_active < 0) ? 0 : !s_active;
    cover_slot_t *slot = &s_slot[target];
    if (slot->buf) {
        lv_image_cache_drop(&slot->dsc);
        heap_caps_free(slot->buf);
        slot->buf = NULL;
    }

    if (decode_to_slot(in, (int)fsize, slot)) {
        s_active = target;
        res = &slot->dsc;
        ESP_LOGI(TAG, "cover %s: %dx%d", path,
                 (int)slot->dsc.header.w, (int)slot->dsc.header.h);
    }

out:
    if (in) heap_caps_free(in);
    lvgl_port_unlock();
    return res;
}

const lv_image_dsc_t *cover_art_placeholder(void)
{
    if (s_placeholder.buf) return &s_placeholder.dsc;
    int len = (int)(_binary_cover_placeholder_jpg_end -
                    _binary_cover_placeholder_jpg_start);
    if (!decode_to_slot(_binary_cover_placeholder_jpg_start, len, &s_placeholder)) {
        ESP_LOGE(TAG, "placeholder dekod failed");
        return NULL;
    }
    return &s_placeholder.dsc;
}
