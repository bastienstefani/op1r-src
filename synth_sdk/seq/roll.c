/* "roll" piano-roll sequencer: notes with their own start, length, pitch and velocity on a grid, in a loop of settable
 * length; polyphonic playback; one pattern per mode (synth / drum), saved.
 *
 * Screen (arpeggio slot 0x2C): grid of 16 columns x 16 pitch rows, notes as bars, time cursor (blue), playhead (red);
 * header: grid, length of new notes (fraction of a whole note), loop length in grid steps; HOLD box top right.
 * Encoders: blue time cursor (grid steps) · green pitch scroll · white length of new notes · red loop length
 * Presses:  blue cycles the grid 1/4 1/8 1/16 1/32 · green clears the notes under the cursor · white cycles the
 *           velocity of new notes 127 / 100 / 64 · red play / hold (HOLD box faded red = held, red = playing / armed)
 * Note key: places a note of that pitch at the cursor with the current length and velocity, then moves the cursor
 * on by that length (step input); the key also sounds. Shift + key removes the note of that pitch under the cursor.
 * Pitch: key + 53 + 12 * octave of the synth slot; drums: key + 53 (the drum sound), played at the drum octave.
 * Starts empty, one bar of 16 steps. Play / hold is not saved (boots held); an external stop (sequencer off, another
 * sequencer or mode) holds. Playback always restarts at the top of the loop.
 *
 * Beat-aligned start: an armed roll starts on the next beat (quarter note) of the stock sequencer clock. The player
 * tick gets the clock's counter modulo 48 (half a beat); rl_on_beat counts its wraps. Beat 1 is set by the last
 * clock reset 0x01A5E7D0 (tape PLAY, MIDI Start, a stock sequencer starting), replaced by rl_clock_reset (hook.S):
 * the same three stores plus rl_reset_pending, so a reset that coincides with the counter's own wrap is not missed.
 * The metronome click itself runs in the encrypted L1 code: aligned with it only after a MIDI Start.
 *
 * Saved (bg_saver, DB table rollseq, one row per mode, key 0 = synth, 1 = drum): loop, grid, velocity, view top,
 * length of new notes, then start, length, pitch, velocity per note, in decimal (no '%' or quote in the SQL).
 *
 * This file is platform independent above the firmware glue (OP1_HOST: harness/roll_test.c). */
#include "../op1_synth.h"

#define RL_MAXNOTES 128                         /* per mode */
#define RL_POLY     16                          /* notes sounding at once (more are not played) */
#define RL_PPQ      96                          /* player ticks per quarter note */
#define RL_MAXLOOP  (RL_PPQ * 64)               /* 16 bars of 4/4 */
#define RL_ROWS     16                          /* pitch rows on screen */
#define RL_WINS     16                          /* grid columns on screen */

typedef struct { uint16_t start, len; uint8_t pitch, vel, used, pad; } rl_note_t;

static const uint16_t rl_grids[4] = { RL_PPQ, RL_PPQ / 2, RL_PPQ / 4, RL_PPQ / 8 };   /* 1/4 1/8 1/16 1/32 */
static const uint8_t  rl_vels[3]  = { 127, 100, 64 };

typedef struct {
    rl_note_t note[RL_MAXNOTES];
    uint16_t loop_len;                          /* ticks */
    uint16_t cur_t;                             /* time cursor, ticks */
    uint16_t cur_len;                           /* length of new notes */
    uint8_t  grid_i, vel_i;                     /* index into rl_grids / rl_vels */
    int8_t   view_top;                          /* pitch of the top row */
    uint8_t  inited;
    volatile uint8_t run;                       /* play requested (red press); not saved */
    uint16_t play_t;                            /* playback: sequencer thread only */
    uint8_t  playing;
    struct { uint8_t note, used; uint16_t off_t; } snd[RL_POLY];
} rl_bank_t;

static rl_bank_t rl[2];                         /* 0 synth, 1 drum */

