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

// Inicializálja a közös SPI buszt + felmountolja az SD-t /sdcard alatt.
// A spi bus host: SD_SPI_HOST — ezt használja a TFT is.
void sd_init(void);

// Beolvassa az MUSIC_DIR-ben található *.mp3 fájlokat (max MAX_TRACKS).
// Visszaadja a beolvasott darabszámot.
int  sd_scan_tracks(track_t *tracks, int max_tracks);

// Ha létezik a fájl mellett "cover.jpg" vagy "folder.jpg",
// kitölti az out_path-et és igazat ad vissza.
bool sd_find_album_art(const char *mp3_path, char *out_path, int out_path_len);
