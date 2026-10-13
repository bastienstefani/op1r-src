/* Host test of plugins/drive (the same code the OP-1 runs):
 *   cc -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover -DOP1_HOST -I.. drive_test.c -lm \
 *      -o build/drive_test && build/drive_test [curves.csv]
 * - DSP: every mode x drive x filter route / type x resonance x cutoff with sines, noise and full-scale steps under
 *   UBSan (no signed overflow), the filter does not lock up; per-mode figures (RMS of a -6 dB sine, DC);
 * - FX callbacks: green steps one mode per detent (page 1), any press toggles page 2 for all four knobs, page 2
 *   steps route / type and moves resonance / level, page 1 drive / cutoff / mix go to the stock encoder;
 *   master renders both channels, init resets the state;
 * - with a file name: the transfer curves as CSV. */
#include "../plugins/drive/drive.c"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6];
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3]; volatile uint8_t op1_fx_page[3][4];
void *host_find_node(void *s, const char *id) { (void)s; (void)id; return 0; }
void host_node_set_transform(void *n, const int32_t *m) { (void)n; (void)m; }
void host_node_set_visible(void *n, int on) { (void)n; (void)on; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; }
static int nlines;
void *host_raster(void) { return &nlines; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c) { (void)r; (void)a; (void)b; (void)w; (void)c; nlines++; }

static const char *NAMES[DV_NMODES] = { "tube", "soft", "hard", "diode1", "diode2", "linfold", "sinfold",
                                         "zerosq", "downs", "asym", "rect", "xshp", "xshpa" };
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void dsp_sweep(void)
{
    static const int32_t V[] = { 0, 1, 1000, 8192, 16384, 24576, 32000, 32767 };
    int nv = sizeof V / sizeof V[0];
    long blocks = 0;
    unsigned seed = 1;
    int16_t buf[64];
    for (int mode = 0; mode < DV_NMODES; mode++)
    for (int di = 0; di < nv; di++)
    for (int rt = 0; rt < 9; rt++)
    for (int ri = 0; ri < nv; ri += 3)
    for (int fi = 0; fi < nv; fi += 2) {
        DvParams P;
        DvState S = { 0 };
        dv_decode(&P, V[di], mode * 32768 / DV_NMODES + 100, V[fi], 32767, rt * 32768 / 9 + 100, V[ri], 32767);
        CHECK(P.mode == mode && P.route == rt / 3 && P.ftype == rt % 3, "decode mode %d rt %d", mode, rt);
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
                dv_block(&S, &P, buf, 64, 1);
                blocks++;
            }
            CHECK(!(abs(S.lp) >= 65535 && abs(S.bp) >= 65535), "filter stuck: mode %s drive %d rt %d res %d f %d sig %d",
                  NAMES[mode], V[di], rt, V[ri], V[fi], sig);
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
                dv_block(&S, &P, buf, 64, 1);
                if (b >= 200) for (int i = 0; i < 64; i++) { ss += (double)buf[i] * buf[i]; sum += buf[i]; cnt++; }
            }
            printf("  d%3d%%: rms %5.1f dBFS dc %6.3f", d * 50, 10 * log10(ss / cnt / (32768.0 * 32768)), sum / cnt / 32768);
        }
        printf("\n");
    }
}

