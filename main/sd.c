#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"

#include "app_config.h"
#include "sd.h"

static const char *TAG = "sd";
static sdmmc_card_t *s_card = NULL;

void sd_init(void)
{
    // Közös SPI busz inicializálása (TFT is ezt használja).
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &buscfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    // 10 MHz: a dupont-bekötés 20/40 MHz-en CRC-error/mount-fail volt, de a
    // 10 MHz méréssel (CLI `sdtest`) hibamentes és ~2× gyorsabb (≈950 KB/s vs
    // 5 MHz ≈540 KB/s). A sebesség az USB MSC módhoz kell: a macOS FSKit a
    // mountkor ~20s alatt beolvassa a (8 GB, 4 KB-cluster → ~7.6 MB) FAT-táblát,
    // ami 5 MHz-en nem fért bele → "FATManager failed to init". 10 MHz-en igen.
    // A track-betöltés/album-art is gyorsabb lett tőle.
    host.max_freq_khz = 10000;   // 10 MHz

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = PIN_SD_CS;
    slot_cfg.host_id = SD_SPI_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        s_card = NULL;
        return;
    }
    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD mounted at %s", SD_MOUNT_POINT);
}

sdmmc_card_t *sd_init_card_raw(void)
{
    // Közös SPI busz (a TFT is ezt használja). Ha már inicializált (pl. nem
    // ez fut először), az INVALID_STATE rendben van.
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &buscfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return NULL;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = 10000;   // 10 MHz — lásd sd_init kommentjét (MSC sebesség)

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = PIN_SD_CS;
    slot_cfg.host_id = SD_SPI_HOST;

    err = sdspi_host_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "sdspi_host_init: %s", esp_err_to_name(err));
        return NULL;
    }

    sdspi_dev_handle_t dev_handle;
    err = sdspi_host_init_device(&slot_cfg, &dev_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdspi_host_init_device: %s", esp_err_to_name(err));
        return NULL;
    }
    host.slot = dev_handle;   // a card-init innen kommunikál a kártyával

    sdmmc_card_t *card = calloc(1, sizeof(sdmmc_card_t));
    if (!card) {
        ESP_LOGE(TAG, "card alloc failed");
        return NULL;
    }
    err = sdmmc_card_init(&host, card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_card_init: %s", esp_err_to_name(err));
        free(card);
        return NULL;
    }
    sdmmc_card_print_info(stdout, card);
    ESP_LOGI(TAG, "SD card raw-init OK (no FAT mount) — MSC ready");
    return card;
}

