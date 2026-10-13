/* "drive" - Serum-style distortion FX (custom FX, own screen drive.svg, everything drawn by code).
 *
 * Signal path (drive_dsp.h): in -> [filter PRE] -> drive -> shaper -> [filter POST] -> mix -> level.
 * 13 shaper modes (our own formulas, named after Serum's): tube, soft clip, hard clip, diode 1 / 2, linear fold,
 * sine fold, zero-square, downsample, asym, rectify, x-shaper, x-shaper asym.
 *
 * Knobs, page 1:  blue drive · green mode (one mode per detent) · white filter cutoff 30 Hz .. 10 kHz · red mix
 * Page 2 (any encoder press toggles the page of all four knobs):
 *                 blue filter route OFF / PRE / POST · green filter type LP / BP / HP · white resonance ·
 *                 red output level -24 .. +6 dB
 * Words (8 x int16, saved with the sound): 0..3 page 1 (raw, inside the filter type's ranges), 4 selector 0x5304
 * (order 4), 5 route x type (9 zones), 6 resonance, 7 level (0..32767).
 * Drive, cutoff and mix are turned by the stock encoder code (acceleration, LFO targets); the mode and page 2 are
 * edited here (op1_fx_t.knob). On the master both channels are processed (render_stereo).
 * Not quite the plugin rules: the tube / soft / diode shapers divide once per sample (32-bit, libgcc). */
#include "op1_plugin.h"
#include "drive_dsp.h"
#include "drive_font.h"
#include "drive_screen.h"

static DvState st[4];                   /* 0 synth, 1 drum, 2 master L, 3 master R */

static void raw_of(int32_t *raw, const int16_t *w)      /* stored words -> 0..32767 (DvView / dv_params_of) */
{
    for (int i = 0; i < 4; i++) raw[i] = op1_fx_param_q15(w, i);
    raw[P_MARK] = w[P_MARK];
    for (int i = 5; i < 8; i++) raw[i] = clamp32(w[i], 0, 32767);
}

static void params_of(DvParams *P, const int16_t *w)
{
    int32_t raw[8];
    raw_of(raw, w);
    dv_params_of(P, raw);
}

static void drive_init(int pos)
{
    st[pos % 3].inited = 0;                     /* dv_block resets the state on its next call */
    if (pos == OP1_FX_MASTER) st[3].inited = 0;
}

static void drive_render(int pos, int16_t *buf, const int16_t *p)
{
    DvParams P;
    params_of(&P, p);
    dv_block(&st[pos % 3], &P, buf, OP1_FX_BLOCK, 1);
}

static void drive_render_stereo(int16_t *l, int16_t *r, const int16_t *p)
{
    DvParams P;
    params_of(&P, p);
    dv_block(&st[2], &P, l, OP1_FX_BLOCK, 1);
    dv_block(&st[3], &P, r, OP1_FX_BLOCK, 1);
}

/* ---- encoders on the drive page (KeyHandler thread); the page is op1_fx_page[pos][0..3], all four alike ---- */
static int drive_key(int pos, int knob, int pressed)
{
    (void)knob;
    if (pressed) {
        uint8_t pg = !op1_fx_page[pos][0];
        for (int k = 0; k < 4; k++) op1_fx_page[pos][k] = pg;
    }
    return 1;
}

static int32_t zone(int k, int n) { return ((2 * k + 1) * 32768) / (2 * n); }   /* centre of step k of n */

static int drive_knob(int pos, int k, int delta, int16_t *w)
{
    int page = op1_fx_page[pos][0], n = delta / 1024;
    DvParams P;
    if (!page && k != 1) return 0;              /* drive, cutoff, mix: the stock encoder */
    params_of(&P, w);
    if (!page) {                                /* green: one mode per detent */
        int m = clamp32(P.mode + n, 0, DV_NMODES - 1);
        if (m != P.mode) w[P_MODE] = op1_fx_param_raw(P_MODE, zone(m, DV_NMODES));
    } else if (k <= 1) {                        /* blue: route, green: filter type */
        int route = P.route, type = P.ftype;
        if (k == 0) route = clamp32(route + n, 0, 2);
        else type = clamp32(type + n, 0, 2);
        if (route != P.route || type != P.ftype) w[P_ROUTE] = (int16_t)zone(route * 3 + type, 9);
    } else {                                    /* white: resonance, red: level */
        int i = k == 2 ? P_RES : P_LEVEL;
        w[i] = (int16_t)clamp32(w[i] + delta, 0, 32767);
    }
    return 1;
}

const op1_fx_t FX_drive = { drive_init, drive_render, { 0, 0, 0, 0 }, drive_render_stereo, drive_key, drive_knob };

/* ======================= screen (GUI thread) ======================= */
static int32_t peak_shown, phase;

static void drive_screen_init(void *scr) { (void)scr; }

static void drive_screen_update(void *scr, int dt)
{
    int pos = op1_fx_screen_pos();
    int32_t pk = pos == OP1_FX_MASTER ? (st[2].peak > st[3].peak ? st[2].peak : st[3].peak) : st[pos].peak;
    DvView v;
    op1_screen_draw(scr, dt);
    raw_of(v.raw, op1_fx_knobs(pos));
    peak_shown = pk > peak_shown ? pk : peak_shown - (peak_shown >> 3);     /* fast up, slow down */
    v.peak = peak_shown;
    v.page = op1_fx_page[pos][0];
    phase = (phase + 160) & 32767;
    v.phase = phase;
    dv_draw_screen(op1_raster(), &v);
}

const op1_screen_t SCREEN_drive = { drive_screen_init, drive_screen_update };