#ifdef OP1_HOST
int host_rl_changed;
void host_rl_note_on(int pos, int note, int vel);
void host_rl_note_off(int pos, int note);
#define RL_CHANGED()             (host_rl_changed++)
#define RL_NOTE_ON(pos, n, v)    host_rl_note_on((pos), (n), (v))
#define RL_NOTE_OFF(pos, n)      host_rl_note_off((pos), (n))
#else
static void rl_changed(void);
/* notes through VoiceCtl like the stock sequencers: key_down 0x01A85750 {midi, vel, send midi = 1}, param 0 (drums:
 * the sound is midi - 0x35, played at the current drum octave); key_up 0x01A85840 as euclid does (seq/euclid.c).
 * Only stock addresses: this file also builds into the overlay (overlay/, make_overlay.py). */
#define VC_DOWN ((void (*)(int32_t *, int))0x01A85750)
#define VC_UP   ((void (*)(int32_t *))0x01A85840)
static void rl_fw_note_on(int midi, int vel)
{
    int32_t n[4] = { midi, vel, 0, 0 };
    ((uint8_t *)n)[8] = 1;
    VC_DOWN(n, 0);
}
static void rl_fw_note_off(int midi)
{
    int32_t n[4] = { midi, 0, 1, 0 };
    ((uint8_t *)n)[8] = 1;
    VC_UP(n);
}
#define RL_CHANGED()             rl_changed()
#define RL_NOTE_ON(pos, n, v)    rl_fw_note_on((n), (v))
#define RL_NOTE_OFF(pos, n)      rl_fw_note_off(n)
#endif

static uint16_t rl_grid(const rl_bank_t *b) { return rl_grids[b->grid_i & 3]; }
static uint16_t rl_end(const rl_note_t *n) { return (uint16_t)(n->start + (n->len ? n->len : 1)); }

static void rl_bank_init(rl_bank_t *b)
{
    if (b->inited) return;
    for (int i = 0; i < RL_MAXNOTES; i++) b->note[i].used = 0;
    for (int i = 0; i < RL_POLY; i++) b->snd[i].used = 0;
    b->grid_i = 2;                              /* 1/16 */
    b->vel_i = 1;                               /* 100 */
    b->loop_len = RL_PPQ / 4 * 16;              /* one bar of 16 steps */
    b->cur_len = RL_PPQ / 4;
    b->cur_t = 0;
    b->view_top = 72;
    b->play_t = 0;
    b->playing = 0;
    b->run = 0;
    b->inited = 1;
}

/* ---- beat tracking on the stock sequencer clock (sequencer thread) ---- */
volatile uint8_t rl_reset_pending;              /* set by rl_clock_reset (hook.S), any thread */
static uint8_t rl_last48 = 0xff;                /* 0xff: nothing seen yet */
static uint8_t rl_half;                         /* 0: first half of the beat, 1: second half */
static int rl_on_beat(int32_t tick48)
{
    unsigned t = (unsigned)tick48 % 48;
    if (rl_reset_pending) {                     /* the clock was reset: beat 1 starts at its first tick */
        rl_reset_pending = 0;
        rl_half = 0;
    } else if (rl_last48 != 0xff && t == (rl_last48 + 1u) % 48) {
        if (t == 0) rl_half ^= 1;
    } else if (t == 0) {
        rl_half = 0;                            /* jumped to 0 without the hook seeing it: treat as a reset */
    }
    rl_last48 = (uint8_t)t;
    return t == 0 && rl_half == 0;
}

/* ---- playback (sequencer thread) ---- */
static void rl_off_all(int pos)
{
    rl_bank_t *b = &rl[pos];
    for (int i = 0; i < RL_POLY; i++)
        if (b->snd[i].used) { RL_NOTE_OFF(pos, b->snd[i].note); b->snd[i].used = 0; }
}