void sd_speed_test(void)
{
    if (!s_card) { ESP_LOGW(TAG, "sdtest: nincs kártya"); return; }

    const int CHUNK = 16;           // szektor/olvasás (8 KB) — = az MSC EP-buffer
    const int SEQ_TOTAL = 4096;     // 2 MB szekvenciálisan
    uint8_t *buf = heap_caps_malloc((size_t)CHUNK * 512, MALLOC_CAP_DMA);
    if (!buf) { ESP_LOGE(TAG, "sdtest: buffer alloc fail"); return; }

    // (1) Szekvenciális multi-block (64 szektor/olvasás)
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = ESP_OK;
    for (int s = 0; s < SEQ_TOTAL; s += CHUNK) {
        err = sdmmc_read_sectors(s_card, buf, s, CHUNK);
        if (err != ESP_OK) { ESP_LOGE(TAG, "sdtest multi: err @%d %s", s, esp_err_to_name(err)); break; }
    }
    int64_t dt = esp_timer_get_time() - t0;
    if (err == ESP_OK && dt > 0)
        ESP_LOGI(TAG, "sdtest MULTI(64-blk): %d KB / %lld ms = %.0f KB/s",
                 SEQ_TOTAL / 2, dt / 1000, (double)SEQ_TOTAL * 512.0 * 1000.0 / (double)dt);

    // (2) Szekvenciális single-block (1 szektor/olvasás) — összehasonlításnak
    t0 = esp_timer_get_time();
    for (int s = 0; s < 512; s++) {
        if (sdmmc_read_sectors(s_card, buf, s, 1) != ESP_OK) break;
    }
    dt = esp_timer_get_time() - t0;
    if (dt > 0)
        ESP_LOGI(TAG, "sdtest SINGLE(1-blk): 256 KB / %lld ms = %.0f KB/s",
                 dt / 1000, 256.0 * 1024.0 * 1000.0 / (double)dt);

    // (3) Random 1-szektoros olvasás latencia (a FAT-séta jellege)
    t0 = esp_timer_get_time();
    int n = 300;
    uint32_t cap = (s_card->csd.capacity > 1) ? s_card->csd.capacity - 1 : 1;
    uint32_t lba = 12345;
    for (int i = 0; i < n; i++) {
        lba = (lba * 2654435761u + 1u) % cap;
        if (sdmmc_read_sectors(s_card, buf, lba, 1) != ESP_OK) break;
    }
    dt = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "sdtest RANDOM(1-blk): %d olvasás / %lld ms = %.2f ms/olvasás",
             n, dt / 1000, (double)dt / 1000.0 / (double)n);

    // (4) BPB dump: a FAT mérete dönti el, belefér-e a macOS 20s mount-ablakába.
    if (sdmmc_read_sectors(s_card, buf, 0, 1) == ESP_OK) {
        // MBR 1. partíció: típus @446+4, LBA-start @446+8 (LE)
        uint32_t part_lba = buf[454] | (buf[455] << 8) | (buf[456] << 16) | ((uint32_t)buf[457] << 24);
        uint8_t  part_type = buf[450];
        ESP_LOGI(TAG, "sdtest MBR: part1 type=0x%02X lba_start=%lu", part_type, (unsigned long)part_lba);
        if (part_lba > 0 && sdmmc_read_sectors(s_card, buf, part_lba, 1) == ESP_OK) {
            uint16_t byts_per_sec = buf[11] | (buf[12] << 8);
            uint8_t  sec_per_clus = buf[13];
            uint8_t  num_fats     = buf[16];
            uint32_t fatsz32      = buf[36] | (buf[37] << 8) | (buf[38] << 16) | ((uint32_t)buf[39] << 24);
            uint64_t fat_bytes    = (uint64_t)fatsz32 * byts_per_sec;
            ESP_LOGI(TAG, "sdtest BPB: byts/sec=%u sec/clus=%u (cluster=%u KB) num_fats=%u FATsz=%lu sec",
                     byts_per_sec, sec_per_clus, (byts_per_sec * sec_per_clus) / 1024, num_fats,
                     (unsigned long)fatsz32);
            ESP_LOGI(TAG, "sdtest FAT-tábla = %lu KB/db -> ~%.1f s beolvasni @500KB/s",
                     (unsigned long)(fat_bytes / 1024), (double)fat_bytes / 1024.0 / 500.0);
        }
    }

    heap_caps_free(buf);
}

static bool has_ext(const char *name, const char *ext)
{
    int nl = strlen(name);
    int el = strlen(ext);
    return (nl > el) && (strcasecmp(name + nl - el, ext) == 0);
}

// -----------------------------------------------------------------------------
// ID3 (v2.2/v2.3/v2.4 + v1) parser — TIT2/TPE1/TALB kinyerésére
// -----------------------------------------------------------------------------
static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint32_t read_syncsafe32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7)  |  (uint32_t)(p[3] & 0x7F);
}

// Decode an ID3 text frame payload (encoding byte + bytes) to UTF-8.
// Supports: 0=ISO-8859-1, 1=UTF-16 with BOM, 2=UTF-16 BE, 3=UTF-8.
// Stops at first NUL terminator within the payload.
static void decode_id3_text(int encoding, const uint8_t *data, int len,
                            char *out, int out_sz)
{
    if (out_sz <= 0) return;
    int o = 0;
    out[0] = 0;
    if (len <= 0) return;

    if (encoding == 3) {                                  // UTF-8
        int start = 0;
        if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) start = 3;
        for (int i = start; i < len && o < out_sz - 1; i++) {
            if (data[i] == 0) break;
            out[o++] = (char)data[i];
        }
    } else if (encoding == 0) {                           // ISO-8859-1 → UTF-8
        for (int i = 0; i < len && o < out_sz - 1; i++) {
            uint8_t c = data[i];
            if (c == 0) break;
            if (c < 0x80) {
                out[o++] = (char)c;
            } else if (o + 1 < out_sz - 1) {
                out[o++] = (char)(0xC0 | (c >> 6));
                out[o++] = (char)(0x80 | (c & 0x3F));
            }
        }
    } else if (encoding == 1 || encoding == 2) {          // UTF-16
        bool little = false;     // default BE for encoding=2
        int start = 0;
        if (encoding == 1) {
            if (len >= 2 && data[0] == 0xFF && data[1] == 0xFE) { little = true;  start = 2; }
            else if (len >= 2 && data[0] == 0xFE && data[1] == 0xFF) { little = false; start = 2; }
        }
        for (int i = start; i + 1 < len; i += 2) {
            uint8_t b0 = data[i], b1 = data[i+1];
            uint16_t cp = little ? (uint16_t)(b0 | (b1 << 8))
                                 : (uint16_t)(b1 | (b0 << 8));
            if (cp == 0) break;
            // Surrogate pair (BMP feletti karakter) — egyszerű skip, a music-tag-ek 99%-a BMP.
            if (cp >= 0xD800 && cp <= 0xDFFF) continue;
            if (cp < 0x80) {
                if (o < out_sz - 1) out[o++] = (char)cp;
            } else if (cp < 0x800) {
                if (o + 1 < out_sz - 1) {
                    out[o++] = (char)(0xC0 | (cp >> 6));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                }
            } else {
                if (o + 2 < out_sz - 1) {
                    out[o++] = (char)(0xE0 | (cp >> 12));
                    out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                }
            }
        }
    }
    out[o] = 0;
}

