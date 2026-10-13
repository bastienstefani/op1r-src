/* drive: screen layout.  Include after drive_dsp.h and drive_font.h.
 *
 *   MODE NAME (green)                          PRE LP (white)
 *   in ~~~~   >   [ transfer curve + dot ]   >   ~~~~ out
 *                         o o   (page)
 *   DRIVE      MODE       FREQ       MIX        <- page 1; page 2: ROUTE TYPE RES LEVEL
 *   45%        3/13       1.2KHZ     100%
 *   ======     ===        =====      ========   (one bar per encoder, in its colour)
 */

typedef struct {
    int32_t raw[8];             /* the FX params as stored (0..32767) */
    int32_t page;               /* 0 or 1 */
    int32_t peak;               /* input peak, Q15 (from the audio side) */
    int32_t phase;              /* animation, 0..32767 per wave period */
} DvView;

/* param indices: page 1 = blue green white orange, page 2 the same; 4 = the op1r selector 0x5304 (never edited) */
#define P_DRIVE 0
#define P_MODE  1
#define P_FREQ  2
#define P_MIX   3
#define P_MARK  4
#define P_ROUTE 5               /* route x type, 9 steps */
#define P_RES   6
#define P_LEVEL 7

static const char *const MODE_NAME[DV_NMODES] = {
    "TUBE", "SOFT CLIP", "HARD CLIP", "DIODE 1", "DIODE 2", "LIN FOLD", "SIN FOLD", "ZERO-SQUARE",
    "DOWNSAMPLE", "ASYM", "RECTIFY", "X-SHAPER", "X-SHAPER ASYM",
};
static const char *const ROUTE_NAME[3] = { "OFF", "PRE", "POST" };
static const char *const TYPE_NAME[3] = { "LP", "BP", "HP" };

static void dv_params_of(DvParams *P, const int32_t *raw) {
    dv_decode(P, raw[P_DRIVE], raw[P_MODE], raw[P_FREQ], raw[P_MIX], raw[P_ROUTE], raw[P_RES], raw[P_LEVEL]);
}

/* the curve as heard: shaper (or rectifier), then dry/wet; x in Q15 */
static int32_t dv_curve(const DvParams *P, int32_t x) {
    int32_t y = P->mode == M_RECT ? dv_rect(x, P->amt_q15)
              : P->mode == M_DOWNS ? x
              : dv_shape(P->mode, (x * P->gain_q8) >> 8);
    return x + (((y - x) * P->mix_q15) >> 15);
}

/* one period of a full-scale sine through the curve, starting at phase ph; out = 0: the input */
static int32_t dv_wave(const DvParams *P, int32_t i, int32_t n, int32_t ph, int out) {
    int32_t t = (int32_t) (((int64_t) i << 15) / n) + ph, x;
    if (out && P->mode == M_DOWNS) {                /* held steps: assume a 441 Hz wave (100 samples/period) */
        int32_t step = (P->ds_n << 15) / 100;
        t = ph + ((t - ph) / step) * step;
    }
    x = sin_q(t * 4);                               /* t: 0..32767 = one period */
    return out ? dv_curve(P, x) : x;
}

static void dv_draw_wave(void *r, const DvParams *P, int32_t x0, int32_t w, int32_t yc, int32_t amp,
                         int32_t ph, int out, const uint8_t c[4]) {
    const int32_t n = 64;
    int32_t i, px = 0, py = 0;
    for (i = 0; i <= n; i++) {
        int32_t x = PX(x0) + (PX(w) * i) / n;
        int32_t y = PX(yc) - (int32_t) (((int64_t) dv_wave(P, i, n, ph, out) * PX(amp)) >> 15);
        if (i) seg(r, px, py, x, y, 1536, c);
        px = x; py = y;
    }
}

