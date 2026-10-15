/* "roll" sequencer: a piano roll (synth mode only).
 *
 * Notes with their own start, length, pitch and velocity on a grid, in a loop of settable length (one grid step ..
 * 16 bars), played polyphonically (up to RL_POLY notes at once; a note beyond that is skipped, never left hanging).
 * One pattern, saved. Synth mode only: selected in drum mode, roll stays passive (its screen says so, the keys play
 * the drums as usual, nothing is sequenced).
 *
 * Screen (roll screen shown): 16 pitch rows x 16 grid columns, the page that holds the time cursor; notes as green
 * bars, the time cursor (blue), the playhead (red), the loop end (dim red), C rows lined and labelled (C4 = 60).
 * Top: grid (blue), length of new notes (white), their velocity (white), loop length in grid steps (red), play state.
 * Controls (roll screen shown; Shift = shift key held):
 *   blue turn    time cursor (grid steps)               blue press   grid 1/4 1/8 1/16 1/32
 *   green turn   scroll the pitch rows                   green press  clear the notes under the cursor
 *   white turn   length of new notes (grid steps)        white press  velocity of new notes 127 / 100 / 64
 *   red turn     loop length (grid steps)                red press    play / hold
 *   note key     a note of that pitch at the cursor with the length and velocity set, then the cursor moves on by
 *                that length (step input); the key also sounds
 *   Shift + key  removes the note of that pitch under the cursor (no sound)
 *   left / right keyboard octave (as euclid) · lift / drop / scissors: nothing (finger is not running)
 * Pitch: key + 0x35 + 12 * the synth slot's octave.
 * Play: the red press arms; playback starts from the top of the loop on the next beat of the OP-1 beat clock (the
 * metronome's, op1_beat_clock; at once while that clock stands still). A reset / stop of the player releases the
 * notes and re-arms: playing goes on from the top of the loop at the next beat. MIDI START restarts a playing roll
 * from the top on the first clock. Selecting roll holds it; play / hold is not saved.
 * Saved (bg_saver, DB table rollseq, row is_synth = 1): "1,loop,grid,velocity,top row,length" (1 = row format; rows
 * without it are ignored), then "start,length,pitch,velocity" per note; times in player ticks (96 per beat).
 *
 * Platform independent (host test harness/roll_test.c); firmware glue at the end and in dispatcher.c.
 * Copyright (C) 2026 bastienstefani. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "../op1_synth.h"
#include "seq_text.h"

#define RL_MAXNOTES 128
#define RL_POLY     16                                  /* notes sounding at once */
#define RL_PPQ      96                                  /* player ticks per beat */
#define RL_MAXLOOP  (RL_PPQ * 64)                       /* 16 bars of 4/4 */
#define RL_ROWS     16                                  /* pitch rows on screen */
#define RL_COLS     16                                  /* grid columns on screen */
#define RL_FORMAT   1                                   /* first field of the saved row */

typedef struct { uint16_t start, len; uint8_t pitch, vel, used, pad; } rl_note_t;

static const uint16_t rl_grids[4] = { RL_PPQ, RL_PPQ / 2, RL_PPQ / 4, RL_PPQ / 8 };   /* 1/4 1/8 1/16 1/32 */
static const uint8_t  rl_vels[3]  = { 127, 100, 64 };

typedef struct {
    rl_note_t note[RL_MAXNOTES];
    uint16_t loop_len, cur_t, cur_len;                  /* ticks: loop, time cursor, length of new notes */
    uint8_t  grid_i, vel_i, top, init;                  /* rl_grids / rl_vels index, pitch of the top row */
    volatile uint8_t run;                               /* play (red press); not saved */
    uint8_t  playing;                                   /* 0: armed (waits for a beat) or held */
    uint16_t play_t;                                    /* playback position (sequencer thread) */
    int32_t  acc[4];                                    /* encoder remainders */
    struct { uint8_t note, used; uint16_t off_t; } snd[RL_POLY];
} rl_bank_t;

static rl_bank_t rl;                                    /* the synth pattern */

#ifdef OP1_HOST
int host_rl_changed;
#define RL_CHANGED() (host_rl_changed++)
void host_rl_note_on(int note, int vel);
void host_rl_note_off(int note);
#define RL_NOTE_ON(n, v) host_rl_note_on((n), (v))
#define RL_NOTE_OFF(n)   host_rl_note_off(n)
#else
static void rl_changed(void);
#define RL_CHANGED() rl_changed()
int  eu_fw_note_on(int pos, int note, int key, int vel);   /* seq/euclid.c: VoiceCtl note on / off */
void eu_fw_note_off(int midi);
#define RL_NOTE_ON(n, v) eu_fw_note_on(0, (n), 0, (v))
#define RL_NOTE_OFF(n)   eu_fw_note_off(n)
#endif

