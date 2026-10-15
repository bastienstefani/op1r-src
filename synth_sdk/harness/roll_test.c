/* Host test of the roll piano-roll sequencer (seq/roll.c).
 *   cc -O1 -Wall -Wextra -fsanitize=address,undefined -DOP1_HOST -I . harness/roll_test.c -lm -o roll_test && ./roll_test
 * checks: armed start on the next beat only, step input / removal / clear, playback timing and gates, polyphony
 * limit with nothing hanging, loop wrap, hold, reset / stop re-arms (from the top at the next beat), MIDI START,
 * encoder remainders, clamps, drum mode does nothing, save / load round trip, rows without the format field and bad
 * rows. "render": JSON screen frames for harness/render_roll.py. */
#include "../seq/roll.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24];
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4];

static int sounding[128], n_on, max_poly, last_vel;
static long now, on_at[1024]; static int on_note[1024], n_log;
void host_rl_note_on(int n, int v)
{
    sounding[n]++; n_on++; last_vel = v;
    int a = 0; for (int i = 0; i < 128; i++) a += sounding[i];
    if (a > max_poly) max_poly = a;
    if (n_log < 1024) { on_note[n_log] = n; on_at[n_log++] = now; }
}
void host_rl_note_off(int n) { sounding[n]--; }

#define MAXL 4096
static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } lines[MAXL]; static int nlines;
void *host_find_node(void *s, const char *id) { (void)s; (void)id; return 0; }
void host_node_set_transform(void *n, const int32_t *m) { (void)n; (void)m; }
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

/* player ticks; a beat every 96 ticks, the first one `phase` ticks from now */
static int phase;
static void ticks(int n) { for (int i = 0; i < n; i++, now++) { rl_tick(phase == 0); phase = phase ? phase - 1 : 95; } }
static int poly(void) { int a = 0; for (int i = 0; i < 128; i++) a += sounding[i]; return a; }
static int nnotes(void) { int n = 0; for (int i = 0; i < RL_MAXNOTES; i++) n += rl.note[i].used; return n; }
static void fresh(void) { memset(&rl, 0, sizeof rl); rl_bank_init(&rl); memset(sounding, 0, sizeof sounding); n_on = n_log = 0; }

