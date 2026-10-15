/* Host test of plugins/drive (the same code the OP-1 runs):
 *   cc -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover -DOP1_HOST -I . harness/drive_test.c -lm \
 *      -o drive_test && ./drive_test [curves.csv]
 * - shapers: the tanh and x / (1 + |x|) tables against the exact formulas over their whole range;
 * - DSP: every mode x drive x filter x resonance x cutoff with sines, noise and full-scale steps under UBSan (no
 *   signed overflow), the filter does not lock up; per-mode figures (RMS of a -6 dB sine, DC);
 * - words: the 7 filter zones, page-2 words, positions independent, init resets, the screen draws;
 * - with a file name: the transfer curves as CSV (x, then each mode at drive 0 / 50 / 100 %). */
#include "../plugins/drive/drive.c"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6];
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4]; volatile uint8_t op1_eng_page[4];
void *host_find_node(void *s, const char *id) { (void)s; (void)id; return 0; }
void host_node_set_transform(void *n, const int32_t *m) { (void)n; (void)m; }
void host_node_set_visible(void *n, int on) { (void)n; (void)on; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; }
static int nlines;
void *host_raster(void) { return &nlines; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c)
{ (void)r; (void)a; (void)b; (void)w; (void)c; nlines++; }

static const char *NAMES[DV_NMODES] = { "tube", "soft", "hard", "diode1", "diode2", "linfold", "sinfold",
                                         "zerosq", "downs", "asym", "rect", "xshp", "xshpa" };
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void shapers(void)
{
    int32_t et = 0, eh = 0;
    for (int32_t x = -140000; x <= 140000; x += 7) {
        double t = x / 32768.0, a = fabs(t) < 3 ? fabs(t) : 3, h = fabs(t) < 16 ? fabs(t) : 16;
        int32_t rt = (int32_t)lrint(32768 * copysign(a * (27 + a * a) / (27 + 9 * a * a), t));
        int32_t rh = (int32_t)lrint(32768 * copysign(h / (1 + h), t));
        rt = rt > 32767 ? 32767 : rt < -32767 ? -32767 : rt;
        rh = rh > 32767 ? 32767 : rh < -32767 ? -32767 : rh;
        if (abs(tanh_q(x) - rt) > et) et = abs(tanh_q(x) - rt);
        if (abs(hyp_q(x) - rh) > eh) eh = abs(hyp_q(x) - rh);
    }
    printf("shapers: tanh table error %d LSB, x/(1+|x|) table error %d LSB\n", et, eh);
    CHECK(et <= 2 && eh <= 3, "shaper tables too far from the formulas");
}

static void dsp_sweep(void)
{
    static const int32_t V[] = { 0, 1, 1000, 8192, 16384, 24576, 32000, 32767 };
    int nv = sizeof V / sizeof V[0];
    long blocks = 0;
    unsigned seed = 1;
    int16_t buf[64];
    for (int mode = 0; mode < DV_NMODES; mode++)
    for (int di = 0; di < nv; di++)
    for (int f = 0; f < DV_NFILTERS; f++)
    for (int ri = 0; ri < nv; ri += 3)
    for (int fi = 0; fi < nv; fi += 2) {
        DvParams P;
        DvState S = { 0 };
        dv_decode(&P, V[di], mode * 32768 / DV_NMODES + 100, V[fi], 32767, f * 32768 / DV_NFILTERS + 100, V[ri], 32767);
        CHECK(P.mode == mode, "decode mode %d", mode);
        CHECK(f ? P.route == (f - 1) / 3 + 1 && P.ftype == (f - 1) % 3 : P.route == R_OFF, "decode filter %d", f);
        for (int sig = 0; sig < 4; sig++) {
            double ph = 0;
            for (int b = 0; b < 400; b++) {
                for (int i = 0; i < 64; i++) {
                    int32_t s;
                    switch (sig) {
                    case 0: s = (int32_t)(32767 * sin(ph)); ph += 2 * M_PI * 110 / 44100; break;
                    case 1: s = (int32_t)(32767 * sin(ph)); ph += 2 * M_PI * 9000 / 44100; break;
                    case 2: seed = seed * 1103515245u + 12345u; s = (int16_t)(seed >> 16); break;
                    default: s = ((b * 64 + i) / 300) & 1 ? 32767 : -32768; break;
                    }
                    buf[i] = (int16_t)s;
                }
                dv_block(&S, &P, buf, 64);
                blocks++;
            }
            CHECK(!(abs(S.lp) >= 65535 && abs(S.bp) >= 65535), "filter stuck: mode %s drive %d filter %d res %d f %d sig %d",
                  NAMES[mode], V[di], f, V[ri], V[fi], sig);
        }
    }
    printf("dsp: %ld blocks\n", blocks);

    /* per-mode figures: -6 dB 220 Hz sine, drive 0 / 50 / 100 %, no filter, full wet, 0 dB level */
    for (int mode = 0; mode < DV_NMODES; mode++) {
        printf("  %-8s", NAMES[mode]);
        for (int d = 0; d <= 2; d++) {
            DvParams P;
            DvState S = { 0 };
            double ph = 0, ss = 0, sum = 0;
            int cnt = 0;
            dv_decode(&P, d * 16383, mode * 32768 / DV_NMODES + 100, 0, 32767, 0, 0, 26214);
            for (int b = 0; b < 700; b++) {
                for (int i = 0; i < 64; i++) { buf[i] = (int16_t)(16384 * sin(ph)); ph += 2 * M_PI * 220 / 44100; }
                dv_block(&S, &P, buf, 64);
                if (b >= 200) for (int i = 0; i < 64; i++) { ss += (double)buf[i] * buf[i]; sum += buf[i]; cnt++; }
            }
            printf("  d%3d%%: rms %5.1f dBFS dc %6.3f", d * 50, 10 * log10(ss / cnt / (32768.0 * 32768)), sum / cnt / 32768);
        }
        printf("\n");
    }
}