// ID3v2 streaming-parse: a header (fseek 0, fread 10) után frame-ről frame-re
// ugrunk. A célzott frame-eket olvassuk, a többit fseek-keljük át. Restore-oljuk
// az eredeti fájl-pozíciót. Visszaadja true-t ha legalább 1 mező sikerült.
static bool parse_id3v2(FILE *fp, char *title, int title_sz,
                                  char *artist, int artist_sz,
                                  char *album, int album_sz)
{
    long start = ftell(fp);
    bool any = false;
    if (fseek(fp, 0, SEEK_SET) != 0) goto done;

    uint8_t hdr[10];
    if (fread(hdr, 1, 10, fp) != 10) goto done;
    if (hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3') goto done;
    int major = hdr[3];
    if (major < 2 || major > 4) goto done;

    uint32_t body_sz = read_syncsafe32(&hdr[6]);
    long body_end = 10 + (long)body_sz;

    // Extended header (v2.3, v2.4) átugrása.
    if ((major == 3 || major == 4) && (hdr[5] & 0x40)) {
        uint8_t eh[4];
        if (fread(eh, 1, 4, fp) != 4) goto done;
        uint32_t ext_sz;
        if (major == 4) {
            ext_sz = read_syncsafe32(eh);
            fseek(fp, ext_sz - 4, SEEK_CUR);   // v2.4: ext_sz tartalmazza saját 4 byte-ját
        } else {
            ext_sz = read_be32(eh);
            fseek(fp, ext_sz, SEEK_CUR);       // v2.3: ext_sz csak a maradék
        }
    }

    bool got_t = false, got_a = false, got_al = false;
    if (title)  title[0]  = 0;
    if (artist) artist[0] = 0;
    if (album)  album[0]  = 0;

    int frame_hdr_sz = (major == 2) ? 6 : 10;

    while (ftell(fp) + frame_hdr_sz <= body_end) {
        uint8_t fh[10];
        if (fread(fh, 1, frame_hdr_sz, fp) != (size_t)frame_hdr_sz) break;
        if (fh[0] == 0) break;                  // padding zóna a frame-ek után

        char id[5] = {0};
        uint32_t frame_sz;
        if (major == 2) {
            memcpy(id, fh, 3); id[3] = 0;
            frame_sz = ((uint32_t)fh[3] << 16) | ((uint32_t)fh[4] << 8) | fh[5];
        } else {
            memcpy(id, fh, 4); id[4] = 0;
            frame_sz = (major == 4) ? read_syncsafe32(&fh[4]) : read_be32(&fh[4]);
        }

        if (frame_sz == 0 || frame_sz > 4096) {
            // Túl nagy frame (pl. embedded APIC artwork) vagy üres — átugorjuk.
            if (frame_sz > 0) fseek(fp, frame_sz, SEEK_CUR);
            continue;
        }

        char *target = NULL; int target_sz = 0; bool *done_flag = NULL;
        if      ((!got_t  && (strcmp(id, "TIT2") == 0 || strcmp(id, "TT2") == 0))) {
            target = title;  target_sz = title_sz;  done_flag = &got_t;
        }
        else if ((!got_a  && (strcmp(id, "TPE1") == 0 || strcmp(id, "TP1") == 0))) {
            target = artist; target_sz = artist_sz; done_flag = &got_a;
        }
        else if ((!got_al && (strcmp(id, "TALB") == 0 || strcmp(id, "TAL") == 0))) {
            target = album;  target_sz = album_sz;  done_flag = &got_al;
        }

        if (target && target_sz > 0) {
            uint8_t local[1024];
            uint8_t *buf = (frame_sz <= sizeof(local)) ? local : malloc(frame_sz);
            if (buf) {
                if (fread(buf, 1, frame_sz, fp) == frame_sz && frame_sz >= 1) {
                    decode_id3_text(buf[0], &buf[1], frame_sz - 1, target, target_sz);
                    *done_flag = true;
                    any = true;
                }
                if (buf != local) free(buf);
            } else {
                fseek(fp, frame_sz, SEEK_CUR);
            }
        } else {
            fseek(fp, frame_sz, SEEK_CUR);
        }

        if (got_t && got_a && got_al) break;
    }

done:
    fseek(fp, start, SEEK_SET);
    return any;
}

// ID3v1: a fájl végén 128 byte. "TAG" + 30 title + 30 artist + 30 album + 4 year + 30 comment + 1 genre.
// Latin-1 kódolás, szóköz vagy NUL paddolás.
static bool parse_id3v1(FILE *fp, char *title, int title_sz,
                                  char *artist, int artist_sz,
                                  char *album, int album_sz)
{
    long start = ftell(fp);
    bool any = false;
    if (fseek(fp, -128, SEEK_END) != 0) goto done;
    uint8_t buf[128];
    if (fread(buf, 1, 128, fp) != 128) goto done;
    if (buf[0] != 'T' || buf[1] != 'A' || buf[2] != 'G') goto done;

    // Egy 30-byte mező → Latin1 trim + decode UTF-8-ra.
    #define COPY_FIELD(offset, out, out_sz) do { \
        if ((out) && (out_sz) > 0) {                                              \
            uint8_t raw[30]; memcpy(raw, &buf[offset], 30);                       \
            int n = 30; while (n > 0 && (raw[n-1] == ' ' || raw[n-1] == 0)) n--;  \
            decode_id3_text(0, raw, n, (out), (out_sz));                          \
            if ((out)[0]) any = true;                                             \
        }                                                                         \
    } while (0)

    COPY_FIELD(3,  title,  title_sz);
    COPY_FIELD(33, artist, artist_sz);
    COPY_FIELD(63, album,  album_sz);
    #undef COPY_FIELD

done:
    fseek(fp, start, SEEK_SET);
    return any;
}