static void test_edit_play(void)
{
    rl_bank_t *b = &rl;
    fresh();
    CHECK(b->loop_len == 384 && b->cur_len == 24 && rl_grid(b) == 24 && !b->run, "defaults: one bar of 16 steps, held");
    /* step input: C E G on steps 0, 1, 2 */
    CHECK(rl_key(0, 60, 0) == 0 && b->cur_t == 24, "a key places a note, sounds, moves the cursor");
    rl_key(0, 64, 0); rl_key(0, 67, 0);
    rl_turn(0, 0, -3 * 1024);
    CHECK(b->cur_t == 0, "cursor back to 0: %d", b->cur_t);
    rl_turn(0, 2, 512); rl_turn(0, 2, 512);             /* two half detents: one step */
    CHECK(b->cur_len == 48, "encoder remainders add up: %d", b->cur_len);
    rl_turn(0, 2, 2 * 1024);
    CHECK(b->cur_len == 96, "length 4 steps: %d", b->cur_len);
    rl_press(0, 2);                                     /* velocity 64 */
    rl_key(0, 48, 0);
    CHECK(b->cur_t == 96 && nnotes() == 4, "cursor after a long note: %d, %d notes", b->cur_t, nnotes());
    rl_turn(0, 0, -3 * 1024);                           /* step 1 */
    CHECK(rl_key(0, 60, 1) == 1 && nnotes() == 4, "Shift + key consumed, nothing of that pitch under the cursor");
    CHECK(rl_key(0, 64, 1) == 1 && nnotes() == 3 && rl_note_at(b, 64, 24) < 0, "Shift + key removes E at step 1");

    /* armed: nothing before the beat, then from the top */
    host_rl_changed = 0;
    rl_press(0, 3);
    CHECK(b->run && host_rl_changed == 0, "red press: play, not saved");
    phase = 40; long t0 = now;
    ticks(40);
    CHECK(n_on == 0 && !b->playing, "armed: waits for the beat");
    ticks(30);                                          /* beat at tick 40: C and C3 at once, G at step 2 */
    CHECK(n_on == 2 && on_at[0] == t0 + 40 && on_at[1] == t0 + 40, "start on the beat: %d notes, first at +%ld", n_on, n_log ? on_at[0] - t0 : -1L);
    CHECK(last_vel == 64 || last_vel == 100, "velocities");
    ticks(40);                                          /* t = 70: C off at 24, G on at 48 */
    CHECK(sounding[60] == 0 && sounding[67] == 1 && sounding[48] == 1, "gates: C %d G %d C3 %d", sounding[60], sounding[67], sounding[48]);
    ticks(384 - 70 + 1);                                /* wrap, then t = 0 again */
    CHECK(poly() == 2 && n_on == 5, "loop wraps, plays again: poly %d on %d", poly(), n_on);
    /* reset / stop: notes off, from the top at the next beat */
    rl_rearm();
    CHECK(poly() == 0 && !b->playing && b->run, "re-armed, nothing sounding");
    int before = n_on;
    phase = 10; ticks(10);
    CHECK(n_on == before, "re-armed: waits for the beat");
    ticks(1);
    CHECK(n_on == before + 2 && b->play_t == 1, "re-armed: from the top on the beat");
    /* MIDI START: from the top on the next tick */
    ticks(100);
    rl_midi_start();
    CHECK(poly() == 0 && b->play_t == 0 && b->playing, "MIDI START: notes off, top");
    before = n_on; phase = 50; ticks(1);
    CHECK(n_on == before + 2, "MIDI START: first notes on the next tick");
    rl_press(0, 3);                                     /* hold */
    ticks(1);
    CHECK(poly() == 0 && !b->playing, "hold releases everything");
    rl_midi_start();
    CHECK(!b->playing, "MIDI START leaves a held roll alone");

    /* polyphony: 20 notes at step 0: 16 play, the others are skipped; all released at the wrap */
    b->cur_t = 0; rl_press(0, 1);                       /* green press: clear step 0 */
    CHECK(rl_note_at(b, 60, 0) < 0 && rl_note_at(b, 48, 0) < 0 && rl_note_at(b, 48, 90) < 0, "clear the notes under the cursor");
    for (int p = 0; p < 20; p++) { b->cur_t = 0; rl_key(0, 40 + p, 0); }
    rl_press(0, 3); phase = 0; max_poly = 0; ticks(384 + 1);
    CHECK(max_poly == 16 && poly() <= 16, "20 notes: %d at once", max_poly);
    ticks(383);
    CHECK(poly() == 0, "nothing hangs after the wrap: %d", poly());
    rl_press(0, 3); ticks(1);

    /* loop length clamps, cursor and length follow */
    b->cur_t = 300;
    rl_turn(0, 3, -100 * 1024);
    CHECK(b->loop_len == 24 && b->cur_t == 0 && b->cur_len == 24, "loop clamps to one step: %d", b->loop_len);
    rl_turn(0, 3, 1000 * 1024);
    CHECK(b->loop_len == RL_MAXLOOP, "loop clamps to 16 bars: %d", b->loop_len);
    b->cur_t = 30;
    rl_press(0, 0);                                     /* grid 1/32, cursor snaps */
    CHECK(rl_grid(b) == 12 && b->cur_t == 24, "grid cycles to 1/32: %d, cursor %d", rl_grid(b), b->cur_t);
    rl_press(0, 0);
    CHECK(rl_grid(b) == 96 && b->cur_t == 0 && b->cur_len == 96, "grid 1/4: cursor %d, length %d", b->cur_t, b->cur_len);
    rl_turn(0, 1, 200 * 1024);
    CHECK(b->top == RL_ROWS - 1, "pitch view clamps low: %d", b->top);
    rl_key(0, 100, 0);
    CHECK(b->top == 102, "a note out of view scrolls to it: %d", b->top);

    /* drum mode: nothing */
    rl_bank_t copy = *b;
    host_rl_changed = 0;
    CHECK(rl_key(1, 60, 0) == 0 && rl_key(1, 60, 1) == 0, "drum keys not consumed");
    rl_press(1, 3); rl_press(1, 0); rl_turn(1, 3, 4096); rl_turn(1, 0, 4096);
    CHECK(!memcmp(&copy, b, sizeof copy) && host_rl_changed == 0, "drum mode changes nothing");
}