static int rl_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int rl_grid(const rl_bank_t *b) { return rl_grids[b->grid_i & 3]; }
static int rl_end(const rl_note_t *n) { return n->start + (n->len ? n->len : 1); }

static void rl_bank_init(rl_bank_t *b)
{
    if (b->init) return;
    for (int i = 0; i < RL_MAXNOTES; i++) b->note[i].used = 0;
    for (int i = 0; i < RL_POLY; i++) b->snd[i].used = 0;
    b->grid_i = 2;                                      /* 1/16 */
    b->vel_i = 1;                                       /* 100 */
    b->loop_len = RL_PPQ / 4 * 16;                      /* one bar of 16 steps */
    b->cur_len = RL_PPQ / 4;
    b->cur_t = 0;
    b->top = 72;
    b->run = 0; b->playing = 0; b->play_t = 0;
    b->init = 1;
}

/* ---- playback (sequencer thread) ---- */
static void rl_off_all(rl_bank_t *b)
{
    for (int i = 0; i < RL_POLY; i++)
        if (b->snd[i].used) { b->snd[i].used = 0; RL_NOTE_OFF(b->snd[i].note); }
}

/* one player tick (1/96 beat); beat: a beat of the beat clock passed since the previous tick */
void rl_tick(int beat)
{
    rl_bank_t *b = &rl;
    if (!b->init) return;
    if (!b->run) {                                      /* hold: release once */
        if (b->playing) { rl_off_all(b); b->play_t = 0; b->playing = 0; }
        return;
    }
    if (!b->playing) {                                  /* armed: from the top of the loop on the next beat */
        if (!beat) return;
        b->play_t = 0;
        b->playing = 1;
    }
    int t = b->play_t, loop = b->loop_len;
    for (int i = 0; i < RL_POLY; i++)                   /* note-offs at the end of their gate */
        if (b->snd[i].used && b->snd[i].off_t == t) { b->snd[i].used = 0; RL_NOTE_OFF(b->snd[i].note); }
    for (int i = 0; i < RL_MAXNOTES; i++) {             /* note-ons, only with a free gate slot */
        const rl_note_t *n = &b->note[i];
        if (!n->used || n->start != t || t >= loop) continue;
        int off = rl_end(n);
        if (off > loop) off = loop;
        for (int j = 0; j < RL_POLY; j++)
            if (!b->snd[j].used) {
                b->snd[j].note = n->pitch; b->snd[j].off_t = (uint16_t)off; b->snd[j].used = 1;
                RL_NOTE_ON(n->pitch, n->vel ? n->vel : 100);
                break;
            }
    }
    if (++b->play_t >= loop) {                          /* wrap: release everything (nothing hangs) */
        b->play_t = 0;
        rl_off_all(b);
    }
}

void rl_rearm(void)                                     /* player reset / stop: notes off, from the top at the next beat */
{
    rl_off_all(&rl);
    rl.playing = 0;
    rl.play_t = 0;
}

void rl_midi_start(void)                                /* MIDI START: a playing (or armed) roll from the top, first tick */
{
    rl_bank_t *b = &rl;
    if (!b->init || !b->run) return;
    rl_off_all(b);
    b->play_t = 0;
    b->playing = 1;
}

/* UI code (keys, encoders, persistence, screen) is compiled for size: code space is short. The clock (rl_tick and
 * what it calls) stays -O2: it runs in the sequencer thread at every tick. */
#pragma GCC push_options
#pragma GCC optimize("Os")
/* ---- editing (KeyHandler thread); pos: mode (synth 0, drum 1: nothing) ---- */
static int rl_note_at(const rl_bank_t *b, int pitch, int t)      /* note of this pitch covering t, or -1 */
{
    for (int i = 0; i < RL_MAXNOTES; i++) {
        const rl_note_t *n = &b->note[i];
        if (n->used && n->pitch == pitch && n->start <= t && t < rl_end(n)) return i;
    }
    return -1;
}

