#include <string.h>

#include "chip8.h"

// -----------------------------------------------------------------------------
// CHIP-8 VM — a klasszikus 35 opcode. Referencia-viselkedés: a "modern
// konszenzus" (Timendus test suite): a sprite kezdő-koordinátája wrappel
// (mod 64/32), a kirajzolás a képernyő szélén klippel; a quirk-ölhető
// utasítások (8XY6/8XYE, FX55/FX65) viselkedését a chip8_quirks_t adja.
// -----------------------------------------------------------------------------

#define ROM_BASE   0x200
#define FONT_BASE  0x050

typedef struct {
    uint8_t  ram[4096];
    uint8_t  v[16];
    uint16_t i;
    uint16_t pc;
    uint16_t stack[16];
    uint8_t  sp;
    uint8_t  dt, st;                  // delay / sound timer
    uint8_t  fb[CHIP8_W * CHIP8_H];   // 1 byte / pixel (0/1)
    bool     keys[CHIP8_KEYS];
    chip8_rect_t rects[CHIP8_DIRTY_RECTS_MAX];   // felgyűlt dirty-rectek
    uint8_t  n_rects;
    bool     halted;
    int8_t   wait_key;     // FX0A: lenyomva észlelt kulcs, a felengedésére várunk (-1: nincs)
    uint32_t rng;
    chip8_quirks_t q;
} chip8_t;

// A ~6 KB-os állapot NEM statikus: a belső RAM-ot az LVGL DMA-bufferei már
// így is kifeszítik (a BSS-be téve a buf2 allokáció elbukott bootkor). A
// hívó allokálja (PSRAM-ból) és chip8_attach-csel köti be.
static chip8_t *C = NULL;

size_t chip8_state_bytes(void) { return sizeof(chip8_t); }

void chip8_attach(void *state_mem) { C = (chip8_t *)state_mem; }

static void mark_full_dirty(void);
static void mark_dirty(int x0, int y0, int x1, int y1);

// A beépített 0–F hex számjegy-sprite-ok (4×5 px), a FX29 ezekre mutat.
static const uint8_t FONT[80] = {
    0xF0, 0x90, 0x90, 0x90, 0xF0,   // 0
    0x20, 0x60, 0x20, 0x20, 0x70,   // 1
    0xF0, 0x10, 0xF0, 0x80, 0xF0,   // 2
    0xF0, 0x10, 0xF0, 0x10, 0xF0,   // 3
    0x90, 0x90, 0xF0, 0x10, 0x10,   // 4
    0xF0, 0x80, 0xF0, 0x10, 0xF0,   // 5
    0xF0, 0x80, 0xF0, 0x90, 0xF0,   // 6
    0xF0, 0x10, 0x20, 0x40, 0x40,   // 7
    0xF0, 0x90, 0xF0, 0x90, 0xF0,   // 8
    0xF0, 0x90, 0xF0, 0x10, 0xF0,   // 9
    0xF0, 0x90, 0xF0, 0x90, 0x90,   // A
    0xE0, 0x90, 0xE0, 0x90, 0xE0,   // B
    0xF0, 0x80, 0x80, 0x80, 0xF0,   // C
    0xE0, 0x90, 0x90, 0x90, 0xE0,   // D
    0xF0, 0x80, 0xF0, 0x80, 0xF0,   // E
    0xF0, 0x80, 0xF0, 0x80, 0x80,   // F
};

void chip8_reset(const chip8_quirks_t *q)
{
    if (!C) return;
    memset(C, 0, sizeof(*C));
    memcpy(C->ram + FONT_BASE, FONT, sizeof(FONT));
    C->pc       = ROM_BASE;
    C->wait_key = -1;
    C->rng      = 0x2EE6D6u;            // determinisztikus default; chip8_seed felülírja
    if (q) C->q = *q;
    else   C->q = (chip8_quirks_t){ .shift_vx = true, .ldstr_keep_i = true };
    mark_full_dirty();                  // az üres képernyő is kirajzolandó
}

// A teljes képernyő egyetlen rectként (CLS / reset).
static void mark_full_dirty(void)
{
    C->rects[0] = (chip8_rect_t){ 0, 0, CHIP8_W - 1, CHIP8_H - 1 };
    C->n_rects  = 1;
}

