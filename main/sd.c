#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_log.h"
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
    host.max_freq_khz = 5000;    // 5 MHz — diagnosztikai mód

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

static bool has_ext(const char *name, const char *ext)
{
    int nl = strlen(name);
    int el = strlen(ext);
    return (nl > el) && (strcasecmp(name + nl - el, ext) == 0);
}

static int cmp_tracks(const void *a, const void *b)
{
    const track_t *ta = a, *tb = b;
    int c = strcasecmp(ta->album, tb->album);
    if (c != 0) return c;
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
        if (!has_ext(e->d_name, ".mp3")) continue;

        snprintf(tracks[n].path, sizeof(tracks[n].path), "%s/%s", album_path, e->d_name);

        // Album és track-név külön mezőben — a UI komponálja össze ha kell.
        strncpy(tracks[n].album, album_name, sizeof(tracks[n].album) - 1);
        tracks[n].album[sizeof(tracks[n].album) - 1] = 0;

        strncpy(tracks[n].name, e->d_name, sizeof(tracks[n].name) - 1);
        tracks[n].name[sizeof(tracks[n].name) - 1] = 0;
        int len = strlen(tracks[n].name);
        if (len > 4) tracks[n].name[len - 4] = 0;  // .mp3 lecsap
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
    // Próbálkozás 1: ugyanaz a fájlnév .jpg-vel
    // Próbálkozás 2: cover.jpg / folder.jpg a könyvtárban
    char base[MAX_PATH_LEN];
    strncpy(base, mp3_path, sizeof(base) - 1);
    base[sizeof(base) - 1] = 0;
    int len = strlen(base);
    if (len < 4) return false;

    // .mp3 → .jpg
    strcpy(base + len - 4, ".jpg");
    struct stat st;
    if (stat(base, &st) == 0) {
        strncpy(out_path, base, out_path_len);
        return true;
    }

    // dirname/cover.jpg
    char dir[MAX_PATH_LEN];
    strncpy(dir, mp3_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char *slash = strrchr(dir, '/');
    if (!slash) return false;
    *slash = 0;

    const char *candidates[] = { "cover.jpg", "folder.jpg", "cover.png", "folder.png" };
    for (int i = 0; i < 4; i++) {
        snprintf(out_path, out_path_len, "%s/%s", dir, candidates[i]);
        if (stat(out_path, &st) == 0) return true;
    }
    return false;
}