// Egy MP3 fájl ID3 metaadatainak betöltése: előbb v2, ha nem, fallback v1.
// title/artist/album üres ha semmi nincs. WAV-ra nem hívjuk (no-op lenne).
static void sd_load_id3(const char *path,
                        char *title, int title_sz,
                        char *artist, int artist_sz,
                        char *album, int album_sz)
{
    if (title)  title[0]  = 0;
    if (artist) artist[0] = 0;
    if (album)  album[0]  = 0;

    FILE *fp = fopen(path, "rb");
    if (!fp) return;
    if (!parse_id3v2(fp, title, title_sz, artist, artist_sz, album, album_sz)) {
        parse_id3v1(fp, title, title_sz, artist, artist_sz, album, album_sz);
    }
    fclose(fp);
}

// Több-mappás sd_scan_tracks-hez: album-csoportosítás, azon belül név.
static int cmp_tracks(const void *a, const void *b)
{
    const track_t *ta = a, *tb = b;
    int c = strcasecmp(ta->album, tb->album);
    if (c != 0) return c;
    return strcasecmp(ta->name, tb->name);
}

// Egy mappán belüli playlist-hez: csak fájlnév szerint. Az album-csoportosítás
// itt csak zavarna, mert az ID3 TALB fájlonként eltérhet (compilation albumok,
// .wav-ok mappa-név album-mal, stb.) és emiatt az alfabetikus fájlsorrend
// szétesik.
static int cmp_tracks_by_name(const void *a, const void *b)
{
    const track_t *ta = a, *tb = b;
    return strcasecmp(ta->name, tb->name);
}