static void test_save(void)
{
    char buf[4000];
    fresh();
    rl_bank_t *b = &rl;
    for (int i = 0; i < 12; i++) rl_key(0, 50 + i * 3, 0);
    rl_press(0, 2); rl_turn(0, 3, 5 * 1024); rl_turn(0, 1, -3 * 1024);
    rl_bank_t saved = *b;
    int n = rl_serialize(0, buf, sizeof buf - 4); buf[n] = 0;
    CHECK(n > 20 && !strchr(buf, '%') && !strchr(buf, '\'') && buf[0] == '1' && buf[1] == ',', "row: %d chars", n);
    CHECK(rl_serialize(1, buf + 3000, 500) == 0 && buf[3000] == 0, "drum row empty");
    memset(&rl, 0, sizeof rl);
    CHECK(rl_parse(0, buf, n) == 1, "row taken");
    CHECK(b->loop_len == saved.loop_len && b->grid_i == saved.grid_i && b->vel_i == saved.vel_i &&
          b->top == saved.top && b->cur_len == saved.cur_len && !b->run, "settings round trip, held");
    int same = nnotes() == 12;
    for (int i = 0; i < 12 && same; i++) same = rl_note_at(b, 50 + i * 3, saved.note[i].start) >= 0;
    CHECK(same, "notes round trip (%d)", nnotes());
    CHECK(rl_parse(1, buf, n) == 0, "drum row ignored");
    /* a full pattern fits the statement buffer */
    for (int i = 0; i < RL_MAXNOTES; i++) b->note[i] = (rl_note_t){ 6000 + i, 6143, 127, 127, 1, 0 };
    b->loop_len = RL_MAXLOOP; b->cur_len = RL_MAXLOOP;
    n = rl_serialize(0, buf, 2800 - 50);
    int k = 0; for (int i = 0; i < n; i++) k += buf[i] == ',';
    CHECK(n < 2400 && k == 5 + 4 * RL_MAXNOTES, "full pattern: %d chars, %d fields", n, k + 1);
    memset(&rl, 0, sizeof rl);
    CHECK(rl_parse(0, buf, n) == 1 && nnotes() == RL_MAXNOTES, "full pattern back: %d notes", nnotes());
    n = rl_serialize(0, buf, 300);
    CHECK(n <= 300 - 22 + 20, "short buffer: stops in time (%d)", n);
    /* rows of earlier roll images (no format field), bad rows */
    fresh();
    static const char old[] = "384,2,1,72,24,0,24,60,100";
    CHECK(rl_parse(0, old, (int)strlen(old)) == 0 && nnotes() == 0, "a row without the format field is ignored");
    static const char bad1[] = "1,99,7,9,200,0,99,5,60,100,10,0,300,0,20,0,64,0,1,2";
    CHECK(rl_parse(0, bad1, (int)strlen(bad1)) == 1, "clamped row taken");
    CHECK(b->loop_len == 99 && b->grid_i == 3 && b->vel_i == 0 && b->top == 127 && b->cur_len == 12, "clamped settings %d %d %d %d %d",
          b->loop_len, b->grid_i, b->vel_i, b->top, b->cur_len);
    CHECK(nnotes() == 2 && b->note[0].start == 99 && b->note[0].len == 5 && b->note[1].start == 20 && b->note[1].len == 1 &&
          b->note[1].vel == 100, "bad notes skipped, length 0 -> 1, velocity 0 -> 100: %d", nnotes());
    static const char bad2[] = "1,384,2,1,72,24,0,24,6x,100";
    CHECK(rl_parse(0, bad2, (int)strlen(bad2)) == 1 && nnotes() == 0, "a bad field ends the notes");
    CHECK(rl_parse(0, "abc", 3) == 0 && rl_parse(0, "", 0) == 0 && rl_parse(0, "1,2,3", 5) == 0, "garbage / short rows ignored");
    CHECK(rl_parse(0, "1,99999,2,1,72,24", 17) == 0, "a field over 9999 rejects the row");
}

static void frame(int pos, const char *label)
{
    rl_screen_update(0, 16, pos);
    printf("{\"label\": \"%s\", \"nodes\": {}, \"lines\": [", label);
    for (int i = 0; i < nlines; i++)
        printf("%s[%d, %d, %d, %d, %d, [%d, %d, %d, %d]]", i ? ", " : "", lines[i].x0, lines[i].y0, lines[i].x1,
               lines[i].y1, lines[i].w, lines[i].c[0], lines[i].c[1], lines[i].c[2], lines[i].c[3]);
    printf("]}\n");
}

static void render(void)
{
    rl_bank_t *b = &rl;
    fresh();
    frame(0, "empty, held");
    static const int mel[] = { 60, 64, 67, 72, 71, 67, 64, 62 };
    for (int i = 0; i < 8; i++) rl_key(0, mel[i], 0);
    rl_turn(0, 2, 1024); b->cur_t = 0;
    rl_key(0, 48, 0); rl_key(0, 55, 0); rl_key(0, 52, 0); rl_key(0, 55, 0);
    b->cur_t = 4 * 24; b->top = 74;
    frame(0, "step input, cursor on step 5");
    rl_press(0, 3); phase = 0; ticks(150);
    frame(0, "playing (red press)");
    rl_turn(0, 3, -4 * 1024); b->cur_t = 0;
    frame(0, "loop 12 steps");
    rl_turn(0, 3, 4 * 1024); rl_press(0, 0); rl_turn(0, 3, 16 * 1024);
    frame(0, "grid 1/32, loop 48 steps");
    b->cur_t = 0; rl_turn(0, 2, 10 * 1024); rl_press(0, 2); rl_key(0, 76, 0);
    frame(0, "long note, length 7/16, velocity 64");
    frame(1, "drum mode");
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "render")) { render(); return 0; }
    test_edit_play();
    test_save();
    fresh(); rl_screen_update(0, 16, 0);
    CHECK(nlines > 20, "screen draws (%d lines)", nlines);
    printf(fails ? "roll: %d FAILED\n" : "roll: all ok\n", fails);
    return fails != 0;
}
