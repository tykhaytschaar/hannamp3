#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// CHIP-8 emulátormag — szándékosan "tiszta C": semmi LVGL/ESP függés, csak a
// VM. A megjelenítés, az input leképezése és az időzítés a game.c dolga.
//
// Időzítés-modell: a hívó frame-enként (tipikusan ~60 Hz) hívja a
// chip8_run(n)-t (n utasítás végrehajtása) és a chip8_tick_60hz()-t (delay/
// sound timer léptetés). A CHIP-8 ROM-ok nem definiálnak órajelet — az n a
// játszhatóság-hangolás helye (tipikusan 10–15 utasítás / frame).

#define CHIP8_W        64
#define CHIP8_H        32
#define CHIP8_KEYS     16
// A RAM 4 KB, a ROM a 0x200-tól töltődik — ennél nagyobb fájl nem CHIP-8 ROM.
#define CHIP8_MAX_ROM  (4096 - 0x200)

// Interpreter-viselkedési eltérések ("quirk"-ök) — a különböző korszakok
// interpretereihez írt ROM-ok más-más viselkedésre épülnek.
typedef struct {
    bool shift_vx;      // 8XY6/8XYE: true = CHIP-48/SCHIP (VX-et shifteli,
                        // VY-t ignorálja); false = eredeti COSMAC (VY → VX)
    bool ldstr_keep_i;  // FX55/FX65: true = CHIP-48/SCHIP (I változatlan);
                        // false = eredeti (I a végén X+1-gyel nő)
} chip8_quirks_t;

// A VM-állapot mérete (~6 KB). A magnak nincs saját statikus állapota — a
// hívó allokálja a munkaterületet (tipikusan PSRAM-ból: a belső RAM-ot az
// LVGL DMA-bufferei kifeszítik), és chip8_attach-csel köti be. Minden API
// no-op / hibát ad, amíg nincs bekötött állapot.
size_t chip8_state_bytes(void);
void   chip8_attach(void *state_mem);

// Teljes reset (RAM, regiszterek, framebuffer, gombok) + quirk-beállítás.
// q == NULL → default: CHIP-48/SCHIP profil (shift_vx + ldstr_keep_i) — a
// klasszikus 90-es évekbeli játékpakkok (Invaders, Tetris, Brix...) ezt várják.
void chip8_reset(const chip8_quirks_t *q);

// ROM betöltése a 0x200-ra. false, ha size == 0 vagy > CHIP8_MAX_ROM.
// chip8_reset után hívandó (nem reset-el magától).
bool chip8_load(const uint8_t *rom, size_t size);

// A CXNN (random) belső xorshift-jének seedelése — determinisztikus alapból,
// a hívó adjon valódi entrópiát (pl. esp_timer_get_time()).
void chip8_seed(uint32_t seed);

// n utasítás végrehajtása. Ismeretlen opcode / stack-hiba esetén a VM
// HALT-ba megy (chip8_halted) és a hátralévő utasítások kimaradnak.
// Visszaadja a ténylegesen végrehajtott utasítások számát.
int chip8_run(int n);

// Delay + sound timer léptetése — 60 Hz-enként hívandó.
void chip8_tick_60hz(void);

// Gomb-állapot beállítása (key: 0..15, az eredeti hex-keypad sorszáma).
void chip8_set_key(int key, bool down);

// true, amíg a sound timer > 0 — a hívó ebből csinál bípet / vizuális jelzést.
bool chip8_beeping(void);

// true, ha a VM hibára futott (ismeretlen opcode / stack alul-túlcsordulás).
bool chip8_halted(void);

// A 64×32-es framebuffer — 1 byte / pixel, 0 vagy 1, sorfolytonos.
const uint8_t *chip8_fb(void);

// true, ha a framebuffer változott az előző hívás óta (a flaget törli).
bool chip8_take_dirty(void);