static void rl_place(rl_bank_t *b, int pitch, int t, int len, int vel)
{
    int slot = -1;
    for (int i = 0; i < RL_MAXNOTES && slot < 0; i++)  /* same start and pitch: replaced */
        if (b->note[i].used && b->note[i].pitch == pitch && b->note[i].start == t) slot = i;
    for (int i = 0; i < RL_MAXNOTES && slot < 0; i++) if (!b->note[i].used) slot = i;
    if (slot < 0) return;                               /* full */
    rl_note_t *n = &b->note[slot];
    n->used = 0;                                        /* fields first, then the flag (read by the sequencer thread) */
    n->start = (uint16_t)t; n->len = (uint16_t)len; n->pitch = (uint8_t)pitch; n->vel = (uint8_t)vel;
    n->used = 1;
}

/* note key on the roll screen (pitch with the octave): returns 1 = consumed (no sound) */
int rl_key(int pos, int pitch, int shift)
{
    rl_bank_t *b = &rl;
    if (pos || pitch < 0 || pitch > 127) return 0;
    rl_bank_init(b);
    if (shift) {                                        /* remove */
        int i = rl_note_at(b, pitch, b->cur_t);
        if (i >= 0) { b->note[i].used = 0; RL_CHANGED(); }
        return 1;
    }
    rl_place(b, pitch, b->cur_t, b->cur_len, rl_vels[b->vel_i % 3]);
    if (pitch > b->top || pitch <= b->top - RL_ROWS)    /* keep it in view */
        b->top = (uint8_t)rl_clampi(pitch + 2, RL_ROWS - 1, 127);
    b->cur_t = (uint16_t)((b->cur_t + b->cur_len) % b->loop_len);   /* step input */
    RL_CHANGED();
    return 0;                                           /* the key also sounds */
}

void rl_press(int pos, int knob)                        /* encoder press, knob 0..3 = blue..red */
{
    rl_bank_t *b = &rl;
    if (pos) return;
    rl_bank_init(b);
    if (knob == 0) {                                    /* grid; the cursor snaps to it */
        b->grid_i = (uint8_t)((b->grid_i + 1) & 3);
        int g = rl_grid(b);
        b->cur_t = (uint16_t)(b->cur_t / g * g);
        if (b->cur_len < g) b->cur_len = (uint16_t)g;
    } else if (knob == 1) {                             /* clear the notes under the cursor */
        for (int i = 0; i < RL_MAXNOTES; i++) {
            rl_note_t *n = &b->note[i];
            if (n->used && n->start <= b->cur_t && b->cur_t < rl_end(n)) n->used = 0;
        }
    } else if (knob == 2) b->vel_i = (uint8_t)((b->vel_i + 1) % 3);
    else { b->run = (uint8_t)!b->run; return; }         /* play / hold (not saved) */
    RL_CHANGED();
}

static int rl_detents(rl_bank_t *b, int k, int delta)  /* 1024 per detent, remainders kept */
{
    if ((b->acc[k] > 0 && delta < 0) || (b->acc[k] < 0 && delta > 0)) b->acc[k] = 0;
    b->acc[k] += delta;
    int st = b->acc[k] / 1024;
    b->acc[k] -= st * 1024;
    return st;
}

void rl_turn(int pos, int knob, int delta)              /* encoder turn */
{
    rl_bank_t *b = &rl;
    if (pos || knob < 0 || knob > 3) return;
    rl_bank_init(b);
    int st = rl_detents(b, knob, delta), g = rl_grid(b);
    if (!st) return;
    if (knob == 0) {                                    /* time cursor (not saved) */
        int t = b->cur_t + st * g, l = b->loop_len;
        t %= l;
        b->cur_t = (uint16_t)(t < 0 ? t + l : t);
        return;
    }
    if (knob == 1) b->top = (uint8_t)rl_clampi(b->top - st, RL_ROWS - 1, 127);
    else if (knob == 2) b->cur_len = (uint16_t)rl_clampi(b->cur_len + st * g, g, b->loop_len);
    else {
        b->loop_len = (uint16_t)rl_clampi(b->loop_len + st * g, g, RL_MAXLOOP);
        if (b->cur_t >= b->loop_len) b->cur_t = 0;
        if (b->cur_len > b->loop_len) b->cur_len = b->loop_len;
    }
    RL_CHANGED();
}

