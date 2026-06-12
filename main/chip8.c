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
    bool     dirty;
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
    C->dirty = true;                    // az üres képernyő is kirajzolandó
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

bool chip8_take_dirty(void)
{
    if (!C) return false;
    bool d = C->dirty;
    C->dirty = false;
    return d;
}

// DXYN: 8 px széles, n px magas sprite XOR-rajzolása az I-ről. A kezdő-
// koordináta wrappel, a sprite a szélén klippel. VF = volt-e 1→0 átmenet.
static void draw_sprite(int vx, int vy, int n)
{
    int xs = vx % CHIP8_W;
    int ys = vy % CHIP8_H;
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
        }
    }
    C->dirty = true;
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
            C->dirty = true;
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

int chip8_run(int n)
{
    if (!C) return 0;
    int done = 0;
    while (done < n && !C->halted) {
        if (!step()) break;
        done++;
    }
    return done;
}
