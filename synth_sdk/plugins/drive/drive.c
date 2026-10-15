/* "drive" - Serum-style distortion FX with its own screen (drive.svg, everything drawn by code).
 * Copyright (C) 2026 bastienstefani. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 *
 * Signal path (drive_dsp.h): in -> [filter PRE] -> drive -> shaper -> [filter POST] -> mix -> level.
 * 13 shaper modes (our own formulas, named after Serum's): tube, soft clip, hard clip, diode 1 / 2, linear fold,
 * sine fold, zero-square, downsample, asym, rectify, x-shaper, x-shaper asym.
 *
 * Knobs, page 1:  blue drive · green mode (13 zones) · white filter cutoff 30 Hz .. 10 kHz · red mix
 * Press blue / green / white for its page 2 (op1_fx_page):
 *                 blue filter OFF / PRE LP / BP / HP / POST LP / BP / HP · green output level -24 .. +6 dB ·
 *                 white resonance
 * Words (saved with the sound): 0..3 page 1 (raw, converted with op1_fx_param_q15), 4 the selector, 5 filter,
 * 6 level, 7 resonance (0..32767). On the master the FX runs on the mono sum (mixed back as the wet part).
 * Per sample: tables and shifts only; the ramps of drive, mix and level divide once per block. */
#include "op1_plugin.h"
#include "drive_dsp.h"
#include "drive_font.h"
#include "drive_screen.h"

static DvState st[3];                   /* 0 synth, 1 drum, 2 master */

static void raw_of(int32_t *raw, const int16_t *w)      /* stored words -> 0..32767 */
{
    for (int i = 0; i < 4; i++) raw[i] = op1_fx_param_q15(w, i);
    raw[4] = w[4];
    for (int i = 5; i < 8; i++) raw[i] = clamp32(w[i], 0, 32767);
}

static void drive_init(int pos) { st[pos % 3].inited = 0; }   /* dv_block resets the state on its next call */

static void drive_render(int pos, int16_t *buf, const int16_t *p)
{
    int32_t raw[8];
    DvParams P;
    raw_of(raw, p);
    dv_params_of(&P, raw);
    dv_block(&st[pos % 3], &P, buf, OP1_FX_BLOCK);
}

const op1_fx_t FX_drive = { drive_init, drive_render, { OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_NONE } };

/* ======================= screen (GUI thread) ======================= */
static int32_t peak_shown, phase;

static void drive_screen_init(void *scr) { (void)scr; }

static void drive_screen_update(void *scr, int dt)
{
    int pos = op1_fx_screen_pos();
    int32_t pk = st[pos % 3].peak;
    DvView v;
    op1_screen_draw(scr, dt);
    raw_of(v.raw, op1_fx_knobs(pos));
    for (int k = 0; k < 4; k++) v.page[k] = op1_fx_page[pos % 3][k];
    peak_shown = pk > peak_shown ? pk : peak_shown - (peak_shown >> 3);     /* fast up, slow down */
    v.peak = peak_shown;
    phase = (phase + 160) & 32767;
    v.phase = phase;
    dv_draw_screen(op1_raster(), &v);
}

const op1_screen_t SCREEN_drive = { drive_screen_init, drive_screen_update };
