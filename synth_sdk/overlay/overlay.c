/* Overlay: roll (seq/roll.c) and drive (plugins/drive) on top of another firmware image built on op1r, e.g. a
 * hundekraft build, without its sources. Built and patched in by make_overlay.py.
 *
 * Each hook does its own part and then calls what the base image had at that site: the next_* symbols are read
 * from the base image by make_overlay.py (CALL / JUMP.L targets, vtable words, the FX screen stub) and given to the
 * linker. Only stock R246 addresses appear here; nothing of the base image's own code is hard-coded.
 *
 * roll:  SeqIds::from_name / ::name (type 5 = arpeggio + rl_active[mode]), the arpeggio player getter (CALL
 *        0x01A46400 @0x01A0D1B0 -> rl_player_get, seq/roll.c), the sequencer clock reset (JUMP.L @0x01A5E7D0 ->
 *        rl_clock_reset below), a screen object in the arpeggio slot 0x2C, presses and turns on that screen.
 * drive: runs in the hidden filter FX (type 5) like op1r's FX, with its own selector DV_MARK (outside op1r's plugin
 *        numbers, so it never takes the place of the base's FX): fx_types name compare, the filter vtable words
 *        (mono / stereo render and set_position), ScreenId fx case 5 (screen object in slot 0x0B), presses and turns
 *        on its page. */
#include "../op1_synth.h"

#define DV_MARK 0x5340                          /* drive's selector (params[4]); op1r uses 0x5301..0x533F */

int32_t op1_db_last_err;                        /* op1_db_write (op1_synth.h), used by seq/roll.c */
volatile uint8_t op1_fx_page[3][4];             /* plugins/drive: page 2 per FX position */

typedef struct { uint32_t alloc; union { char buf[16]; char *ptr; } u; uint32_t size, cap; } dstring;

/* the base image's hooks (make_overlay.py, --defsym) */
extern int         next_seq_from_name(const dstring *s);
extern const char *next_seq_name(int type);
extern void       *next_screens_ctor(void);
extern void        next_screens_load(void *table);
extern void        next_key(int code, int pressed);
extern void        next_knob(int knob, int16_t *delta);
extern int         next_fx_name_eq(const char *name, const dstring *s);
extern void        next_fx_mono(void *self, int16_t *buf, const int16_t *p);
extern void        next_fx_mono_pos(void *self, int pos);
extern void        next_fx_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p);
extern void        next_fx_stereo_pos(void *self, int pos);
extern int         next_fx_screen(void);

/* seq/roll.c, plugins/drive/drive.c */
extern volatile uint8_t rl_active[2];
extern const op1_screen_t SCREEN_roll, SCREEN_drive;
extern const op1_fx_t FX_drive;
void rl_press(int pos, int knob);
void rl_turn(int pos, int knob, int delta);

/* stock R246 */
#define MODE()          (*(volatile int *)0x01DE4C40 == 1)       /* 0 synth, 1 drum */
#define CURRENT_SCREEN  (*(volatile int *)0x018B65C8)
#define SCREEN_TABLE    ((void **)0x01DF4E54)
#define SCREEN_CTOR     ((void *(*)(void *, int, const char *))0x01A3BAB0)
#define ITER_SCREEN_VT  ((const uint32_t *)0x019103FC)
#define SVG_LOAD        ((void *(*)(void *))0x01A503D8)
#define PATCH_OBJ(drum, slot) ((void *)((drum ? 0x01DDD9A8 : 0x01DDD7E8) + ((slot) & 7) * 0x38))
#define PATCH_SET_FX    ((void (*)(void *, const int16_t *))0x019FF664)
#define ARP_SCREEN_ID   0x2C
#define FX_SCREEN_ID    0x0B

static int streq_n(const char *a, const char *b, uint32_t n)       /* a: n chars (not terminated), b: C string */
{
    uint32_t i = 0;
    for (; i < n; i++) if (a[i] != b[i] || !b[i]) return 0;
    return b[n] == 0;
}
static const char *dstr(const dstring *s) { return s->cap > 15 ? s->u.ptr : s->u.buf; }

/* ======================= screens: objects like op1r's (iter screen class, own update / init) ======================= */
static uint32_t rl_scr[32], rl_vt[10], dv_scr[32], dv_vt[10];
static void *arp_screen;
static uint8_t rl_ready, dv_ready;