void rl_tick(int pos, int32_t tick48)           /* 96 per quarter note, current mode only */
{
    rl_bank_t *b = &rl[pos];
    int beat = rl_on_beat(tick48);              /* track every tick, also on hold */
    if (!b->run) {                              /* hold: release once, then wait for the red press */
        if (b->playing) { rl_off_all(pos); b->play_t = 0; b->playing = 0; }
        return;
    }
    if (!b->playing) {                          /* armed: start at the top of the loop on the next beat */
        if (!beat) return;
        b->play_t = 0;
        b->playing = 1;
    }
    uint16_t t = b->play_t;
    for (int i = 0; i < RL_POLY; i++)           /* note-offs at the end of their gate */
        if (b->snd[i].used && b->snd[i].off_t == t) { RL_NOTE_OFF(pos, b->snd[i].note); b->snd[i].used = 0; }
    for (int i = 0; i < RL_MAXNOTES; i++) {     /* note-ons; only with a free gate slot (else it would hang) */
        rl_note_t *n = &b->note[i];
        if (!n->used || n->start != t || n->start >= b->loop_len || n->pitch >= 128) continue;
        uint16_t off = rl_end(n);
        if (off > b->loop_len) off = b->loop_len;
        for (int j = 0; j < RL_POLY; j++)
            if (!b->snd[j].used) {
                RL_NOTE_ON(pos, n->pitch, n->vel ? n->vel : 100);
                b->snd[j].note = n->pitch; b->snd[j].off_t = off; b->snd[j].used = 1;
                break;
            }
    }
    if (++b->play_t >= b->loop_len) {           /* wrap: release everything (no hanging notes) */
        b->play_t = 0;
        rl_off_all(pos);
    }
}

void rl_stop_all(void)                          /* any external stop: both modes released and held; idempotent */
{
    for (int m = 0; m < 2; m++) {
        rl_off_all(m);
        rl[m].play_t = 0;
        rl[m].playing = 0;
        rl[m].run = 0;
    }
}

/* ---- editing (KeyHandler thread) ---- */
static int rl_note_at(const rl_bank_t *b, int pitch, uint16_t t)   /* used note of this pitch covering t, or -1 */
{
    for (int i = 0; i < RL_MAXNOTES; i++) {
        const rl_note_t *n = &b->note[i];
        if (n->used && n->pitch == pitch && n->start <= t && t < rl_end(n)) return i;
    }
    return -1;
}

static void rl_place(rl_bank_t *b, int pitch, uint16_t t, uint16_t len, uint8_t vel)
{
    int slot = -1;
    if (pitch < 0 || pitch > 127) return;
    for (int i = 0; i < RL_MAXNOTES; i++)       /* same start + pitch: replace */
        if (b->note[i].used && b->note[i].pitch == pitch && b->note[i].start == t) { slot = i; break; }
    if (slot < 0)
        for (int i = 0; i < RL_MAXNOTES; i++) if (!b->note[i].used) { slot = i; break; }
    if (slot < 0) return;                       /* full */
    b->note[slot].used = 0;                     /* fields before the used flag (read by the sequencer thread) */
    b->note[slot].start = t;
    b->note[slot].len = len;
    b->note[slot].pitch = (uint8_t)pitch;
    b->note[slot].vel = vel;
    b->note[slot].used = 1;
}

/* note key on the roll screen: pitch already includes the octave. -> 1 = consumed (no live note) */
int rl_key(int pos, int pitch, int shift)
{
    rl_bank_t *b = &rl[pos];
    rl_bank_init(b);
    if (pitch < 0 || pitch > 127) return shift;
    if (shift) {                                /* remove; no live note */
        int i = rl_note_at(b, pitch, b->cur_t);
        if (i >= 0) { b->note[i].used = 0; RL_CHANGED(); }
        return 1;
    }
    rl_place(b, pitch, b->cur_t, b->cur_len, rl_vels[b->vel_i % 3]);
    b->view_top = (int8_t)(pitch + 2 > 127 ? 127 : pitch + 2);                     /* keep it in view */
    b->cur_t = (uint16_t)((b->cur_t + b->cur_len) % b->loop_len);                  /* step input */
    RL_CHANGED();
    return 0;                                   /* also sounds live */
}

