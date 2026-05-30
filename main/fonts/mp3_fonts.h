#pragma once
//
// Inter Medium (500 weight) LVGL fontok, magyar (Latin-1 supplement + ő/ű) +
// LVGL FontAwesome szimbólumokkal.
//
// Újragenerálás: lásd fonts/regen.sh
//

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_font_t mp3_inter_12;
extern const lv_font_t mp3_inter_14;
extern const lv_font_t mp3_inter_18;

// -----------------------------------------------------------------------------
// Saját ikonok (a hivatalos lv_symbol_def.h-ban nincsenek)
//
// Csak ezekkel a fontokkal (mp3_inter_*) használd — a beépített LVGL
// Montserrat fontokban nincsenek ezek a glyphek.
// -----------------------------------------------------------------------------
#define MP3_SYMBOL_LOCK    "\xEF\x80\xA3"  // U+F023 (FontAwesome lock — zárva)
#define MP3_SYMBOL_UNLOCK  "\xEF\x82\x9C"  // U+F09C (FontAwesome unlock — nyitva)

#ifdef __cplusplus
}
#endif