static void screen_obj(uint32_t *obj, uint32_t *vt, int id, const char *svg, const op1_screen_t *sc)
{
    void *saved = SCREEN_TABLE[id];
    SCREEN_CTOR(obj, id, svg);                  /* registers itself in the table ... */
    SCREEN_TABLE[id] = saved;                   /* ... undo: the base keeps the slot */
    for (int i = 0; i < 10; i++) vt[i] = ITER_SCREEN_VT[i];
    vt[3 * 2 + 1] = (uint32_t)sc->update;       /* +0x1c */
    vt[4 * 2 + 1] = (uint32_t)sc->init;         /* +0x24 */
    obj[3] = (uint32_t)vt;
}

void *ov_screens_ctor(void)                     /* CALL @0x01A3BD58 */
{
    void *r = next_screens_ctor();
    screen_obj(rl_scr, rl_vt, ARP_SCREEN_ID, "roll.svg", &SCREEN_roll);
    screen_obj(dv_scr, dv_vt, FX_SCREEN_ID, "drive.svg", &SCREEN_drive);
    return r;
}

static void rl_sync(void)                       /* the roll screen in the arpeggio slot while roll is this mode's */
{
    if (rl_ready) SCREEN_TABLE[ARP_SCREEN_ID] = rl_active[MODE()] ? (void *)rl_scr : arp_screen;
}

void ov_screens_load(void *table)               /* CALL @0x019F07D2 (boot: load every screen) */
{
    next_screens_load(table);
    if (rl_scr[2]) {
        rl_scr[0] = (uint32_t)SVG_LOAD((void *)rl_scr[2]);
        arp_screen = SCREEN_TABLE[ARP_SCREEN_ID];
        if (rl_scr[0] && arp_screen) { SCREEN_roll.init(rl_scr); rl_ready = 1; rl_sync(); }   /* (loads saved rolls) */
    }
    if (dv_scr[2]) {
        dv_scr[0] = (uint32_t)SVG_LOAD((void *)dv_scr[2]);
        if (dv_scr[0]) { SCREEN_drive.init(dv_scr); dv_ready = 1; }
    }
}

/* ======================= roll: sequencer names ======================= */
static const dstring arp_name = { 0, { "arpeggio" }, 8, 15 };

int ov_seq_from_name(const dstring *s)          /* 3 CALLs of SeqIds::from_name */
{
    if (streq_n(dstr(s), "roll", s->size)) {
        next_seq_from_name(&arp_name);          /* the base clears its own sequencer flags for this mode */
        rl_active[MODE()] = 1;
        return 5;
    }
    rl_active[MODE()] = 0;
    return next_seq_from_name(s);
}

const char *ov_seq_name(int type)               /* JUMP.L at SeqIds::name */
{
    if (type == 5 && rl_active[MODE()]) return "roll";
    return next_seq_name(type);
}

static int roll_page(void)
{
    rl_sync();
    return rl_ready && rl_active[MODE()] && CURRENT_SCREEN == ARP_SCREEN_ID;
}

/* replacement for the stock sequencer-clock reset 0x01A5E7D0: the same three stores, plus the flag for roll's beat
 * tracking (seq/roll.c rl_on_beat). Only P1 and R0, as the original. */
__asm__(
"    .text\n"
"    .align 2\n"
"    .global _rl_clock_reset\n"
"_rl_clock_reset:\n"
"    P1.L = 0x187C;\n"
"    P1.H = 0x018C;\n"
"    R0 = 0 (X);\n"
"    [P1] = R0;\n"
"    [P1 + 4] = R0;\n"
"    [P1 + 8] = R0;\n"
"    P1.L = _rl_reset_pending;\n"
"    P1.H = _rl_reset_pending;\n"
"    R0 = 1 (X);\n"
"    B [P1] = R0;\n"
"    rts;\n"
);

/* ======================= drive: the hidden filter FX with selector DV_MARK ======================= */
static const int16_t fx_rng_default[4][2] = { { 64, 14528 }, { 0, 24576 }, { 0, 22528 }, { 8192, 32767 } };

static int dv_is_drive(int pos)                 /* the FX this position runs is drive */
{
    int type = pos == OP1_FX_MASTER ? *(volatile int *)0xFF801090
             : *(volatile int *)((pos ? 0xFF801754 : 0xFF801734) + (*(volatile int *)(pos ? 0xFF8016D8 : 0xFF8016D4) & 7) * 4);
    return type == 5 && op1_fx_knobs(pos)[4] == DV_MARK;
}