/* ---- persistence text (sq_store): "1,loop,grid,velocity,top,length" + ",start,length,pitch,velocity" per note ---- */
int rl_serialize(int pos, char *out, int n)
{
    rl_bank_t *b = &rl;
    if (n > 0) *out = 0;
    if (pos || n < 40) return 0;                        /* (the drum row stays empty) */
    rl_bank_init(b);
    char *p = out, *e = out + n - 22;                   /* a note: <= 4 x 4 digits + 4 commas, + the end */
    const int h[6] = { RL_FORMAT, b->loop_len, b->grid_i, b->vel_i, b->top, b->cur_len };
    for (int i = 0; i < 6; i++) { if (i) *p++ = ','; p = sq_num(p, (uint32_t)h[i], 1); }
    for (int i = 0; i < RL_MAXNOTES && p < e; i++) {
        const rl_note_t *nt = &b->note[i];
        if (!nt->used) continue;
        const int v[4] = { nt->start, nt->len, nt->pitch, nt->vel };
        for (int k = 0; k < 4; k++) { *p++ = ','; p = sq_num(p, (uint32_t)v[k], 1); }
    }
    return (int)(p - out);
}

static int rl_field(const char **pp, const char *e, int *v)   /* the next number (0..9999) of a row: 1, else 0 */
{
    const char *p = *pp;
    int x = -1;
    for (; p < e && *p != ','; p++) {
        if (*p < '0' || *p > '9' || x > 999) return 0;
        x = (x < 0 ? 0 : x * 10) + (*p - '0');
    }
    if (x < 0) return 0;
    *pp = p < e ? p + 1 : p;
    *v = x;
    return 1;
}

int rl_parse(int pos, const char *s, int len)           /* returns 1 if the row was taken */
{
    rl_bank_t *b = &rl;
    const char *e = s + len;
    int h[6], v[4], k = 0;
    if (pos) return 0;
    rl_bank_init(b);
    for (int i = 0; i < 6; i++) if (!rl_field(&s, e, &h[i])) return 0;
    if (h[0] != RL_FORMAT) return 0;
    rl_off_all(b);
    b->grid_i = (uint8_t)(h[2] & 3);
    int g = rl_grid(b);
    b->loop_len = (uint16_t)rl_clampi(h[1], g, RL_MAXLOOP);
    b->vel_i = (uint8_t)(h[3] % 3);
    b->top = (uint8_t)rl_clampi(h[4], RL_ROWS - 1, 127);
    b->cur_len = (uint16_t)rl_clampi(h[5], g, b->loop_len);
    b->cur_t = 0; b->play_t = 0; b->playing = 0; b->run = 0;
    for (int i = 0; i < RL_MAXNOTES; i++) b->note[i].used = 0;
    while (k < RL_MAXNOTES && s < e) {
        int f = 0;
        while (f < 4 && rl_field(&s, e, &v[f])) f++;
        if (f < 4) break;                               /* (a bad field ends the notes) */
        if (v[0] >= RL_MAXLOOP || v[2] > 127) continue;
        rl_note_t *n = &b->note[k++];
        n->start = (uint16_t)v[0];
        n->len = (uint16_t)rl_clampi(v[1], 1, RL_MAXLOOP);
        n->pitch = (uint8_t)v[2];
        n->vel = (uint8_t)(v[3] ? rl_clampi(v[3], 1, 127) : 100);
        n->used = 1;
    }
    return 1;
}

/* ======================= screen (GUI thread) =======================
 * Grid: one line per grid step (brighter on a beat, brighter still on a bar), a dim green line under every C row with
 * its name at the left; notes as green bars (the part inside the page), cursor blue, playhead red, loop end dim red.
 * Header: grid (blue), LEN (white), VEL (white), LOOP in grid steps (red), PLAY / HOLD box (red / dim red). */
#include "roll_geom.h"
#define RL_RH    OP1_Q10(RL_ROW_H)
#define RL_GY0   OP1_Q10(RL_GRID_Y0)
#define RL_GY1   (RL_GY0 + RL_ROWS * RL_RH)
#define RL_GX0   OP1_Q10(RL_GRID_X0)
#define RL_GW    (OP1_Q10(RL_GRID_X1) - RL_GX0)

static int32_t rl_x(int ticks, int win) { return RL_GX0 + (int32_t)((int64_t)ticks * RL_GW / win); }
static void rl_vline(void *r, int32_t x, int32_t w, const uint8_t *c) { sq_line(r, x, RL_GY0, x, RL_GY1, w, c); }