static void callbacks(void)
{
    int16_t *w = host_fx_knobs[0];
    static const int16_t def[8] = { 4403, 945, 11264, 32700, 0x5304, 1820, 6553, 26214 };
    DvParams P;
    for (int i = 0; i < 8; i++) w[i] = def[i];
    op1_fx_page[0][0] = op1_fx_page[0][1] = op1_fx_page[0][2] = op1_fx_page[0][3] = 0;

    for (int k = 0; k < 4; k++)                                     /* page 1: only green is ours */
        CHECK(FX_drive.knob(0, k, 1024, w) == (k == 1), "page 1 knob %d handled", k);
    params_of(&P, w);
    CHECK(P.mode == 1, "green +1 detent: mode %d", P.mode);
    FX_drive.knob(0, 1, 3 * 1024, w); params_of(&P, w);
    CHECK(P.mode == 4, "green +3 detents: mode %d", P.mode);
    FX_drive.knob(0, 1, 40 * 1024, w); params_of(&P, w);
    CHECK(P.mode == DV_NMODES - 1, "mode clamps at the last: %d", P.mode);
    FX_drive.knob(0, 1, -100 * 1024, w); params_of(&P, w);
    CHECK(P.mode == 0, "mode clamps at the first: %d", P.mode);
    CHECK(w[0] == def[0] && w[2] == def[2] && w[3] == def[3] && w[4] == 0x5304, "only the mode word changed");

    CHECK(FX_drive.key(0, 2, 1) == 1 && FX_drive.key(0, 2, 0) == 1, "press and release handled");
    CHECK(op1_fx_page[0][0] && op1_fx_page[0][1] && op1_fx_page[0][2] && op1_fx_page[0][3], "page 2 on for all");
    CHECK(!op1_fx_page[1][0] && !op1_fx_page[2][0], "page 2 is per FX position");
    for (int k = 0; k < 4; k++) CHECK(FX_drive.knob(0, k, 0, w) == 1, "page 2 knob %d handled", k);
    FX_drive.knob(0, 0, 1024, w); params_of(&P, w);
    CHECK(P.route == R_PRE && P.ftype == F_LP, "blue: route PRE (%d %d)", P.route, P.ftype);
    FX_drive.knob(0, 1, 2048, w); params_of(&P, w);
    CHECK(P.route == R_PRE && P.ftype == F_HP, "green: type HP (%d %d)", P.route, P.ftype);
    FX_drive.knob(0, 0, 5 * 1024, w); params_of(&P, w);
    CHECK(P.route == R_POST && P.ftype == F_HP, "route clamps at POST (%d %d)", P.route, P.ftype);
    FX_drive.knob(0, 2, 40 * 1024, w);
    CHECK(w[P_RES] == 32767, "resonance clamps high: %d", w[P_RES]);
    FX_drive.knob(0, 3, -40 * 1024, w);
    CHECK(w[P_LEVEL] == 0, "level clamps low: %d", w[P_LEVEL]);
    FX_drive.key(0, 0, 1);
    CHECK(!op1_fx_page[0][0] && !op1_fx_page[0][3], "page 1 again");

    /* master: both channels processed, each with its own state; init(2) resets both */
    int16_t l[64], r[64];
    for (int i = 0; i < 8; i++) w[i] = def[i];
    w[P_MIX] = 32767;
    FX_drive.init(OP1_FX_MASTER);
    for (int i = 0; i < 64; i++) { l[i] = (int16_t)(20000 * sin(i * 0.2)); r[i] = 0; }
    FX_drive.render_stereo(l, r, w);
    int nzl = 0, maxr = 0;                                          /* (tube's bias leaves a tiny DC step) */
    for (int i = 0; i < 64; i++) { nzl += l[i] != 0; if (abs(r[i]) > maxr) maxr = abs(r[i]); }
    CHECK(nzl > 50 && maxr < 64, "stereo: left processed, silent right stays near silent (%d %d)", nzl, maxr);
    CHECK(st[2].peak > 0 && st[3].peak == 0, "stereo peaks %d %d", st[2].peak, st[3].peak);
    FX_drive.init(OP1_FX_MASTER);
    CHECK(!st[2].inited && !st[3].inited, "init(2) resets both master states");

    nlines = 0;
    SCREEN_drive.update(0, 16);
    CHECK(nlines > 300, "screen draws (%d lines)", nlines);
}

int main(int argc, char **argv)
{
    dsp_sweep();
    callbacks();
    if (argc > 1) {                                                 /* transfer curves at drive 0 / 50 / 100 % */
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