static void dv_clamp(int pos)                   /* words 0..3 inside the filter's ranges (read_fx_param asserts) */
{
    int16_t *p = (int16_t *)op1_fx_knobs(pos);
    for (int i = 0; i < 4; i++) {
        const int16_t *rg = (const int16_t *)(0x01DE5C50 + 5 * 0x30 + i * 6);
        int lo = rg[0], hi = rg[1];
        if (hi <= lo) { lo = fx_rng_default[i][0]; hi = fx_rng_default[i][1]; }
        if (p[i] < lo) p[i] = (int16_t)lo; else if (p[i] > hi) p[i] = (int16_t)hi;
    }
}

static int dv_page(void)
{
    return dv_ready && CURRENT_SCREEN == FX_SCREEN_ID && SCREEN_TABLE[FX_SCREEN_ID] == (void *)dv_scr
           && dv_is_drive(op1_fx_screen_pos());
}

int ov_fx_name_eq(const char *name, const dstring *s)     /* fx_types name compare: "drive" is a "filter" */
{
    if (streq_n(dstr(s), "drive", s->size) && name[0] == 'f' && name[1] == 'i' && name[2] == 'l' && name[3] == 't'
        && name[4] == 'e' && name[5] == 'r' && !name[6])
        return 1;
    return next_fx_name_eq(name, s);
}

static int dv_mono_pos;                         /* the mono filter object's position: 0 synth, 1 drum */
static uint8_t dv_mono_on, dv_st_on;            /* drive ran last (else: (re)initialise it first) */

void ov_fx_mono(void *self, int16_t *buf, const int16_t *p)
{
    if (p[4] != DV_MARK) { dv_mono_on = 0; next_fx_mono(self, buf, p); return; }
    if (!dv_mono_on) { dv_mono_on = 1; FX_drive.init(dv_mono_pos); }
    FX_drive.render(dv_mono_pos, buf, p);
}

void ov_fx_mono_pos(void *self, int pos)
{
    dv_mono_pos = pos & 1;
    dv_mono_on = 0;
    next_fx_mono_pos(self, pos);
}

void ov_fx_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p)
{
    if (p[4] != DV_MARK) { dv_st_on = 0; next_fx_stereo(self, l, r, p); return; }
    if (!dv_st_on) { dv_st_on = 1; FX_drive.init(OP1_FX_MASTER); }
    FX_drive.render_stereo(l, r, p);
}

void ov_fx_stereo_pos(void *self, int pos)
{
    dv_st_on = 0;
    next_fx_stereo_pos(self, pos);
}

/* ScreenId fx case 5: the stub ov_fx_case (below) runs inside the switch function's frame, like the base's */
int ov_fx_screen_id(void)
{
    int id = next_fx_screen();                  /* the base puts its own / the stock screen into slot 0x0B */
    if (dv_ready) {
        int pos = op1_fx_screen_pos();
        if (dv_is_drive(pos)) { dv_clamp(pos); SCREEN_TABLE[FX_SCREEN_ID] = dv_scr; }
    }
    return id;
}
__asm__(
"    .text\n"
"    .align 2\n"
"    .global _ov_fx_case\n"
"_ov_fx_case:\n"
"    CALL _ov_fx_screen_id;\n"
"    P0 = [FP + 4];\n"
"    UNLINK;\n"
"    JUMP (P0);\n"
);

/* ======================= keys and encoders (KeyHandler thread): ours first, then the base's chain ============== */
void ov_key(int code, int pressed)              /* CALL @0x019C2D3E */
{
    if (code >= 0x3B && code <= 0x3E) {         /* encoder presses blue..red */
        if (roll_page()) { if (pressed) rl_press(MODE(), code - 0x3B); return; }
        if (dv_page()) { FX_drive.key(op1_fx_screen_pos(), code - 0x3B, pressed); return; }
    }
    next_key(code, pressed);
    rl_sync();                                  /* mode / sequencer may have changed */
}

void ov_knob(int knob, int16_t *delta)          /* CALL @0x019C2E94 */
{
    if (knob >= 0 && knob < 4) {
        if (roll_page()) { rl_turn(MODE(), knob, *delta); return; }
        if (dv_page()) {
            int pos = op1_fx_screen_pos();
            int16_t *w = (int16_t *)op1_fx_knobs(pos), before[8];
            for (int i = 0; i < 8; i++) before[i] = w[i];
            if (FX_drive.knob(pos, knob, *delta, w)) {
                dv_clamp(pos);
                for (int i = 0; i < 8; i++)
                    if (w[i] != before[i]) {            /* into the sound (master: saved from the live words) */
                        if (pos != OP1_FX_MASTER)
                            PATCH_SET_FX(PATCH_OBJ(pos, *(volatile int *)(pos ? 0xFF8016D8 : 0xFF8016D4)), w);
                        break;
                    }
                return;
            }
        }
    }
    next_knob(knob, delta);
}
