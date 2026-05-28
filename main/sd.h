#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"

#define SD_SPI_HOST  SPI2_HOST

typedef struct {
    char path[384];   // teljes elérési út
    char album[96];   // album mappa neve
    char name[128];   // track fájlnév (kiterjesztés nélkül)
} track_t;

// Egy könyvtár-bejegyzés a böngészőhöz (almappa vagy .mp3 fájl).
typedef struct {
    char name[128];   // a bejegyzés neve (mappánál a mappanév, fájlnál a fájlnév kiterjesztéssel)
    bool is_dir;      // true = almappa, false = .mp3 fájl
} dir_entry_t;

// Inicializálja a közös SPI buszt + felmountolja az SD-t /sdcard alatt.
// A spi bus host: SD_SPI_HOST — ezt használja a TFT is.
void sd_init(void);

// Beolvassa az MUSIC_DIR-ben található *.mp3 fájlokat (max MAX_TRACKS).
// Visszaadja a beolvasott darabszámot.
int  sd_scan_tracks(track_t *tracks, int max_tracks);

// Ha létezik a fájl mellett "cover.jpg" vagy "folder.jpg",
// kitölti az out_path-et és igazat ad vissza.
bool sd_find_album_art(const char *mp3_path, char *out_path, int out_path_len);

// Böngészőhöz: egy könyvtár tartalma (almappák + .mp3 fájlok), NEM rekurzív.
// Rendezés: előbb a mappák, aztán a fájlok, ábécé szerint. Visszaadja a db-ot.
int sd_list_dir(const char *path, dir_entry_t *out, int max_entries);

// Lejátszáshoz: egy könyvtár .mp3 fájljai track_t-ként (NEM rekurzív).
// A track_t.album a mappa neve lesz. Visszaadja a db-ot.
int sd_load_dir_tracks(const char *path, track_t *out, int max_tracks);