/* a duration in ticks as a reduced fraction of a whole note: 24 -> "1/16", 72 -> "3/16", 768 -> "2/1" */
static char *rl_frac(char *p, int ticks)
{
    int d = RL_PPQ * 4, a = ticks, b = d;
    while (b) { int t = a % b; a = b; b = t; }          /* a = gcd */
    if (!a) return sq_cat(p, "0");
    p = sq_num(p, (uint32_t)(ticks / a), 1);
    p = sq_cat(p, "/");
    return sq_num(p, (uint32_t)(d / a), 1);
}

static void rl_screen_draw(void *r, int pos)
{
    rl_bank_t *b = &rl;
    char t[16];
    const int32_t s = OP1_Q10(1.2), y = OP1_Q10(RL_HEAD_Y);
    rl_bank_init(b);
    if (pos) {                                          /* drum mode: not available */
        const int32_t sb = OP1_Q10(2.4), ss = OP1_Q10(1.4);
        sq_text(r, OP1_Q10(160) - sq_width(sb, "ROLL") / 2, OP1_Q10(48), sb, "ROLL", 0x600, rl_green);
        sq_text(r, OP1_Q10(160) - sq_width(ss, "SYNTH MODE ONLY") / 2, OP1_Q10(82), ss, "SYNTH MODE ONLY", 0x400, rl_white);
        return;
    }
    int g = rl_grid(b), win = RL_COLS * g, w0 = b->cur_t / win * win, loop = b->loop_len;
    for (int c = 0; c <= RL_COLS; c++) {                /* grid */
        int at = w0 + c * g;
        int32_t w = at % (RL_PPQ * 4) == 0 ? 0x300 : at % RL_PPQ == 0 ? 0x200 : 0x100;
        rl_vline(r, rl_x(c * g, win), w, at % RL_PPQ == 0 ? rl_grey : rl_dim);
    }
    for (int i = 0; i < RL_ROWS; i++) {                 /* C rows */
        int note = b->top - i;
        if (note < 0 || note % 12) continue;
        int32_t ry = RL_GY0 + (i + 1) * RL_RH;
        sq_line(r, RL_GX0, ry, OP1_Q10(RL_GRID_X1), ry, 0x200, rl_cline);
        char *p = sq_cat(t, "C");
        if (note < 12) sq_cat(p, "-"); else sq_num(p, (uint32_t)(note / 12 - 1), 1);
        sq_text(r, OP1_Q10(2), ry - OP1_Q10(6.5), OP1_Q10(0.9), t, 0x200, rl_grey);
    }
    for (int i = 0; i < RL_MAXNOTES; i++) {             /* notes */
        const rl_note_t *n = &b->note[i];
        if (!n->used) continue;
        int row = b->top - n->pitch, e = rl_end(n);
        if (row < 0 || row >= RL_ROWS || e <= w0 || n->start >= w0 + win) continue;
        int a0 = n->start < w0 ? w0 : n->start, a1 = e > w0 + win ? w0 + win : e;
        int32_t ny = RL_GY0 + row * RL_RH + RL_RH / 2;
        sq_line(r, rl_x(a0 - w0, win) + 0x200, ny, rl_x(a1 - w0, win) - 0x200, ny, RL_RH - 0x600, n->start >= loop ? rl_dim : rl_green);
    }
    if (loop > w0 && loop < w0 + win) rl_vline(r, rl_x(loop - w0, win), 0x300, rl_dimred);   /* loop end */
    rl_vline(r, rl_x(b->cur_t - w0, win), 0x400, rl_blue);                                     /* cursor */
    int pt = b->play_t;
    if (b->run && b->playing && pt >= w0 && pt < w0 + win) rl_vline(r, rl_x(pt - w0, win), 0x600, rl_red);   /* playhead */
    /* header */
    rl_frac(t, g);
    sq_text(r, OP1_Q10(RL_GRID_TX), y, s, t, 0x380, rl_blue);
    rl_frac(sq_cat(t, "LEN "), b->cur_len);
    sq_text(r, OP1_Q10(RL_LEN_TX), y, s, t, 0x380, rl_white);
    sq_num(sq_cat(t, "VEL "), rl_vels[b->vel_i % 3], 1);
    sq_text(r, OP1_Q10(RL_VEL_TX), y, s, t, 0x380, rl_white);
    char *p = sq_num(sq_cat(t, "LOOP "), (uint32_t)(loop / g), 1);
    if (loop % g) sq_cat(p, "+");                       /* (a loop set with a finer grid) */
    sq_text(r, OP1_Q10(RL_LOOP_TX), y, s, t, 0x380, rl_red);
    {                                                   /* play state */
        const uint8_t *c = b->run ? rl_red : rl_dimred;
        const char *st = b->run ? "PLAY" : "HOLD";
        int32_t x0 = OP1_Q10(RL_BOX_X0), x1 = OP1_Q10(RL_BOX_X1), y0 = OP1_Q10(RL_BOX_Y0), y1 = OP1_Q10(RL_BOX_Y1), th = 0x180;
        sq_line(r, x0 - th / 2, y0, x1 + th / 2, y0, th, c);
        sq_line(r, x0 - th / 2, y1, x1 + th / 2, y1, th, c);
        sq_line(r, x0, y0, x0, y1, th, c);
        sq_line(r, x1, y0, x1, y1, th, c);
        sq_text(r, (x0 + x1) / 2 - sq_width(s, st) / 2, y, s, st, 0x380, c);
    }
}