// Egy könyvtár szkennelése: MP3-ak hozzáadása a listához.
// A `tracks[n].name` formátum: "AlbumNév — Fájlnév".
static int scan_album_dir(const char *album_path, const char *album_name,
                          track_t *tracks, int start, int max_tracks)
{
    DIR *d = opendir(album_path);
    if (!d) return start;
    int n = start;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max_tracks) {
        if (e->d_name[0] == '.') continue;
        bool is_mp3 = has_ext(e->d_name, ".mp3");
        bool is_wav = has_ext(e->d_name, ".wav");
        if (!is_mp3 && !is_wav) continue;

        snprintf(tracks[n].path, sizeof(tracks[n].path), "%s/%s", album_path, e->d_name);

        // Album fallback: a mappa neve. ID3 TALB ezt felülírja MP3-nál.
        strncpy(tracks[n].album, album_name, sizeof(tracks[n].album) - 1);
        tracks[n].album[sizeof(tracks[n].album) - 1] = 0;

        strncpy(tracks[n].name, e->d_name, sizeof(tracks[n].name) - 1);
        tracks[n].name[sizeof(tracks[n].name) - 1] = 0;
        int len = strlen(tracks[n].name);
        if (len > 4) tracks[n].name[len - 4] = 0;  // .mp3 / .wav lecsap

        // ID3 metadata MP3-nál (WAV-ra no-op a parser).
        tracks[n].title[0]  = 0;
        tracks[n].artist[0] = 0;
        if (is_mp3) {
            char id3_album[sizeof(tracks[n].album)] = {0};
            sd_load_id3(tracks[n].path,
                        tracks[n].title,  sizeof(tracks[n].title),
                        tracks[n].artist, sizeof(tracks[n].artist),
                        id3_album,        sizeof(id3_album));
            if (id3_album[0]) {
                strncpy(tracks[n].album, id3_album, sizeof(tracks[n].album) - 1);
                tracks[n].album[sizeof(tracks[n].album) - 1] = 0;
            }
        }
        n++;
    }
    closedir(d);
    return n;
}

int sd_scan_tracks(track_t *tracks, int max_tracks)
{
    if (!s_card) return 0;

    DIR *root = opendir(SD_MOUNT_POINT);
    if (!root) {
        ESP_LOGW(TAG, "Cannot open SD root %s", SD_MOUNT_POINT);
        return 0;
    }

    int n = 0;
    struct dirent *e;
    while ((e = readdir(root)) != NULL && n < max_tracks) {
        if (e->d_name[0] == '.') continue;
        if (e->d_type != DT_DIR) continue;     // csak a könyvtárak = albumok

        char album_path[384];
        snprintf(album_path, sizeof(album_path), "%s/%s", SD_MOUNT_POINT, e->d_name);
        int before = n;
        n = scan_album_dir(album_path, e->d_name, tracks, n, max_tracks);
        ESP_LOGI(TAG, "Album '%s': %d tracks", e->d_name, n - before);
    }
    closedir(root);

    qsort(tracks, n, sizeof(track_t), cmp_tracks);
    ESP_LOGI(TAG, "Total %d tracks across all albums", n);
    return n;
}

bool sd_find_album_art(const char *mp3_path, char *out_path, int out_path_len)
{
    // Szabály:
    //   - pontosan EGY .jpg/.jpeg van a track mappájában → az lesz az art,
    //     a fájlnévtől függetlenül
    //   - több jpg → név-egyezés: <tracknév>.jpg, cover.jpg, folder.jpg
    // (PNG nincs: a firmware-ben csak JPEG-dekóder van.)
    char dir[MAX_PATH_LEN];
    strncpy(dir, mp3_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char *slash = strrchr(dir, '/');
    if (!slash) return false;
    *slash = 0;

    DIR *d = opendir(dir);
    if (!d) return false;
    int  jpg_count = 0;
    char only[256] = {0};   // az egyetlen jpg neve (ha jpg_count == 1)
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || e->d_type == DT_DIR) continue;
        if (!has_ext(e->d_name, ".jpg") && !has_ext(e->d_name, ".jpeg")) continue;
        if (++jpg_count == 1) {
            strncpy(only, e->d_name, sizeof(only) - 1);
        } else {
            break;   // több is van — a név már nem számít, egyezésre váltunk
        }
    }
    closedir(d);

    if (jpg_count == 0) return false;
    if (jpg_count == 1) {
        snprintf(out_path, out_path_len, "%s/%s", dir, only);
        return true;
    }

    // Több jpg → 1. jelölt: a track fájlneve .jpg kiterjesztéssel
    struct stat st;
    char base[MAX_PATH_LEN];
    strncpy(base, mp3_path, sizeof(base) - 1);
    base[sizeof(base) - 1] = 0;
    char *dot = strrchr(base, '.');
    if (dot && (size_t)(dot - base) + 5 < sizeof(base)) {
        strcpy(dot, ".jpg");
        if (stat(base, &st) == 0) {
            strncpy(out_path, base, out_path_len);
            out_path[out_path_len - 1] = 0;
            return true;
        }
    }

    // 2. jelölt: cover.jpg / folder.jpg (a FATFS név-egyeztetése kisbetű-független)
    const char *candidates[] = { "cover.jpg", "folder.jpg" };
    for (int i = 0; i < 2; i++) {
        snprintf(out_path, out_path_len, "%s/%s", dir, candidates[i]);
        if (stat(out_path, &st) == 0) return true;
    }
    return false;
}