void rl_press(int pos, int knob)                /* encoder press, knob 0..3 = blue..red */
{
    rl_bank_t *b = &rl[pos];
    rl_bank_init(b);
    switch (knob) {
    case 0:                                     /* blue: grid */
        b->grid_i = (b->grid_i + 1) & 3;
        if (b->cur_len < rl_grid(b)) b->cur_len = rl_grid(b);
        break;
    case 1:                                     /* green: clear the notes under the cursor */
        for (int i = 0; i < RL_MAXNOTES; i++) {
            rl_note_t *n = &b->note[i];
            if (n->used && n->start <= b->cur_t && b->cur_t < rl_end(n)) n->used = 0;
        }
        break;
    case 2:                                     /* white: velocity of new notes */
        b->vel_i = (b->vel_i + 1) % 3;
        break;
    default:                                    /* red: play / hold (the sequencer thread acts on it), not saved */
        b->run = !b->run;
        return;
    }
    RL_CHANGED();
}

void rl_turn(int pos, int knob, int delta)      /* encoder turn, 1024 per detent */
{
    rl_bank_t *b = &rl[pos];
    int n = delta / 1024, g;
    rl_bank_init(b);
    if (!n) return;
    g = rl_grid(b);
    switch (knob) {
    case 0: {                                   /* blue: time cursor */
        int t = (int)b->cur_t + n * g;
        while (t < 0) t += b->loop_len;
        b->cur_t = (uint16_t)(t % b->loop_len);
        return;                                 /* not saved */
    }
    case 1: {                                   /* green: pitch view */
        int v = b->view_top - n;
        b->view_top = (int8_t)(v < RL_ROWS - 1 ? RL_ROWS - 1 : v > 127 ? 127 : v);
        return;                                 /* saved with the next edit */
    }
    case 2: {                                   /* white: length of new notes */
        int l = (int)b->cur_len + n * g;
        b->cur_len = (uint16_t)(l < g ? g : l > b->loop_len ? b->loop_len : l);
        break;
    }
    default: {                                  /* red: loop length */
        int l = (int)b->loop_len + n * g;
        b->loop_len = (uint16_t)(l < g ? g : l > RL_MAXLOOP ? RL_MAXLOOP : l);
        if (b->cur_t >= b->loop_len) b->cur_t = 0;
        break;
    }
    }
    RL_CHANGED();
}

/* ---- persistence: one text row per mode ---- */
static char *rl_put(char *p, unsigned v)
{
    char t[10]; int i = 0;
    do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v && i < 10);
    while (i) *p++ = t[--i];
    return p;
}

static int rl_get(const char **pp, unsigned *out)
{
    const char *p = *pp; unsigned v = 0;
    while (*p && (*p < '0' || *p > '9')) p++;
    if (!*p) return 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (unsigned)(*p++ - '0');
    *pp = p; *out = v;
    return 1;
}

/* -> characters written (no terminator) */
int rl_serialize(int pos, char *out, int max)
{
    rl_bank_t *b = &rl[pos];
    char *p = out;
    rl_bank_init(b);
    if (max < 40) return 0;
    p = rl_put(p, b->loop_len); *p++ = ',';
    p = rl_put(p, b->grid_i);   *p++ = ',';
    p = rl_put(p, b->vel_i);    *p++ = ',';
    p = rl_put(p, (uint8_t)b->view_top); *p++ = ',';
    p = rl_put(p, b->cur_len);
    for (int i = 0; i < RL_MAXNOTES; i++) {
        const rl_note_t *n = &b->note[i];
        if (!n->used) continue;
        if (p - out > max - 24) break;          /* 4 numbers <= 19 characters */
        *p++ = ','; p = rl_put(p, n->start);
        *p++ = ','; p = rl_put(p, n->len);
        *p++ = ','; p = rl_put(p, n->pitch);
        *p++ = ','; p = rl_put(p, n->vel);
    }
    return (int)(p - out);
}