// Új dirty-téglalap felvétele. Átfedő/érintkező (±1 px) meglévőbe olvad —
// a sprite "töröl a régi helyen + rajzol mellé" párja így egy rect lesz.
// Tele listánál a legkisebb terület-növekedést okozó rectbe olvasztunk.
static void mark_dirty(int x0, int y0, int x1, int y1)
{
    for (int i = 0; i < C->n_rects; i++) {
        chip8_rect_t *r = &C->rects[i];
        if (x0 <= r->x1 + 1 && x1 + 1 >= r->x0 &&
            y0 <= r->y1 + 1 && y1 + 1 >= r->y0) {
            if (x0 < r->x0) r->x0 = (uint8_t)x0;
            if (y0 < r->y0) r->y0 = (uint8_t)y0;
            if (x1 > r->x1) r->x1 = (uint8_t)x1;
            if (y1 > r->y1) r->y1 = (uint8_t)y1;
            return;
        }
    }
    if (C->n_rects < CHIP8_DIRTY_RECTS_MAX) {
        C->rects[C->n_rects++] =
            (chip8_rect_t){ (uint8_t)x0, (uint8_t)y0, (uint8_t)x1, (uint8_t)y1 };
        return;
    }
    int best = 0;
    int best_grow = 0x7FFFFFFF;
    for (int i = 0; i < C->n_rects; i++) {
        const chip8_rect_t *r = &C->rects[i];
        int ux0 = x0 < r->x0 ? x0 : r->x0;
        int uy0 = y0 < r->y0 ? y0 : r->y0;
        int ux1 = x1 > r->x1 ? x1 : r->x1;
        int uy1 = y1 > r->y1 ? y1 : r->y1;
        int grow = (ux1 - ux0 + 1) * (uy1 - uy0 + 1)
                 - (r->x1 - r->x0 + 1) * (r->y1 - r->y0 + 1);
        if (grow < best_grow) { best_grow = grow; best = i; }
    }
    chip8_rect_t *r = &C->rects[best];
    if (x0 < r->x0) r->x0 = (uint8_t)x0;
    if (y0 < r->y0) r->y0 = (uint8_t)y0;
    if (x1 > r->x1) r->x1 = (uint8_t)x1;
    if (y1 > r->y1) r->y1 = (uint8_t)y1;
}

bool chip8_load(const uint8_t *rom, size_t size)
{
    if (!C || !rom || size == 0 || size > CHIP8_MAX_ROM) return false;
    memcpy(C->ram + ROM_BASE, rom, size);
    return true;
}

void chip8_seed(uint32_t seed)
{
    if (C && seed) C->rng = seed;       // a xorshift 0-ból nem mozdul ki
}

static uint8_t rnd8(void)
{
    uint32_t x = C->rng;                // xorshift32
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    C->rng = x;
    return (uint8_t)x;
}

void chip8_set_key(int key, bool down)
{
    if (C && key >= 0 && key < CHIP8_KEYS) C->keys[key] = down;
}

void chip8_tick_60hz(void)
{
    if (!C) return;
    if (C->dt) C->dt--;
    if (C->st) C->st--;
}

bool chip8_beeping(void)       { return C && C->st > 0; }
bool chip8_halted(void)        { return C && C->halted; }
const uint8_t *chip8_fb(void)  { return C ? C->fb : NULL; }

int chip8_take_dirty_rects(chip8_rect_t *out, int max)
{
    if (!C || !out) return 0;
    int n = C->n_rects < max ? C->n_rects : max;
    for (int i = 0; i < n; i++) out[i] = C->rects[i];
    C->n_rects = 0;
    return n;
}

// DXYN: 8 px széles, n px magas sprite XOR-rajzolása az I-ről. A kezdő-
// koordináta wrappel, a sprite a szélén klippel. VF = volt-e 1→0 átmenet.
static void draw_sprite(int vx, int vy, int n)
{
    int xs = vx % CHIP8_W;
    int ys = vy % CHIP8_H;
    // A ténylegesen átbillentett pixelek befoglalója (XOR 1-gyel mindig
    // változtat, tehát minden kirajzolt set-bit dirty).
    int mx0 = CHIP8_W, my0 = CHIP8_H, mx1 = -1, my1 = -1;
    C->v[0xF] = 0;
    for (int row = 0; row < n; row++) {
        int py = ys + row;
        if (py >= CHIP8_H) break;
        uint8_t bits = C->ram[(C->i + row) & 0xFFF];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (0x80u >> col))) continue;
            int px = xs + col;
            if (px >= CHIP8_W) break;
            uint8_t *p = &C->fb[py * CHIP8_W + px];
            if (*p) C->v[0xF] = 1;
            *p ^= 1;
            if (px < mx0) mx0 = px;
            if (px > mx1) mx1 = px;
            if (py < my0) my0 = py;
            if (py > my1) my1 = py;
        }
    }
    if (mx1 >= 0) mark_dirty(mx0, my0, mx1, my1);
}

