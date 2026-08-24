#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"
#include "sdmmc_cmd.h"

#define SD_SPI_HOST  SPI2_HOST

typedef struct {
    char path[384];     // teljes elérési út
    char album[96];     // album: ID3 TALB/TAL, vagy fallback a mappa neve
    char name[128];     // track fájlnév (kiterjesztés nélkül) — fallback a title-höz
    char title[128];    // ID3 TIT2/TT2 (UTF-8). Üres ha nincs ID3.
    char artist[96];    // ID3 TPE1/TP1 (UTF-8). Üres ha nincs ID3.
} track_t;

// Egy könyvtár-bejegyzés a böngészőhöz (almappa, zenefájl, m3u playlist
// vagy Game Boy ROM).
typedef struct {
    char name[128];   // a bejegyzés neve (mappánál a mappanév, fájlnál a fájlnév kiterjesztéssel)
    bool is_dir;      // true = almappa
    bool is_m3u;      // true = .m3u/.m3u8 playlist (a UI "Play all" sorként mutatja)
    bool is_gb;       // true = .gb/.gbc Game Boy (Color) ROM (tap = GB mode, lásd gb.c)
} dir_entry_t;

// Csak a közös SPI busz inicializálása, SD-mount NÉLKÜL. Idempotens (a már
// futó buszra INVALID_STATE = OK). A low-battery boot-ág használja: az
// ui_init-hez kell a busz, de kártyára ott nincs szükség.
esp_err_t sd_bus_init(void);

// Inicializálja a közös SPI buszt + felmountolja az SD-t /sdcard alatt.
// A spi bus host: SD_SPI_HOST — ezt használja a TFT is.
void sd_init(void);

// USB MSC módhoz: inicializálja a közös SPI buszt + az SD kártyát, de NEM
// mountolja a FAT-ot (a host kapja a nyers blokk-hozzáférést). Visszaadja a
// kártya-handle-t, vagy NULL-t hiba esetén. NE hívd a sd_init-tel együtt —
// ez külön (MSC-) boot-ág.
sdmmc_card_t *sd_init_card_raw(void);

// Diagnosztika: nyers SD-olvasási sebesség mérése (multi-block / single-block /
// random latencia), az eredményt ESP_LOG-ra írja. A CLI `sdtest` hívja.
void sd_speed_test(void);

// Beolvassa az MUSIC_DIR-ben található *.mp3 fájlokat (max MAX_TRACKS).
// Visszaadja a beolvasott darabszámot.
int  sd_scan_tracks(track_t *tracks, int max_tracks);

// Album art keresése a track mappájában. Ha pontosan egy .jpg/.jpeg van a
// mappában, az lesz az art (névtől függetlenül); ha több, név-egyezés dönt:
// <tracknév>.jpg → cover.jpg → folder.jpg. Igazat ad, ha talált.
bool sd_find_album_art(const char *mp3_path, char *out_path, int out_path_len);

// Böngészőhöz: egy könyvtár tartalma (almappák + zenefájlok + m3u playlistek),
// NEM rekurzív. Rendezés: m3u-k legelöl, aztán mappák, végül fájlok, ábécé
// szerint. Visszaadja a db-ot.
int sd_list_dir(const char *path, dir_entry_t *out, int max_entries);

// Lejátszáshoz: egy könyvtár .mp3 fájljai track_t-ként (NEM rekurzív).
// A track_t.album a mappa neve lesz. Visszaadja a db-ot.
int sd_load_dir_tracks(const char *path, track_t *out, int max_tracks);

// Egy .m3u/.m3u8 playlist betöltése track_t-ként, a lista SORRENDJÉBEN (nincs
// újrarendezés — a prev/next és az auto-advance is ezt követi). Relatív utak
// az m3u mappájához képest; "#" kommentek, CRLF, backslash és ".." kezelve.
// A nem létező / nem lejátszható bejegyzések kimaradnak (log). Visszaadja a db-ot.
int sd_load_m3u_tracks(const char *m3u_path, track_t *out, int max_tracks);