void rl_parse(int pos, const char *s)           /* s: 0-terminated row; nothing (no row): defaults stay */
{
    rl_bank_t *b = &rl[pos];
    unsigned v;
    int g, i = 0;
    rl_bank_init(b);
    if (!rl_get(&s, &v)) return;
    b->loop_len = (uint16_t)(v > RL_MAXLOOP ? RL_MAXLOOP : v);   /* clamped below, once the grid is known */
    if (rl_get(&s, &v)) b->grid_i = (uint8_t)(v & 3);
    if (rl_get(&s, &v)) b->vel_i = (uint8_t)(v % 3);
    if (rl_get(&s, &v)) b->view_top = (int8_t)(v > 127 ? 127 : (v < RL_ROWS - 1 ? RL_ROWS - 1 : v));
    if (rl_get(&s, &v)) b->cur_len = (uint16_t)(v > RL_MAXLOOP ? RL_MAXLOOP : v);
    g = rl_grid(b);
    if (b->loop_len < g) b->loop_len = (uint16_t)g;
    if (b->cur_len < g) b->cur_len = (uint16_t)g;
    if (b->cur_len > b->loop_len) b->cur_len = b->loop_len;
    if (b->cur_t >= b->loop_len) b->cur_t = 0;
    for (int k = 0; k < RL_MAXNOTES; k++) b->note[k].used = 0;
    while (i < RL_MAXNOTES) {
        unsigned st, ln, pi, ve;
        if (!rl_get(&s, &st) || !rl_get(&s, &ln) || !rl_get(&s, &pi) || !rl_get(&s, &ve)) break;
        if (st >= RL_MAXLOOP || pi > 127) continue;
        b->note[i].start = (uint16_t)st;
        b->note[i].len = (uint16_t)(ln ? (ln > RL_MAXLOOP ? RL_MAXLOOP : ln) : 1);
        b->note[i].pitch = (uint8_t)pi;
        b->note[i].vel = (uint8_t)(ve ? (ve > 127 ? 127 : ve) : 100);
        b->note[i].used = 1;
        i++;
    }
}

/* ---- screen (GUI thread): everything drawn with lines ---- */
#define RL_X0   (14 << 10)
#define RL_Y0   (22 << 10)
#define RL_RH   (8 << 10)
#define RL_GW   (300 << 10)

static const uint8_t RL_COL_GRID[4] = { 0x6f, 0x71, 0x73, 0xff };
static const uint8_t RL_COL_BAR [4] = { 0x00, 0x52, 0x34, 0xff };
static const uint8_t RL_COL_NOTE[4] = { 0x00, 0xed, 0x95, 0xff };
static const uint8_t RL_COL_CUR [4] = { 0x4d, 0x9e, 0xff, 0xff };
static const uint8_t RL_COL_HEAD[4] = { 0xff, 0x3a, 0x5d, 0xff };
static const uint8_t RL_COL_HOLD[4] = { 0x5c, 0x17, 0x25, 0xff };   /* faded red: held */
static const uint8_t RL_COL_TEXT[4] = { 0xff, 0xff, 0xff, 0xff };

static void rl_line(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}
static void rl_hline(void *r, int32_t x0, int32_t x1, int32_t yc, int32_t w, const uint8_t *c) { rl_line(r, x0, yc, x1, yc, w, c); }
static void rl_vline(void *r, int32_t xc, int32_t y0, int32_t y1, int32_t w, const uint8_t *c) { rl_line(r, xc, y0, xc, y1, w, c); }

static void rl_num(char *out, unsigned v)       /* up to 3 digits */
{
    char *e = rl_put(out, v % 1000);
    *e = 0;
}
/* a duration in ticks as a reduced fraction of a whole note: 24 -> "1/16", 72 -> "3/16", 768 -> "2/1" */
static void rl_frac(char *out, unsigned ticks)
{
    unsigned d = RL_PPQ * 4, a = ticks, b = d, t;
    while (b) { t = a % b; a = b; b = t; }      /* a = gcd(ticks, 384) */
    if (!a) { out[0] = '0'; out[1] = 0; return; }
    rl_num(out, ticks / a);
    while (*out) out++;
    *out++ = '/';
    rl_num(out, d / a);
}