// Egy utasítás végrehajtása. false = HALT (a hívó loop kilép).
static bool step(void)
{
    uint16_t op = (uint16_t)(C->ram[C->pc & 0xFFF] << 8) | C->ram[(C->pc + 1) & 0xFFF];
    C->pc = (C->pc + 2) & 0xFFF;

    int x   = (op >> 8) & 0xF;
    int y   = (op >> 4) & 0xF;
    int nn  = op & 0xFF;
    int nnn = op & 0xFFF;

    switch (op >> 12) {
    case 0x0:
        if (op == 0x00E0) {                         // CLS
            memset(C->fb, 0, sizeof(C->fb));
            mark_full_dirty();
        } else if (op == 0x00EE) {                  // RET
            if (C->sp == 0) { C->halted = true; return false; }
            C->pc = C->stack[--C->sp];
        }
        // egyéb 0NNN (SYS): ignorálva
        break;

    case 0x1: C->pc = nnn; break;                    // JP NNN

    case 0x2:                                       // CALL NNN
        if (C->sp >= 16) { C->halted = true; return false; }
        C->stack[C->sp++] = C->pc;
        C->pc = nnn;
        break;

    case 0x3: if (C->v[x] == nn)     C->pc = (C->pc + 2) & 0xFFF; break;   // SE VX, NN
    case 0x4: if (C->v[x] != nn)     C->pc = (C->pc + 2) & 0xFFF; break;   // SNE VX, NN
    case 0x5:
        if ((op & 0xF) != 0) { C->halted = true; return false; }
        if (C->v[x] == C->v[y]) C->pc = (C->pc + 2) & 0xFFF;                // SE VX, VY
        break;
    case 0x9:
        if ((op & 0xF) != 0) { C->halted = true; return false; }
        if (C->v[x] != C->v[y]) C->pc = (C->pc + 2) & 0xFFF;                // SNE VX, VY
        break;

    case 0x6: C->v[x] = (uint8_t)nn; break;                              // LD VX, NN
    case 0x7: C->v[x] = (uint8_t)(C->v[x] + nn); break;                   // ADD VX, NN

    case 0x8: {
        // A VF-et MINDIG a művelet után állítjuk (X lehet maga az F is).
        switch (op & 0xF) {
        case 0x0: C->v[x] = C->v[y]; break;
        case 0x1: C->v[x] |= C->v[y]; break;
        case 0x2: C->v[x] &= C->v[y]; break;
        case 0x3: C->v[x] ^= C->v[y]; break;
        case 0x4: {                                                     // ADD: VF = carry
            int sum = C->v[x] + C->v[y];
            C->v[x] = (uint8_t)sum;
            C->v[0xF] = sum > 0xFF;
            break;
        }
        case 0x5: {                                                     // SUB: VF = NOT borrow
            bool nb = C->v[x] >= C->v[y];
            C->v[x] = (uint8_t)(C->v[x] - C->v[y]);
            C->v[0xF] = nb;
            break;
        }
        case 0x7: {                                                     // SUBN: VF = NOT borrow
            bool nb = C->v[y] >= C->v[x];
            C->v[x] = (uint8_t)(C->v[y] - C->v[x]);
            C->v[0xF] = nb;
            break;
        }
        case 0x6: {                                                     // SHR
            uint8_t src = C->q.shift_vx ? C->v[x] : C->v[y];
            C->v[x] = src >> 1;
            C->v[0xF] = src & 1;
            break;
        }
        case 0xE: {                                                     // SHL
            uint8_t src = C->q.shift_vx ? C->v[x] : C->v[y];
            C->v[x] = (uint8_t)(src << 1);
            C->v[0xF] = (src >> 7) & 1;
            break;
        }
        default: C->halted = true; return false;
        }
        break;
    }

    case 0xA: C->i = nnn; break;                                         // LD I, NNN
    case 0xB: C->pc = (nnn + C->v[0]) & 0xFFF; break;                     // JP V0, NNN
    case 0xC: C->v[x] = rnd8() & nn; break;                              // RND VX, NN
    case 0xD: draw_sprite(C->v[x], C->v[y], op & 0xF); break;             // DRW VX, VY, N

    case 0xE:
        if (nn == 0x9E) {                                               // SKP VX
            if (C->keys[C->v[x] & 0xF])  C->pc = (C->pc + 2) & 0xFFF;
        } else if (nn == 0xA1) {                                        // SKNP VX
            if (!C->keys[C->v[x] & 0xF]) C->pc = (C->pc + 2) & 0xFFF;
        } else { C->halted = true; return false; }
        break;

    case 0xF:
        switch (nn) {
        case 0x07: C->v[x] = C->dt; break;                                // LD VX, DT
        case 0x0A:                                                      // LD VX, K
            // Lenyomás MAJD felengedés kell — tartott gombbal nem pörög át.
            if (C->wait_key >= 0) {
                if (!C->keys[C->wait_key]) {
                    C->v[x] = (uint8_t)C->wait_key;
                    C->wait_key = -1;
                    break;                       // megvan a kulcs, mehetünk tovább
                }
            } else {
                for (int k = 0; k < CHIP8_KEYS; k++) {
                    if (C->keys[k]) { C->wait_key = (int8_t)k; break; }
                }
            }
            C->pc = (C->pc - 2) & 0xFFF;           // várakozás: ugyanide térünk vissza
            break;
        case 0x15: C->dt = C->v[x]; break;                                // LD DT, VX
        case 0x18: C->st = C->v[x]; break;                                // LD ST, VX
        case 0x1E: C->i = (C->i + C->v[x]) & 0xFFF; break;                 // ADD I, VX
        case 0x29: C->i = FONT_BASE + (C->v[x] & 0xF) * 5; break;         // LD F, VX
        case 0x33:                                                      // LD B, VX (BCD)
            C->ram[C->i & 0xFFF]       = C->v[x] / 100;
            C->ram[(C->i + 1) & 0xFFF] = (C->v[x] / 10) % 10;
            C->ram[(C->i + 2) & 0xFFF] = C->v[x] % 10;
            break;
        case 0x55:                                                      // LD [I], V0..VX
            for (int r = 0; r <= x; r++) C->ram[(C->i + r) & 0xFFF] = C->v[r];
            if (!C->q.ldstr_keep_i) C->i = (C->i + x + 1) & 0xFFF;
            break;
        case 0x65:                                                      // LD V0..VX, [I]
            for (int r = 0; r <= x; r++) C->v[r] = C->ram[(C->i + r) & 0xFFF];
            if (!C->q.ldstr_keep_i) C->i = (C->i + x + 1) & 0xFFF;
            break;
        default: C->halted = true; return false;
        }
        break;
    }
    return true;
}