static void rl_screen_update(void *scr, int dt, int pos)
{
    op1_screen_draw(scr, dt);
    rl_screen_draw(op1_raster(), pos);
}

#ifndef OP1_HOST
/* ======================= firmware player (extra sequencer "roll", type 3 + rl_active[mode], like euclid) =======
 * DB row seq_types "roll"; dispatcher.c: SeqIds::from_name sets rl_active, create_player case 3 -> rl_player_get,
 * screen object in the finger slot 0x2D. Note keys edit only while the roll screen is shown in synth mode and are
 * not consumed (they sound), except Shift + key (removal). Drum mode: passive player. */
#define RL_MODE() (*(volatile int *)0x01DE4C40 == 1)
#define RL_SYNTH_OCT() (((const int *)0x01DFE4E0)[*(volatile int *)0xFF8016D4 & 7])
static int rl_nop(void *self) { (void)self; return 0; }
static int rl_pkey_down(void *self, int key)
{
    (void)self;
    if (RL_MODE() || key < 0 || key > 23 || *(volatile int *)0x018B65C8 != 0x2D) return 0;
    return rl_key(0, key + 0x35 + RL_SYNTH_OCT() * 12, *(volatile uint8_t *)0x01DD7F78);
}
#pragma GCC pop_options
/* the beat clock passed a beat since the last tick (the time to the next beat went up), or it stands still */
static int32_t rl_last_next = -1;
static int rl_beat(void)
{
    op1_beat_t c;
    op1_beat_clock(&c);
    if (c.len <= 0 || c.step <= 0 || c.next < 0 || c.next > c.len) { rl_last_next = -1; return 1; }
    int beat = rl_last_next >= 0 && c.next > rl_last_next;
    rl_last_next = c.next;
    return beat;
}
static void rl_ptick(void *self, int counter)
{
    (void)self; (void)counter;
    int beat = rl_beat();                               /* (every tick: the reference stays current) */
    if (!RL_MODE()) rl_tick(beat);
}
static void rl_preset(void *self) { (void)self; rl_rearm(); }
#pragma GCC push_options
#pragma GCC optimize("Os")
static const uint32_t rl_vt[16] = {
    0, 0,  0, (uint32_t)rl_nop,  0, (uint32_t)rl_pkey_down,  0, (uint32_t)rl_nop,
    0, (uint32_t)rl_preset,  0, (uint32_t)rl_ptick,  0, (uint32_t)rl_nop,  0, (uint32_t)rl_preset };
static struct { const uint32_t *vt; uint32_t pad[7]; } rl_player = { rl_vt, { 0 } };
volatile uint8_t rl_active[2];                          /* roll selected (synth, drum) */

/* persistence: bg_saver + DB as euclid, table rollseq (is_synth, data); seq_text.h: sq_store_* */
static char rl_sql[2800];                               /* 128 notes: row <= 2330 chars */
static sq_store_t rl_store = { .name = "rollseq", .ser = rl_serialize, .parse = rl_parse,
                               .sql = rl_sql, .cap = sizeof rl_sql, .maxlen = 2700 };
static void rl_changed(void) { sq_store_dirty(&rl_store); }
void *rl_player_get(void)                               /* roll (re)selected: held */
{
    rl_bank_init(&rl);
    sq_store_load(&rl_store);
    if (!RL_MODE()) { rl.run = 0; rl_rearm(); }
    return &rl_player;
}

static void rl_scr_init(void *scr) { (void)scr; }
static void rl_scr_update(void *scr, int dt) { rl_screen_update(scr, dt, RL_MODE()); }
const op1_screen_t SCREEN_roll = { rl_scr_init, rl_scr_update };
#endif
#pragma GCC pop_options
