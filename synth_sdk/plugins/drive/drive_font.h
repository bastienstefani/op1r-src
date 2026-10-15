/* drive: the screen. Serum's display (transfer curve, the shaped wave) drawn in the OP-1's own language: black
 * background, thin lines, one colour per encoder (blue, green, white, orange). Lines only, through op1_raster_line
 * (the firmware's anti-aliased line; recorded by the host harness), so the same code runs on a PC. Screen 320 x 160,
 * coordinates 22.10 fixed point. Stroke font, colours and line helper.
 * Copyright (C) 2026 bastienstefani. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#include <stdint.h>

#define PX(v) ((int32_t) (v) * 1024)

static const uint8_t C_BLUE  [4] = { 0x4d, 0x9e, 0xff, 0xff };
static const uint8_t C_GREEN [4] = { 0x00, 0xed, 0x95, 0xff };
static const uint8_t C_WHITE [4] = { 0xff, 0xff, 0xff, 0xff };
static const uint8_t C_ORANGE[4] = { 0xff, 0x7a, 0x1a, 0xff };
static const uint8_t C_GRID  [4] = { 0x3a, 0x3c, 0x40, 0xff };
static const uint8_t C_DIM   [4] = { 0x80, 0x82, 0x88, 0xff };
static const uint8_t C_GLOW  [4] = { 0x16, 0x34, 0x66, 0xff };     /* the wide line under the curve */

static void seg(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t c[4]) {
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}

/* ---- stroke font: glyphs on a 4 x 6 grid, "xy" digit pairs, '.' lifts the pen ---- */
static const char *glyph(char ch) {
    switch (ch) {
    case 'A': return "06 01 10 30 41 46 . 03 43";
    case 'B': return "06 00 30 41 42 33 03 . 33 44 45 36 06";
    case 'C': return "41 30 10 01 05 16 36 45";
    case 'D': return "00 06 36 45 41 30 00";
    case 'E': return "40 00 06 46 . 03 33";
    case 'F': return "40 00 06 . 03 33";
    case 'G': return "41 30 10 01 05 16 36 45 43 23";
    case 'H': return "00 06 . 40 46 . 03 43";
    case 'I': return "10 30 . 20 26 . 16 36";
    case 'J': return "40 45 36 16 05";
    case 'K': return "00 06 . 40 03 46";
    case 'L': return "00 06 46";
    case 'M': return "06 00 23 40 46";
    case 'N': return "06 00 46 40";
    case 'O': case '0': return "10 30 41 45 36 16 05 01 10";
    case 'P': return "06 00 30 41 42 33 03";
    case 'Q': return "10 30 41 45 36 16 05 01 10 . 34 46";
    case 'R': return "06 00 30 41 42 33 03 . 23 46";
    case 'S': return "41 30 10 01 02 13 33 44 45 36 16 05";
    case 'T': return "00 40 . 20 26";
    case 'U': return "00 05 16 36 45 40";
    case 'V': return "00 26 40";
    case 'W': return "00 16 23 36 40";
    case 'X': return "00 46 . 40 06";
    case 'Y': return "00 23 40 . 23 26";
    case 'Z': return "00 40 06 46";
    case '1': return "11 20 26 . 16 36";
    case '2': return "01 10 30 41 42 06 46";
    case '3': return "00 40 23 33 44 45 36 16 05";
    case '4': return "30 04 44 . 32 36";
    case '5': return "40 00 02 32 43 45 36 06";
    case '6': return "30 10 01 05 16 36 45 43 32 12 03";
    case '7': return "00 40 41 16";
    case '8': return "10 30 41 42 33 13 04 05 16 36 45 44 33 . 13 02 01 10";
    case '9': return "16 36 45 41 30 10 01 03 14 34 43";
    case '%': return "06 40 . 00 01 . 45 46";
    case '-': return "03 33";
    case '+': return "03 43 . 21 25";
    case '.': return "16 26";
    case '/': return "06 40";
    default:  return "";
    }
}

/* text at (x, y) = top-left in px, unit = px per grid step in 22.10 (1.5 px -> 1536), returns the end x */
static int32_t text(void *r, int32_t x, int32_t y, const char *s, int32_t unit, int32_t w, const uint8_t c[4]) {
    for (; *s; s++) {
        const char *g = glyph(*s);
        int pen = 0;
        int32_t px = 0, py = 0;
        while (*g) {
            if (*g == ' ') { g++; continue; }
            if (*g == '.') { pen = 0; g++; continue; }
            {
                int32_t nx = x + (g[0] - '0') * unit, ny = y + (g[1] - '0') * unit;
                if (pen) seg(r, px, py, nx, ny, w, c);
                px = nx; py = ny; pen = 1;
                g += 2;
            }
        }
        x += (*s == ' ' || *s == '.' ? 4 : 6) * unit;
    }
    return x;
}
static int32_t text_width(const char *s, int32_t unit) {
    int32_t x = 0;
    for (; *s; s++) x += (*s == ' ' || *s == '.' ? 4 : 6) * unit;
    return x - 2 * unit;
}

static char *put_dec(char *p, int32_t v) {             /* v >= 0, up to 5 digits */
    char t[6];
    int i = 0;
    if (v < 0) { *p++ = '-'; v = -v; }
    do { t[i++] = (char) ('0' + v % 10); v /= 10; } while (v && i < 5);
    while (i) *p++ = t[--i];
    *p = 0;
    return p;
}
static char *put_s(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
