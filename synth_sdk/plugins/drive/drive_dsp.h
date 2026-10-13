/* drive: the audio core of the distortion FX.  Fixed point (the Blackfin has no FPU), no firmware dependency, so
 * harness/drive_test.c runs the same code on a PC.
 *
 * Signal path, as in Serum's distortion module:   in -> [filter PRE] -> drive -> shaper -> [filter POST] -> mix -> level
 * The modes are our own formulas, named after Serum's and written to behave like them by ear: they are not
 * copies of Serum's curves.
 *
 * Formats: samples int16; Q15 = 1.0 is 32768.  After the drive gain a sample can reach 40.0 (int32).
 */
#include <stdint.h>
#include "drive_tables.h"

enum {
    M_TUBE, M_SOFT, M_HARD, M_DIODE1, M_DIODE2, M_LINFOLD, M_SINFOLD, M_ZEROSQ, M_DOWNS, M_ASYM, M_RECT,
    M_XSHP, M_XSHPA, DV_NMODES
};
enum { R_OFF, R_PRE, R_POST };
enum { F_LP, F_BP, F_HP };

/* raw parameter values are 0..32767 (one encoder range); u15 = that value */
typedef struct {
    int32_t gain_q8;            /* drive gain (1.0 = 256) */
    int32_t f_q14, q_q14;       /* filter coefficient and damping */
    int32_t mix_q15, lvl_q12;   /* dry/wet, output level */
    int32_t ds_n;               /* downsample hold length */
    int32_t amt_q15;            /* rectify: amount (0 clean, 0.5 half-wave, 1 full-wave) */
    uint8_t mode, route, ftype;
} DvParams;

typedef struct {
    int32_t gain_q8, mix_q15, lvl_q12;      /* values reached at the end of the last block (ramped) */
    int32_t lp, bp;                         /* filter state, Q13 */
    int32_t dcx, dcy;                       /* DC blocker state, Q15 */
    int32_t hold, hold_ph;                  /* downsample */
    int32_t peak;                           /* input peak of the last block, Q15 (for the screen) */
    uint8_t inited;
} DvState;