static void plugin(void)
{
    static const int16_t def[8] = { 9830, 1260, 16384, 32678, 0x5301, 2340, 26214, 6553 };   /* host: words as knobs */
    int16_t a[64], b[64];
    DvParams P;
    int32_t raw[8];
    CHECK(FX_drive.press[0] == OP1_PRESS_PAGE && FX_drive.press[1] == OP1_PRESS_PAGE &&
          FX_drive.press[2] == OP1_PRESS_PAGE && FX_drive.press[3] == OP1_PRESS_NONE, "press modes");
    raw_of(raw, def);
    dv_params_of(&P, raw);
    CHECK(P.mode == M_TUBE && P.route == R_OFF && P.lvl_q12 > 4000 && P.lvl_q12 < 4200, "defaults: tube, filter off, 0 dB");
    for (int f = 0; f < DV_NFILTERS; f++) {         /* zone centres as the stock encoder leaves them */
        int16_t w[8];
        for (int i = 0; i < 8; i++) w[i] = def[i];
        w[P_FILTER] = (int16_t)((2 * f + 1) * 32768 / (2 * DV_NFILTERS));
        raw_of(raw, w); dv_params_of(&P, raw);
        CHECK(f ? P.route == (f - 1) / 3 + 1 && P.ftype == (f - 1) % 3 : P.route == R_OFF, "filter zone %d", f);
    }
    /* positions are independent, init resets */
    for (int pos = 0; pos < 3; pos++) FX_drive.init(pos);
    for (int i = 0; i < 64; i++) a[i] = b[i] = (int16_t)(20000 * sin(i * 0.2));
    FX_drive.render(0, a, def);
    for (int k = 0; k < 20; k++) { int16_t c[64] = { 0 }; FX_drive.render(1, c, def); }
    FX_drive.init(0);
    FX_drive.render(0, b, def);
    int same = 1;
    for (int i = 0; i < 64; i++) same &= a[i] == b[i];
    CHECK(same, "position 0 after init renders as the first time, whatever position 1 did");
    CHECK(st[2].inited == 0, "master untouched");
    /* screen: page 1 and every knob on page 2 */
    for (int pg = 0; pg < 2; pg++) {
        for (int k = 0; k < 4; k++) op1_fx_page[0][k] = (uint8_t)pg;
        for (int i = 0; i < 8; i++) host_fx_knobs[0][i] = def[i];
        nlines = 0;
        SCREEN_drive.update(0, 16);
        CHECK(nlines > 300, "screen page %d draws (%d lines)", pg + 1, nlines);
    }
}

int main(int argc, char **argv)
{
    shapers();
    dsp_sweep();
    plugin();
    if (argc > 1) {
        FILE *f = fopen(argv[1], "w");
        for (int i = -256; i <= 256 && f; i++) {
            int32_t x = i * 128;
            fprintf(f, "%d", x);
            for (int mode = 0; mode < DV_NMODES; mode++)
                for (int d = 0; d <= 2; d++) {
                    DvParams P;
                    dv_decode(&P, d * 16383, mode * 32768 / DV_NMODES + 100, 0, 32767, 0, 0, 26214);
                    fprintf(f, ",%d", mode == M_RECT ? dv_rect(x, P.amt_q15) : dv_shape(mode, (x * P.gain_q8) >> 8));
                }
            fprintf(f, "\n");
        }
        if (f) fclose(f);
    }
    printf(fails ? "drive: %d FAILED\n" : "drive: all ok\n", fails);
    return fails != 0;
}