/* 7-segment digits, '/' and the letters of HOLD in a 5 x 8 px box; x, y = top-left in px */
static const uint8_t rl_seg7[10] = { 0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f };   /* a..g = bit 0..6 */
static const int8_t rl_gl_h[] = { 0,0,0,8, 5,0,5,8, 0,4,5,4, -1 };
static const int8_t rl_gl_o[] = { 0,0,5,0, 5,0,5,8, 5,8,0,8, 0,8,0,0, -1 };
static const int8_t rl_gl_l[] = { 0,0,0,8, 0,8,5,8, -1 };
static const int8_t rl_gl_d[] = { 0,0,0,8, 0,0,3,0, 3,0,5,2, 5,2,5,6, 5,6,3,8, 3,8,0,8, -1 };
static void rl_text(void *r, int x, int y, const char *s, const uint8_t *c)
{
    const int32_t w = 5 << 10, h = 8 << 10, m = 4 << 10, th = 5 << 8;     /* 1.25 px strokes */
    for (; *s; s++, x += 7) {
        int32_t X = x << 10, Y = y << 10;
        const int8_t *g = *s == 'H' ? rl_gl_h : *s == 'O' ? rl_gl_o : *s == 'L' ? rl_gl_l : *s == 'D' ? rl_gl_d : 0;
        if (g) {
            for (; g[0] >= 0; g += 4) rl_line(r, X + (g[0] << 10), Y + (g[1] << 10), X + (g[2] << 10), Y + (g[3] << 10), th, c);
            continue;
        }
        if (*s == '/') { rl_line(r, X, Y + h, X + w, Y, th, c); continue; }
        if (*s < '0' || *s > '9') continue;
        unsigned b = rl_seg7[*s - '0'];
        if (b & 0x01) rl_hline(r, X, X + w, Y, th, c);
        if (b & 0x02) rl_vline(r, X + w, Y, Y + m, th, c);
        if (b & 0x04) rl_vline(r, X + w, Y + m, Y + h, th, c);
        if (b & 0x08) rl_hline(r, X, X + w, Y + h, th, c);
        if (b & 0x10) rl_vline(r, X, Y + m, Y + h, th, c);
        if (b & 0x20) rl_vline(r, X, Y, Y + m, th, c);
        if (b & 0x40) rl_hline(r, X, X + w, Y + m, th, c);
    }
}

static int32_t rl_x(int ticks, int win_ticks) { return RL_X0 + (int32_t)((int64_t)ticks * RL_GW / win_ticks); }