static void dv_draw_screen(void *r, const DvView *v) {
    DvParams P;
    char buf[24], *p;
    int32_t i, bx0 = 114, by0 = 22, bs = 92, cx, cy;
    dv_params_of(&P, v->raw);

    /* header: mode name (green encoder), filter state (page 2) */
    text(r, PX(12), PX(5), MODE_NAME[P.mode], 1536, 1280, C_GREEN);
    if (P.route == R_OFF) {
        text(r, PX(308) - text_width("FILTER OFF", 1280), PX(6), "FILTER OFF", 1280, 1024, C_DIM);
    } else {
        p = put_s(buf, ROUTE_NAME[P.route]); p = put_s(p, " "); put_s(p, TYPE_NAME[P.ftype]);
        text(r, PX(308) - text_width(buf, 1536), PX(5), buf, 1536, 1280, C_WHITE);
    }

    /* the curve box: corner brackets, dotted axes */
    cx = bx0 + bs / 2; cy = by0 + bs / 2;
    {
        const int32_t L = 8, w = 1280;
        int32_t x1 = bx0 + bs, y1 = by0 + bs;
        seg(r, PX(bx0), PX(by0), PX(bx0 + L), PX(by0), w, C_DIM); seg(r, PX(bx0), PX(by0), PX(bx0), PX(by0 + L), w, C_DIM);
        seg(r, PX(x1), PX(by0), PX(x1 - L), PX(by0), w, C_DIM);  seg(r, PX(x1), PX(by0), PX(x1), PX(by0 + L), w, C_DIM);
        seg(r, PX(bx0), PX(y1), PX(bx0 + L), PX(y1), w, C_DIM);  seg(r, PX(bx0), PX(y1), PX(bx0), PX(y1 - L), w, C_DIM);
        seg(r, PX(x1), PX(y1), PX(x1 - L), PX(y1), w, C_DIM);    seg(r, PX(x1), PX(y1), PX(x1), PX(y1 - L), w, C_DIM);
        for (i = 4; i < bs - 4; i += 6) {
            seg(r, PX(bx0 + i), PX(cy), PX(bx0 + i + 2), PX(cy), 1024, C_GRID);
            seg(r, PX(cx), PX(by0 + i), PX(cx), PX(by0 + i + 2), 1024, C_GRID);
        }
    }
    /* transfer curve: a wide dark line under a thin bright one (Serum's glow) */
    {
        const int32_t n = 64;
        int32_t px = 0, py = 0, pass;
        for (pass = 0; pass < 2; pass++)
            for (i = 0; i <= n; i++) {
                int32_t xin = -32767 + (65534 * i) / n;
                int32_t y = dv_curve(&P, xin);
                int32_t X = PX(cx) + (int32_t) (((int64_t) xin * PX(bs / 2 - 2)) >> 15);
                int32_t Y = PX(cy) - (int32_t) (((int64_t) y * PX(bs / 2 - 2)) >> 15);
                if (i) seg(r, px, py, X, Y, pass ? 1536 : 5120, pass ? C_BLUE : C_GLOW);
                px = X; py = Y;
            }
    }
    /* the dot: where the signal peak sits on the curve, with guides to the axes */
    {
        int32_t xin = sat15(v->peak), y = dv_curve(&P, xin);
        int32_t X = PX(cx) + (int32_t) (((int64_t) xin * PX(bs / 2 - 2)) >> 15);
        int32_t Y = PX(cy) - (int32_t) (((int64_t) y * PX(bs / 2 - 2)) >> 15);
        seg(r, X, PX(cy), X, Y, 1024, C_DIM);
        seg(r, PX(cx), Y, X, Y, 1024, C_DIM);
        seg(r, X - 1, Y, X + 1, Y, 6144, C_WHITE);
    }

    /* in -> curve -> out */
    dv_draw_wave(r, &P, 14, 88, cy, 26, v->phase, 0, C_DIM);
    dv_draw_wave(r, &P, 218, 88, cy, 26, v->phase, 1, C_WHITE);
    for (i = 0; i < 2; i++) {                       /* chevrons */
        int32_t x = i ? 210 : 106;
        seg(r, PX(x), PX(cy - 3), PX(x + 3), PX(cy), 1280, C_DIM);
        seg(r, PX(x + 3), PX(cy), PX(x), PX(cy + 3), 1280, C_DIM);
    }

    /* page dots under the box */
    seg(r, PX(cx - 5) - 1, PX(by0 + bs + 6), PX(cx - 5) + 1, PX(by0 + bs + 6), v->page ? 2048 : 4096, v->page ? C_DIM : C_WHITE);
    seg(r, PX(cx + 5) - 1, PX(by0 + bs + 6), PX(cx + 5) + 1, PX(by0 + bs + 6), v->page ? 4096 : 2048, v->page ? C_WHITE : C_DIM);

    /* the four encoders: label, value, bar */
    for (i = 0; i < 4; i++) {
        static const uint8_t *const COL[4] = { C_BLUE, C_GREEN, C_WHITE, C_ORANGE };
        const char *label;
        int32_t x0 = 10 + i * 78, raw, dim = 0;
        p = buf;
        if (!v->page) {
            static const char *const L1[4] = { "DRIVE", "MODE", "FREQ", "MIX" };
            label = L1[i];
            raw = v->raw[i];
            switch (i) {
            case 0: p = put_dec(p, (raw * 100 + 16383) / 32767); put_s(p, "%"); break;
            case 1: p = put_dec(p, P.mode + 1); p = put_s(p, "/"); put_dec(p, DV_NMODES); break;
            case 2: {
                int32_t hz = lut33(FC_HZ, raw);
                if (hz >= 1000) { p = put_dec(p, hz / 1000); p = put_s(p, "."); p = put_dec(p, (hz % 1000) / 100); put_s(p, "KHZ"); }
                else { p = put_dec(p, hz); put_s(p, "HZ"); }
                dim = P.route == R_OFF;
                break;
            }
            default: p = put_dec(p, (raw * 100 + 16383) / 32767); put_s(p, "%"); break;
            }
        } else {
            static const char *const L2[4] = { "ROUTE", "TYPE", "RES", "LEVEL" };
            label = L2[i];
            switch (i) {
            case 0: raw = v->raw[P_ROUTE]; put_s(p, ROUTE_NAME[P.route]); break;
            case 1: raw = v->raw[P_ROUTE]; put_s(p, TYPE_NAME[P.ftype]); dim = P.route == R_OFF; break;
            case 2: raw = v->raw[P_RES]; p = put_dec(p, (raw * 100 + 16383) / 32767); put_s(p, "%"); dim = P.route == R_OFF; break;
            default: {
                int32_t db;
                raw = v->raw[P_LEVEL];
                db = -24 + (raw * 30 + 16383) / 32767;
                if (db > 0) p = put_s(p, "+");
                p = put_dec(p, db); put_s(p, " DB");
                break;
            }
            }
            if (i == 0) raw = P.route * 16383;      /* bars: position within the choice */
            if (i == 1) raw = P.ftype * 16383;
        }
        text(r, PX(x0), PX(124), label, 1024, 1024, C_DIM);
        text(r, PX(x0), PX(134), buf, 1536, 1280, dim ? C_DIM : COL[i]);
        seg(r, PX(x0), PX(152), PX(x0 + 66), PX(152), 1024, C_GRID);
        seg(r, PX(x0), PX(152), PX(x0) + (int32_t) (((int64_t) clamp32(raw, 0, 32767) * PX(66)) >> 15), PX(152), 3072,
            dim ? C_DIM : COL[i]);
    }
}