static inline int32_t clamp32(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline int32_t sat15(int32_t x) { return clamp32(x, -32767, 32767); }

/* table lookup with linear interpolation, u = 0..32767 over 33 points */
static int32_t lut33(const int16_t *t, int32_t u) {
    int32_t i, fr;
    u = clamp32(u, 0, 32767);
    i = u >> 10;                                    /* 0..31 */
    fr = u & 1023;
    return t[i] + (((t[i + 1] - t[i]) * fr) >> 10);
}

/* sin(pi/2 * x), x in Q15 (1.0 = a quarter turn), any int32 */
static int32_t sin_q(int32_t x) {
    uint32_t u = (uint32_t) x & 0x1ffff;            /* mod 4.0 (one turn) */
    int32_t q = (int32_t) (u >> 15), r = (int32_t) (u & 0x7fff), v, i, fr, s;
    v = (q & 1) ? 32768 - r : r;                    /* 0..32768 */
    i = v >> 7;
    fr = v & 127;
    s = (i >= 256) ? SINQ[256] : SINQ[i] + (((SINQ[i + 1] - SINQ[i]) * fr) >> 7);
    return (q & 2) ? -s : s;
}

/* tanh(x), Pade (x(27 + x^2) / (27 + 9 x^2)), exact 1.0 at |x| = 3 */
static int32_t tanh_q(int32_t x) {
    int32_t t, t2, num, den;
    x = clamp32(x, -98304, 98304);
    t = x >> 3;                                     /* Q12, |t| <= 12288 */
    t2 = (t * t) >> 12;                             /* <= 36864 */
    num = t * (110592 + t2);                        /* Q24, |num| <= 1.82e9 */
    den = (110592 + 9 * t2) >> 3;                   /* Q9 */
    return sat15(num / den);
}

/* x / (1 + |x|): softer knee than tanh */
static int32_t hyp_q(int32_t x) {
    int32_t a;
    x = clamp32(x, -524288, 524288);                /* 16.0 */
    a = x < 0 ? -x : x;
    return sat15((x * 2048) / ((32768 + a) >> 4));
}

/* triangle fold: period 4.0, folds at +-1 */
static int32_t fold_q(int32_t x) {
    int32_t u = (int32_t) (((uint32_t) x + 32768u) & 0x1ffff);
    return sat15(u < 65536 ? u - 32768 : 98304 - u);
}

/* The shaper.  x = driven input (Q15, may exceed 1.0).  Pure function (the downsample mode is done in
 * dv_block), so the screen draws its curve with the same code. */
static int32_t dv_shape(int mode, int32_t x) {
    int32_t c = sat15(x), y;
    switch (mode) {
    case M_TUBE:                                    /* biased soft clip: even harmonics, warm */
        y = tanh_q(x + 8192) - 8025;                /* tanh(x + 0.25) - tanh(0.25) */
        return sat15((y * 26214) >> 15);            /* x 0.8 */
    case M_SOFT:  return tanh_q(x);
    case M_HARD:  return c;
    case M_DIODE1:                                  /* one half soft-clipped, the other almost untouched */
        return x >= 0 ? tanh_q(x) : tanh_q(x >> 2);
    case M_DIODE2:                                  /* softer, asymmetric knee */
        return x >= 0 ? hyp_q(x) : hyp_q(x >> 1);
    case M_LINFOLD: return fold_q(x);
    case M_SINFOLD: return sat15(sin_q(clamp32(x, -1310720, 1310720)));
    case M_ZEROSQ:                                  /* sign(x) x^2: squeezes the zero crossing */
        y = (c * c) >> 15;
        return x < 0 ? -y : y;
    case M_ASYM:                                    /* offset hard clip: the positive half clips first */
        return sat15(c + 13107 > 32767 ? 32767 - 13107 : c);
    case M_RECT:                                    /* rectifier: done in dv_rect (needs the amount) */
    case M_XSHP:                                    /* x - 0.15 sin(2 pi x): a stepped transfer curve */
        return sat15(c - ((4915 * sin_q(c * 4)) >> 15));
    case M_XSHPA:                                   /* the same on the positive half only */
        return c >= 0 ? sat15(c - ((4915 * sin_q(c * 4)) >> 15)) : c;
    case M_DOWNS:
    default:      return c;
    }
}

static int dv_needs_dc(int mode) {
    return mode == M_TUBE || mode == M_DIODE1 || mode == M_DIODE2 || mode == M_ASYM || mode == M_RECT || mode == M_XSHPA;
}

/* rectify: drive morphs clean -> half-wave (50 %) -> full-wave (100 %); the DC blocker recentres it */
static int32_t dv_rect(int32_t x, int32_t amt_q15) {
    int32_t c = sat15(x), a = c < 0 ? -c : c;
    return sat15(c + (((a - c) * amt_q15) >> 15));  /* |a - c| <= 65534 */
}

/* params (8 x 0..32767, words 0..3 normalised from the stored words) -> working values.  Layout: see drive.c. */
static void dv_decode(DvParams *P, int32_t drive, int32_t mode, int32_t freq, int32_t mix,
                      int32_t route_type, int32_t res, int32_t level) {
    int32_t rt;
    P->mode = (uint8_t) clamp32((clamp32(mode, 0, 32767) * DV_NMODES) >> 15, 0, DV_NMODES - 1);
    if (P->mode == M_DOWNS || P->mode == M_RECT)
        P->gain_q8 = 256;                           /* drive sets the amount, not a gain */
    else if (P->mode == M_LINFOLD || P->mode == M_SINFOLD)
        P->gain_q8 = 256 + ((clamp32(drive, 0, 32767) * 9) >> 7);   /* folds: 1x .. 10x */
    else
        P->gain_q8 = lut33(DRIVE_Q8, drive);
    P->amt_q15 = clamp32(drive, 0, 32767);
    P->ds_n = lut33(DS_N, drive);
    if (P->ds_n < 1) P->ds_n = 1;
    P->f_q14 = lut33(SVF_F_Q14, freq);
    P->q_q14 = 32604 - ((clamp32(res, 0, 32767) * 31600) >> 15);     /* damping 1.99 .. 0.06 */
    P->mix_q15 = clamp32(mix, 0, 32767);
    P->lvl_q12 = lut33(LEVEL_Q12, level);
    rt = clamp32((clamp32(route_type, 0, 32767) * 9) >> 15, 0, 8);   /* 9 combinations of route x type */
    P->route = (uint8_t) (rt / 3);
    P->ftype = (uint8_t) (rt % 3);
}

/* 2x oversampled Chamberlin state-variable filter, one sample; x and result in Q15 */
static int32_t dv_filter(DvState *S, const DvParams *P, int32_t x) {
    int32_t in = sat15(x) >> 2, hp = 0, k;                          /* Q13 */
    for (k = 0; k < 2; k++) {
        S->lp = clamp32(S->lp + ((P->f_q14 * S->bp) >> 14), -65535, 65535);
        hp = clamp32(in - S->lp - ((P->q_q14 * S->bp) >> 14), -65535, 65535);
        S->bp = clamp32(S->bp + ((P->f_q14 * hp) >> 14), -65535, 65535);
    }
    k = P->ftype == F_LP ? S->lp : P->ftype == F_BP ? S->bp : hp;
    return sat15(k * 4);
}

static void dv_reset(DvState *S, const DvParams *P) {
    S->gain_q8 = P->gain_q8;
    S->mix_q15 = P->mix_q15;
    S->lvl_q12 = P->lvl_q12;
    S->lp = S->bp = S->dcx = S->dcy = S->hold = S->hold_ph = S->peak = 0;
    S->inited = 1;
}

/* process n samples in place (stride = distance between samples, in int16) */
static void dv_block(DvState *S, const DvParams *P, int16_t *buf, int n, int stride) {
    int32_t g = S->gain_q8, m = S->mix_q15, l = S->lvl_q12;
    int32_t dg, dm, dl, peak = 0;
    int i, dc = dv_needs_dc(P->mode);
    if (!S->inited || n <= 0) { dv_reset(S, P); g = S->gain_q8; m = S->mix_q15; l = S->lvl_q12; if (n <= 0) return; }
    dg = (P->gain_q8 - g) / n;                      /* linear ramps over the block: no zipper noise */
    dm = (P->mix_q15 - m) / n;
    dl = (P->lvl_q12 - l) / n;
    for (i = 0; i < n; i++, buf += stride) {
        int32_t dry = *buf, x = dry, y, a;
        g += dg; m += dm; l += dl;
        if (P->route == R_PRE) x = dv_filter(S, P, x);
        a = x < 0 ? -x : x;
        if (a > peak) peak = a;
        if (P->mode == M_DOWNS) {
            if (S->hold_ph <= 0) { S->hold = x; S->hold_ph = P->ds_n; }
            S->hold_ph--;
            y = S->hold;
        } else if (P->mode == M_RECT) {
            y = dv_rect(x, P->amt_q15);
        } else {
            y = dv_shape(P->mode, (x * g) >> 8);    /* |x * g| <= 32768 * 10240: fits */
        }
        if (dc) {                                   /* y - x1 + 0.995 y1 (about 35 Hz) */
            int32_t o = y - S->dcx + ((32604 * S->dcy) >> 15);
            S->dcx = y;
            S->dcy = clamp32(o, -65535, 65535);
            y = sat15(o);
        }
        if (P->route == R_POST) y = dv_filter(S, P, y);
        y = dry + (((y - dry) * m) >> 15);          /* |y - dry| <= 65534, m <= 32767 */
        *buf = (int16_t) sat15((y * l) >> 12);      /* l <= 8173 */
    }
    S->gain_q8 = P->gain_q8;                        /* land exactly on the targets */
    S->mix_q15 = P->mix_q15;
    S->lvl_q12 = P->lvl_q12;
    S->peak = peak;
}