// Sorrend: m3u playlistek legelöl (a UI "Play all" sorai), aztán mappák,
// végül fájlok; azon belül ábécé (kis/nagybetű-független).
static int entry_rank(const dir_entry_t *e)
{
    if (e->is_m3u) return 0;
    return e->is_dir ? 1 : 2;
}

static int cmp_entries(const void *a, const void *b)
{
    const dir_entry_t *ea = a, *eb = b;
    int ra = entry_rank(ea), rb = entry_rank(eb);
    if (ra != rb) return ra - rb;
    return strcasecmp(ea->name, eb->name);
}

int sd_list_dir(const char *path, dir_entry_t *out, int max_entries)
{
    if (!s_card) return 0;
    DIR *d = opendir(path);
    if (!d) {
        ESP_LOGW(TAG, "opendir failed: %s", path);
        return 0;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max_entries) {
        if (e->d_name[0] == '.') continue;
        bool is_dir = (e->d_type == DT_DIR);
        bool is_m3u = !is_dir && (has_ext(e->d_name, ".m3u")
                               || has_ext(e->d_name, ".m3u8"));
        bool is_ch8 = !is_dir && has_ext(e->d_name, ".ch8");
        if (!is_dir && !is_m3u && !is_ch8 && !has_ext(e->d_name, ".mp3")
                    && !has_ext(e->d_name, ".wav")) continue;
        strncpy(out[n].name, e->d_name, sizeof(out[n].name) - 1);
        out[n].name[sizeof(out[n].name) - 1] = 0;
        out[n].is_dir = is_dir;
        out[n].is_m3u = is_m3u;
        out[n].is_ch8 = is_ch8;
        n++;
    }
    closedir(d);
    qsort(out, n, sizeof(dir_entry_t), cmp_entries);
    return n;
}

int sd_load_dir_tracks(const char *path, track_t *out, int max_tracks)
{
    if (!s_card) return 0;
    // album név fallback = az út utolsó komponense (ID3 TALB ezt felülírja).
    const char *album = strrchr(path, '/');
    album = album ? album + 1 : path;
    int n = scan_album_dir(path, album, out, 0, max_tracks);
    qsort(out, n, sizeof(track_t), cmp_tracks_by_name);
    return n;
}

// -----------------------------------------------------------------------------
// M3U playlist betöltés
// -----------------------------------------------------------------------------
// "." és ".." komponensek feloldása in-place. Kell, mert a FATFS VFS
// (FF_FS_RPATH=0) nem értelmezi a ".."-t, az m3u-kban viszont tipikus a
// "../MasikAlbum/track.mp3" forma. Gyökér fölé nem enged ki (a felesleges
// ".." egyszerűen elfogy — az eredmény stat()-ja úgyis megbukik, ha rossz).
static void path_normalize(char *path)
{
    char buf[sizeof(((track_t *)0)->path)];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    char *segs[48];
    int n = 0;
    bool abs = (buf[0] == '/');
    for (char *tok = strtok(buf, "/"); tok; tok = strtok(NULL, "/")) {
        if (strcmp(tok, ".") == 0) continue;
        if (strcmp(tok, "..") == 0) { if (n > 0) n--; continue; }
        if (n < (int)(sizeof(segs) / sizeof(segs[0]))) segs[n++] = tok;
    }
    char *w = path;
    if (abs) *w++ = '/';
    for (int i = 0; i < n; i++) {
        if (i) *w++ = '/';
        size_t l = strlen(segs[i]);
        memcpy(w, segs[i], l);
        w += l;
    }
    *w = 0;
}

