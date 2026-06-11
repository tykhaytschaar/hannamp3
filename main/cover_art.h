#pragma once

#include "lvgl.h"

// -----------------------------------------------------------------------------
// Album art betöltés a Now Playing képernyőhöz.
//
// A JPEG-et az esp_new_jpeg dekódolja RGB888-ba, PSRAM-bufferbe — ugyanaz az
// út, mint a boot splash-nél (boot_splash.c). Az LVGL fs/tjpgd útvonalat
// szándékosan kerüljük: a tjpgd a teljes képet az LVGL 64 KB-os heapjébe
// bontaná ki, ami valós méretű borítóknál (~500×500) mindig kevés.
//
// Memória: két slot forog — az éppen kirajzolt cover bufferét csak akkor
// szabadítjuk fel, amikor már egy ÚJABB váltotta le a widgeten. A hívónak
// minden sikeres cover_art_load után lv_image_set_src-öt kell hívnia a
// visszakapott dsc-vel (ui_show_track ezt teszi).
// -----------------------------------------------------------------------------

// JPEG fájl dekódolása PSRAM-ba. NULL, ha nem olvasható / nem dekódolható /
// túl nagy. A visszaadott dsc a következő utáni load-ig érvényes.
// Az LVGL portlockot belül veszi fel (rekurzív — lock alól is hívható).
const lv_image_dsc_t *cover_art_load(const char *path);

// A flash-be ágyazott placeholder (assets/cover_placeholder.jpg), lazy-dekódolva.
// Az első hívás dekódol, utána ugyanazt a dsc-t adja. NULL, ha a dekódolás
// nem sikerült (nem várt eset — beágyazott, ismert tartalom).
const lv_image_dsc_t *cover_art_placeholder(void);