void rl_screen_draw(void *r, int pos)
{
    rl_bank_t *b = &rl[pos];
    int grid, win_ticks, win_start;
    const int32_t gy1 = RL_Y0 + RL_ROWS * RL_RH;
    char tmp[12];
    rl_bank_init(b);
    grid = rl_grid(b);
    win_ticks = RL_WINS * grid;
    win_start = (b->cur_t / win_ticks) * win_ticks;                 /* page so the cursor stays on screen */

    for (int s = 0; s <= RL_WINS; s++)                              /* one line per grid step, brighter on the beat */
        rl_vline(r, rl_x(s * grid, win_ticks), RL_Y0, gy1, ((win_start + s * grid) % RL_PPQ) == 0 ? (2 << 9) : (1 << 9), RL_COL_GRID);
    for (int i = 0; i < RL_ROWS; i++) {                             /* a faint line under every C */
        int note = b->view_top - i;
        if (note >= 0 && note % 12 == 0) rl_hline(r, RL_X0, RL_X0 + RL_GW, RL_Y0 + i * RL_RH + RL_RH, 1 << 9, RL_COL_BAR);
    }
    for (int i = 0; i < RL_MAXNOTES; i++) {                         /* notes */
        const rl_note_t *n = &b->note[i];
        if (!n->used) continue;
        int row = b->view_top - n->pitch, e = rl_end(n);
        if (row < 0 || row >= RL_ROWS || e <= win_start || n->start >= win_start + win_ticks) continue;
        int a0 = n->start < win_start ? win_start : n->start, a1 = e > win_start + win_ticks ? win_start + win_ticks : e;
        rl_hline(r, rl_x(a0 - win_start, win_ticks) + (1 << 9), rl_x(a1 - win_start, win_ticks) - (1 << 9),
                 RL_Y0 + row * RL_RH + RL_RH / 2, RL_RH - (2 << 10), RL_COL_NOTE);
    }
    if (b->cur_t >= win_start && b->cur_t < win_start + win_ticks)  /* time cursor */
        rl_vline(r, rl_x(b->cur_t - win_start, win_ticks), RL_Y0, gy1, 2 << 9, RL_COL_CUR);
    if (b->run && b->playing && b->play_t >= win_start && b->play_t < win_start + win_ticks)   /* playhead */
        rl_vline(r, rl_x(b->play_t - win_start, win_ticks), RL_Y0, gy1, 3 << 9, RL_COL_HEAD);

    rl_frac(tmp, (unsigned)grid);                  rl_text(r, 4, 5, tmp, RL_COL_TEXT);   /* grid */
    rl_frac(tmp, b->cur_len);                      rl_text(r, 70, 5, tmp, RL_COL_TEXT);  /* length of new notes */
    rl_num(tmp, (unsigned)(b->loop_len / grid));   rl_text(r, 140, 5, tmp, RL_COL_TEXT); /* loop, grid steps */
    {                                                               /* HOLD box */
        const uint8_t *c = b->run ? RL_COL_HEAD : RL_COL_HOLD;
        const int32_t th = 5 << 8, e = th / 2, bx0 = 277 << 10, bx1 = 311 << 10, by0 = 2 << 10, by1 = 16 << 10;
        rl_hline(r, bx0 - e, bx1 + e, by0, th, c);
        rl_hline(r, bx0 - e, bx1 + e, by1, th, c);
        rl_vline(r, bx0, by0, by1, th, c);
        rl_vline(r, bx1, by0, by1, th, c);
        rl_text(r, 281, 5, "HOLD", c);
    }
}

#ifndef OP1_HOST
/* ======================= firmware player (extra sequencer "roll", runs as type 5 (arpeggio) + flag) ===============
 * DB row seq_types "roll"; SeqIds::from_name "roll" -> type 5 with rl_active[mode] = 1 (dispatcher.c). The arpeggio's
 * player getter (CALL 0x01A46400 @0x01A0D1B0 in PlayerManager::create_player) -> rl_player_get. Player vtable as
 * euclid's (seq/euclid.c): +0x14 key_down, +0x1c key_up, +0x24 leave, +0x2c tick (counter modulo 48), +0x3c stop. */
#define RL_MODE()      (*(volatile int *)0x01DE4C40 == 1)
#define RL_SCREEN_ID   0x2C
#define RL_SYNTH_OCT() (((const int *)0x01DFE4E0)[*(volatile int *)0xFF8016D4 & 7])
volatile uint8_t rl_active[2];                  /* roll selected (synth, drum) */
static int rl_nop(void *self) { (void)self; return 0; }
static int rl_key_down(void *self, int key)
{
    (void)self;
    int pos = RL_MODE();
    if (!rl_active[pos] || *(volatile int *)0x018B65C8 != RL_SCREEN_ID || key < 0 || key > 23)
        return 0;                               /* not editing: the key just plays */
    return rl_key(pos, key + 53 + (pos ? 0 : 12 * RL_SYNTH_OCT()), *(volatile uint8_t *)0x01DD7F78);
}
static void rl_ptick(void *self, int counter) { (void)self; rl_tick(RL_MODE(), counter); }
static void rl_pstop(void *self) { (void)self; rl_stop_all(); }
static const uint32_t rl_vt[16] = {
    0, 0,  0, (uint32_t)rl_nop,  0, (uint32_t)rl_key_down,  0, (uint32_t)rl_nop,
    0, (uint32_t)rl_pstop,  0, (uint32_t)rl_ptick,  0, (uint32_t)rl_nop,  0, (uint32_t)rl_pstop };