// Egy track_t kitöltése teljes útvonalból: név (kiterjesztés nélkül), album-
// fallback a szülőmappa neve, ID3 MP3-nál — ugyanaz a logika, mint a
// scan_album_dir-ben, csak egyetlen, már ismert útvonalra.
static bool fill_track_from_path(const char *path, track_t *t)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    bool is_mp3 = has_ext(base, ".mp3");
    if (!is_mp3 && !has_ext(base, ".wav")) return false;

    strncpy(t->path, path, sizeof(t->path) - 1);
    t->path[sizeof(t->path) - 1] = 0;

    strncpy(t->name, base, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = 0;
    int len = strlen(t->name);
    if (len > 4) t->name[len - 4] = 0;   // .mp3 / .wav lecsap

    // Album fallback: a track SAJÁT mappájának neve (albumokon átívelő
    // playlistnél trackenként más-más).
    t->album[0] = 0;
    if (base > path + 1) {
        const char *q = base - 2;             // a '/' előtti utolsó karakter
        while (q > path && *q != '/') q--;
        if (*q == '/') q++;
        size_t alen = (size_t)(base - 1 - q);
        if (alen >= sizeof(t->album)) alen = sizeof(t->album) - 1;
        memcpy(t->album, q, alen);
        t->album[alen] = 0;
    }

    t->title[0]  = 0;
    t->artist[0] = 0;
    if (is_mp3) {
        char id3_album[sizeof(t->album)] = {0};
        sd_load_id3(t->path,
                    t->title,  sizeof(t->title),
                    t->artist, sizeof(t->artist),
                    id3_album, sizeof(id3_album));
        if (id3_album[0]) {
            strncpy(t->album, id3_album, sizeof(t->album) - 1);
            t->album[sizeof(t->album) - 1] = 0;
        }
    }
    return true;
}

int sd_load_m3u_tracks(const char *m3u_path, track_t *out, int max_tracks)
{
    if (!s_card) return 0;
    FILE *f = fopen(m3u_path, "r");
    if (!f) {
        ESP_LOGW(TAG, "m3u: nem nyithato: %s", m3u_path);
        return 0;
    }

    // Az m3u mappája — ehhez képest oldjuk fel a relatív utakat.
    char dir[sizeof(((track_t *)0)->path)];
    strncpy(dir, m3u_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    else       strcpy(dir, SD_MOUNT_POINT);

    int n = 0, skipped = 0;
    char line[sizeof(((track_t *)0)->path)];
    bool truncated = false;
    while (fgets(line, sizeof(line), f)) {
        if (n >= max_tracks) { truncated = true; break; }

        // Trim: CR/LF/szóköz a végéről, szóköz az elejéről; backslash → slash.
        int l = strlen(line);
        while (l > 0 && (line[l-1] == '\r' || line[l-1] == '\n' ||
                         line[l-1] == ' '  || line[l-1] == '\t')) line[--l] = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0 || *p == '#') continue;     // üres sor / EXTM3U / EXTINF
        for (char *c = p; *c; c++) if (*c == '\\') *c = '/';

        char full[sizeof(((track_t *)0)->path)];
        if (p[0] == '/') {
            // Abszolút út: ha nem a mountpont alatt van (PC-n készült lista),
            // a kártya gyökeréhez értelmezzük.
            if (strncmp(p, SD_MOUNT_POINT "/", strlen(SD_MOUNT_POINT) + 1) == 0)
                snprintf(full, sizeof(full), "%.383s", p);
            else
                snprintf(full, sizeof(full), SD_MOUNT_POINT "%.376s", p);
        } else {
            snprintf(full, sizeof(full), "%.255s/%.127s", dir, p);
        }
        path_normalize(full);

        struct stat st;
        if (stat(full, &st) != 0 || !fill_track_from_path(full, &out[n])) {
            skipped++;
            ESP_LOGW(TAG, "m3u: kihagyva: '%s'", p);
            continue;
        }
        n++;
    }
    fclose(f);

    ESP_LOGI(TAG, "m3u %s: %d track (%d kihagyva)", m3u_path, n, skipped);
    if (truncated) ESP_LOGW(TAG, "m3u: a lista %d tracknel csonkolva", max_tracks);
    return n;
}
