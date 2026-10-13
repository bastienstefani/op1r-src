/* Host test of the roll piano-roll sequencer (seq/roll.c).
 *   cc -O1 -Wall -Wextra -fsanitize=address,undefined -DOP1_HOST -I.. roll_test.c -lm -o build/roll_test && build/roll_test
 * checks: beat-aligned start against a simulated stock clock (counter modulo 48, resets at random times, also between
 * the read and the increment), step input / removal / clear, playback timing and gates, polyphony, loop wrap and
 * hold release, both modes independent, save / load round trip and bad rows. "render": JSON screen frames for
 * harness/render_roll.py. */
#include "../seq/roll.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6];
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3]; volatile uint8_t op1_fx_page[3][4];

static int sounding[2][128], n_on[2], n_off[2], max_poly, last_vel;
static long now, on_at[256]; static int n_log;
void host_rl_note_on(int pos, int n, int v)
{
    sounding[pos][n]++; n_on[pos]++; last_vel = v;
    int a = 0; for (int i = 0; i < 128; i++) a += sounding[pos][i];
    if (a > max_poly) max_poly = a;
    if (n_log < 256) on_at[n_log++] = now;
}
void host_rl_note_off(int pos, int n) { sounding[pos][n]--; n_off[pos]++; }

#define MAXL 4096
static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } lines[MAXL]; static int nlines;
void *host_find_node(void *s, const char *id) { (void)s; (void)id; return 0; }
void host_node_set_transform(void *n, const int32_t *m) { (void)n; (void)m; }
void host_node_set_visible(void *n, int on) { (void)n; (void)on; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; nlines = 0; }
void *host_raster(void) { return &nlines; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c)
{
    (void)r; const op1_pt *p = a, *q = b;
    if (nlines < MAXL) { lines[nlines].x0 = p->x; lines[nlines].y0 = p->y; lines[nlines].x1 = q->x; lines[nlines].y1 = q->y;
                         lines[nlines].w = w; memcpy(lines[nlines].c, c, 4); nlines++; }
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* the stock clock: the player tick reads a counter modulo 48, then the counter advances */
static int c48;
static void ticks(int pos, int n) { for (int i = 0; i < n; i++, now++) { rl_tick(pos, c48); c48 = (c48 + 1) % 48; } }
static void reset_clock(void) { c48 = 0; rl_reset_pending = 1; }
static int poly(int pos) { int a = 0; for (int i = 0; i < 128; i++) a += sounding[pos][i]; return a; }

static void test_beat(void)
{
    long since = 0, at = 0, maxw = 0;
    int armed = 0, rw = 0, bad = 0, starts = 0, resets = 0, races = 0;
    srand(7);
    rl_reset_pending = 0; rl_last48 = 0xff; c48 = 0;
    for (long tick = 0; tick < 5000000; tick++) {
        int r = rand() % 4000;
        if (r == 0) { c48 = 0; since = 0; rl_reset_pending = 1; resets++; rw = 1; }      /* reset before the read */
        if (!armed && rand() % 600 == 0) { armed = 1; at = tick; rw = 0; }             /* red press */
        if (rl_on_beat(c48) && armed) {
            starts++;
            if (since % 96) bad++;
            if (!rw && tick - at > maxw) maxw = tick - at;
            armed = 0;
        }
        if (r == 1) { c48 = 1; since = 1; rl_reset_pending = 1; resets++; races++; rw = 1; }   /* between read and step */
        else { c48 = (c48 + 1) % 48; since++; }
    }
    printf("beat: resets=%d (races=%d) starts=%d off-beat=%d max wait=%ld\n", resets, races, starts, bad, maxw);
    CHECK(!bad && maxw <= 95 && starts > 1000, "beat-aligned start");
}

static void test_edit_play(void)
{
    rl_bank_t *b = &rl[0];
    rl_bank_init(&rl[0]); rl_bank_init(&rl[1]);
    CHECK(b->loop_len == 384 && b->cur_len == 24 && rl_grid(b) == 24, "defaults: one bar of 16 steps");
    /* step input: C E G on steps 0, 1, 2; then a chord note on step 0 */
    CHECK(rl_key(0, 60, 0) == 0 && b->cur_t == 24, "key places a note and advances the cursor");
    rl_key(0, 64, 0); rl_key(0, 67, 0);
    rl_turn(0, 0, -3 * 1024);                                       /* blue: back to step 0 */
    CHECK(b->cur_t == 0, "cursor back to 0: %d", b->cur_t);
    rl_turn(0, 2, 3 * 1024);                                        /* white: new notes 4 steps long */
    CHECK(b->cur_len == 96, "length 4 steps: %d", b->cur_len);
    rl_press(0, 2);                                                 /* white press: velocity 64 */
    rl_key(0, 48, 0);
    CHECK(b->cur_t == 96, "cursor after a long note: %d", b->cur_t);
    int n = 0; for (int i = 0; i < RL_MAXNOTES; i++) n += b->note[i].used;
    CHECK(n == 4, "4 notes: %d", n);
    rl_turn(0, 0, -4 * 1024);
    CHECK(rl_key(0, 64, 1) == 1, "Shift + key consumed");
    rl_turn(0, 0, 1024);
    CHECK(rl_key(0, 64, 1) == 1 && rl_note_at(b, 64, 24) < 0, "Shift + key removes E at step 1");

    /* playback: armed, starts on the next beat; C and G (and the long C3) on time, gates as set */
    host_rl_changed = 0;
    rl_press(0, 3);
    CHECK(b->run && host_rl_changed == 0, "red press: play, not saved");
    reset_clock(); ticks(0, 30);                                    /* clock reset: beat at counter 0 */
    CHECK(n_on[0] == 2 && on_at[0] == 0, "start on the beat: %d notes, first at %ld", n_on[0], n_log ? on_at[0] : -1L);
    CHECK(last_vel == 64 || last_vel == 100, "velocities");
    ticks(0, 40);                                                   /* t = 70: C off at 24, G on at 48 */
    CHECK(sounding[0][60] == 0 && sounding[0][67] == 1 && sounding[0][48] == 1, "gates: C %d G %d C3 %d",
          sounding[0][60], sounding[0][67], sounding[0][48]);
    ticks(0, 384 - 70 + 1);                                         /* wrap, then t = 0 again */
    CHECK(poly(0) == 2 && n_on[0] == 5, "loop wraps, plays again: poly %d on %d", poly(0), n_on[0]);
    rl_press(0, 3);                                                 /* hold */
    ticks(0, 1);
    CHECK(poly(0) == 0 && !b->playing, "hold releases everything");
    CHECK(n_on[1] == 0, "drum bank untouched");

    /* polyphony: 20 notes at step 0: 16 play (gate slots), the others are skipped; all released at the wrap */
    rl_press(0, 1);                                                 /* green press: clear the cursor column (t 24) */
    b->cur_t = 0; rl_press(0, 1);
    for (int p = 0; p < 20; p++) { b->cur_t = 0; rl_key(0, 40 + p, 0); }
    rl_press(0, 3); reset_clock(); max_poly = 0; ticks(0, 384);
    CHECK(max_poly == 16 && poly(0) == 0, "20 notes: %d played at once, %d left after the wrap", max_poly, poly(0));
    rl_stop_all();
    CHECK(!rl[0].run && !rl[1].run, "stop holds both modes");
    for (int i = 0; i < 128; i++) sounding[0][i] = 0;

    /* loop length clamps, cursor follows */
    rl_turn(0, 3, -100 * 1024);
    CHECK(b->loop_len == 24 && b->cur_t == 0, "loop clamps to one step: %d", b->loop_len);
    rl_turn(0, 3, 1000 * 1024);
    CHECK(b->loop_len == RL_MAXLOOP, "loop clamps to 16 bars: %d", b->loop_len);
    rl_press(0, 0);                                                 /* grid 1/32 */
    CHECK(rl_grid(b) == 12, "grid cycles: %d", rl_grid(b));
}

static void test_save(void)
{
    char buf[8192];
    rl_bank_t saved = rl[0];
    int n = rl_serialize(0, buf, sizeof buf - 1); buf[n] = 0;
    CHECK(n > 20 && !strchr(buf, '%') && !strchr(buf, '\''), "row: %d chars", n);
    memset(&rl[0], 0, sizeof rl[0]);
    rl_parse(0, buf);
    CHECK(rl[0].loop_len == saved.loop_len && rl[0].grid_i == saved.grid_i && rl[0].vel_i == saved.vel_i &&
          rl[0].view_top == saved.view_top && rl[0].cur_len == saved.cur_len, "settings round trip");
    int a = 0, c = 0;
    for (int i = 0; i < RL_MAXNOTES; i++) a += saved.note[i].used, c += rl[0].note[i].used;
    CHECK(a == c, "notes round trip %d %d", a, c);
    /* a full bank stays under the 8 KB statement */
    for (int i = 0; i < RL_MAXNOTES; i++) rl[1].note[i] = (rl_note_t){ 6000, 6143, 127, 127, 1, 0 };
    n = rl_serialize(1, buf, sizeof buf - 64);
    CHECK(n < 3000, "full bank row: %d chars", n);
    /* bad rows: garbage, out-of-range values, nothing */
    memset(&rl[1], 0, sizeof rl[1]);
    rl_parse(1, "");
    CHECK(rl[1].inited && rl[1].loop_len == 384, "empty row: defaults");
    rl_parse(1, "99999,7,9,200,0,99999,5,60,100,10,0,300,0,20,30,64,0");
    CHECK(rl[1].loop_len == RL_MAXLOOP && rl[1].grid_i == 3 && rl[1].vel_i == 0 && rl[1].view_top == 127 &&
          rl[1].cur_len == 12, "clamped settings");
    int used = 0; for (int i = 0; i < RL_MAXNOTES; i++) used += rl[1].note[i].used;
    CHECK(used == 1 && rl[1].note[0].start == 20 && rl[1].note[0].pitch == 64 && rl[1].note[0].vel == 100,
          "bad notes skipped, velocity 0 -> 100: %d", used);
    rl_parse(1, "abc");
    CHECK(rl[1].loop_len == RL_MAXLOOP, "garbage: unchanged");
}

static void frame(int pos, const char *label)
{
    host_screen_draw(0, 16);
    rl_screen_draw(host_raster(), pos);
    printf("{\"label\": \"%s\", \"nodes\": {}, \"lines\": [", label);
    for (int i = 0; i < nlines; i++)
        printf("%s[%d, %d, %d, %d, %d, [%d, %d, %d, %d]]", i ? ", " : "", lines[i].x0, lines[i].y0, lines[i].x1,
               lines[i].y1, lines[i].w, lines[i].c[0], lines[i].c[1], lines[i].c[2], lines[i].c[3]);
    printf("]}\n");
}

static void render(void)
{
    rl_bank_t *b = &rl[0];
    memset(rl, 0, sizeof rl);
    rl_bank_init(&rl[0]); rl_bank_init(&rl[1]);
    frame(0, "empty, held");
    static const int mel[] = { 60, 64, 67, 72, 71, 67, 64, 62 };
    for (int i = 0; i < 8; i++) rl_key(0, mel[i], 0);
    rl_turn(0, 2, 1024); b->cur_t = 0;
    rl_key(0, 48, 0); rl_key(0, 55, 0); rl_key(0, 52, 0); rl_key(0, 55, 0);
    b->cur_t = 4 * 24; b->view_top = 74;
    frame(0, "step input, cursor on step 5");
    rl_press(0, 3); reset_clock(); ticks(0, 150);
    frame(0, "playing (red press)");
    rl_press(0, 0); rl_turn(0, 3, 16 * 1024);
    frame(0, "grid 1/32, loop 48 steps");
    b->cur_t = 0; rl_turn(0, 2, 10 * 1024); rl_key(0, 76, 0);
    frame(0, "long note, length 7/16");
    rl_bank_init(&rl[1]);
    for (int i = 0; i < 16; i++) { rl[1].cur_t = (uint16_t)(i * 24); rl_key(1, 53 + (i % 4 == 0 ? 0 : i % 2 ? 2 : 4), 0); }
    rl[1].view_top = 60;
    frame(1, "drum mode: its own pattern");
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "render")) { render(); return 0; }
    test_beat();
    test_edit_play();
    test_save();
    printf(fails ? "roll: %d FAILED\n" : "roll: all ok\n", fails);
    return fails != 0;
}