static struct { const uint32_t *vt; uint32_t pad[7]; } rl_player = { rl_vt, { 0 } };

/* ---- persistence via the background saver, as euclid (seq/euclid.c) ---- */
#define BG_REGISTER ((void (*)(void *))0x0193F010)
#define BG_DIRTY    ((void (*)(void *))0x0193EF3C)
#define DB_HANDLE   ((void *(*)(void))0x0196DD44)
#define DB_EXEC     ((void (*)(void *, const char *))0x0196F950)
#define DB_GET      ((void (*)(void *, void *, const char *))0x0196FBD0)
#define FW_FREE     ((void (*)(void *))0x01A5E310)
typedef struct { uint32_t alloc; union { char buf[16]; char *ptr; } u; uint32_t size, cap; uint32_t pad; } rl_dstr;
static const char RL_CREATE[] = "CREATE TABLE IF NOT EXISTS rollseq (is_synth INTEGER PRIMARY KEY, data TEXT NOT NULL)";
static void rl_bg_nop(void *self) { (void)self; }
static void rl_bg_save(void *self)
{
    static char sql[8192];                      /* 128 notes: < 2.6 KB per row */
    static const char head[] = "INSERT OR REPLACE INTO rollseq VALUES (";
    void *db = DB_HANDLE();
    (void)self;
    DB_EXEC(db, RL_CREATE);
    for (int pos = 0; pos < 2; pos++) {         /* key 0 = synth, 1 = drum (the column name is historical) */
        int o = 0;
        for (const char *h = head; *h; h++) sql[o++] = *h;
        sql[o++] = (char)('0' + pos); sql[o++] = ','; sql[o++] = '\'';
        o += rl_serialize(pos, sql + o, (int)sizeof sql - o - 4);
        sql[o++] = '\''; sql[o++] = ')'; sql[o] = 0;
        op1_db_write(db, sql);                  /* (no assert: long statement, op1_synth.h) */
    }
}
static const uint32_t rl_bg_vt[6] = { 0, 0, 0, (uint32_t)rl_bg_nop, 0, (uint32_t)rl_bg_save };
static struct { uint8_t dirty, saving, pad[2]; const uint32_t *vt; uint32_t more[2]; } rl_bg;
static uint8_t rl_bg_ready;
static void rl_changed(void) { if (rl_bg_ready) BG_DIRTY(&rl_bg); }
static void rl_load(void)
{
    void *db = DB_HANDLE();
    rl_bank_init(&rl[0]);
    rl_bank_init(&rl[1]);
    DB_EXEC(db, RL_CREATE);
    for (int pos = 0; pos < 2; pos++) {
        rl_dstr s = { 0 };
        DB_GET(&s, db, pos ? "SELECT data FROM rollseq WHERE is_synth=1" : "SELECT data FROM rollseq WHERE is_synth=0");
        if (s.size) rl_parse(pos, s.cap > 15 ? s.u.ptr : s.u.buf);
        if (s.cap > 15) FW_FREE(s.u.ptr);
    }
    BG_REGISTER(&rl_bg);
    rl_bg.vt = rl_bg_vt;
    rl_bg_ready = 1;
}

void *rl_player_get(void)                       /* CALL @0x01A0D1B0: roll or the stock arpeggio */
{
    if (!rl_bg_ready) rl_load();
    if (!rl_active[RL_MODE()]) return ((void *(*)(void))0x01A46400)();
    return &rl_player;
}

/* screen: own object swapped into the arpeggio screen slot 0x2C (dispatcher.c) */
static void rl_scr_init(void *scr) { (void)scr; if (!rl_bg_ready) rl_load(); }
static void rl_scr_update(void *scr, int dt) { op1_screen_draw(scr, dt); rl_screen_draw(op1_raster(), RL_MODE()); }
const op1_screen_t SCREEN_roll = { rl_scr_init, rl_scr_update };
#endif