// Frame-vágási heurisztika: a frame ott ér véget, ahol egy rajzolási szakasz
// lezárult — volt már DXYN, és utána FRAME_CUT_GAP-nél több nem-rajzoló
// utasítás futott (a játékciklus "logika" fázisa). Így egy sok-sprite-os
// újrarajzolás (sűrűn követő DXYN-ek, pl. az Invaders teljes rácsmenete:
// draw-ok közt ~14 utasítás) EGY frame-en belül marad, miközben a ritkásabb
// rajzolású játékciklus (pl. a lövedék "töröl+rajzol" párja után ~17+
// utasítás logika) frame-enként egy iterációt halad — a megjelenítés sima,
// a játéktempó nem skálázódik a büdzsével. Empirikus küszöb: az Invaders
// rács-menete 8-14-es, a lövedék-iterációja 17-es gappel rajzol; a vágásnak
// a kettő közé kell esnie (a since_draw a köztes utasítások száma, ami a
// gap-1 maximumot éri el a következő draw előtt).
#define FRAME_CUT_GAP  15

int chip8_run_frame(int budget, int max_budget)
{
    if (!C) return 0;
    int executed   = 0;
    int since_draw = 0;
    bool drew      = false;
    while (!C->halted) {
        if (drew && since_draw > FRAME_CUT_GAP) break;   // rajzolási szakasz vége
        if (!drew && executed >= budget) break;          // rajzolás nélküli frame
        if (executed >= max_budget) break;               // védőkorlát
        uint16_t op = (uint16_t)(C->ram[C->pc & 0xFFF] << 8)
                    | C->ram[(C->pc + 1) & 0xFFF];
        bool is_draw = (op >> 12) == 0xD;
        if (!step()) break;
        executed++;
        if (is_draw) {
            drew = true;
            since_draw = 0;
        } else {
            since_draw++;
        }
    }
    return executed;
}
