#pragma once

#include <stdint.h>
#include <stdbool.h>

// -----------------------------------------------------------------------------
// Boot splash — a firmware-be ágyazott JPEG frame-szett ("videó"). A frame-ek
// az assets/boot/frame_NNN.jpg fájlokból kerülnek a binárisba (EMBED_FILES,
// lásd CMakeLists.txt), így lejátszáskor NINCS SD-hozzáférés — a közös SPI
// buszon nem ütközik az SD-olvasás a TFT-kirajzolással.
//
// A dekódolást az esp_new_jpeg végzi (a tjpgd nem eszi meg: a frame-ek FFFE
// COM szegmenssel kezdődnek JFIF APP0 helyett, és LV_USE_FS_MEMFS is off).
// A kimenet RGB888 — pontosan az album-art (tjpgd) útvonalát tükrözve, így a
// szín/byte-order/tájolás az LVGL + panel pipeline-on garantáltan helyes.
// -----------------------------------------------------------------------------

// A frame-ek natív felbontása = a kijelző fekvő felbontása (480×320).
#define BOOT_SPLASH_W   480
#define BOOT_SPLASH_H   320

// A beágyazott frame-ek száma.
int boot_splash_frame_count(void);

// A `idx`. frame dekódolása RGB888-ba az `outbuf`-ba (legalább
// BOOT_SPLASH_W*BOOT_SPLASH_H*3 byte, 16-byte igazítva). Visszaadja true-t
// sikeres dekódolásnál.
bool boot_splash_decode_rgb888(int idx, uint8_t *outbuf, int outbuf_len);
