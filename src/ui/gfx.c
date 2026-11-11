#include "gfx.h"
#include <stdlib.h>
#include <string.h>

extern const uint8_t g_font5x7[95][5];

surface_t *surf_new(int w, int h)
{
    surface_t *s = malloc(sizeof *s);
    if (!s) return NULL;
    s->w = w;
    s->h = h;
    s->px = calloc((size_t)w * h, sizeof(uint16_t));
    if (!s->px) { free(s); return NULL; }
    return s;
}

void surf_free(surface_t *s)
{
    if (!s) return;
    free(s->px);
    free(s);
}

void gfx_clear(surface_t *s, uint16_t c)
{
    for (int i = 0; i < s->w * s->h; ++i) s->px[i] = c;
}

void gfx_fill(surface_t *s, int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0) return;
    for (int r = 0; r < h; ++r) {
        uint16_t *p = s->px + (size_t)(y + r) * s->w + x;
        for (int i = 0; i < w; ++i) p[i] = c;
    }
}

void gfx_pixel(surface_t *s, int x, int y, uint16_t c)
{
    if ((unsigned)x < (unsigned)s->w && (unsigned)y < (unsigned)s->h) s->px[(size_t)y * s->w + x] = c;
}

void gfx_hline(surface_t *s, int x, int y, int w, uint16_t c) { gfx_fill(s, x, y, w, 1, c); }
void gfx_vline(surface_t *s, int x, int y, int h, uint16_t c) { gfx_fill(s, x, y, 1, h, c); }

void gfx_rect(surface_t *s, int x, int y, int w, int h, uint16_t c)
{
    gfx_hline(s, x, y, w, c);
    gfx_hline(s, x, y + h - 1, w, c);
    gfx_vline(s, x, y, h, c);
    gfx_vline(s, x + w - 1, y, h, c);
}

uint16_t gfx_lerp(uint16_t a, uint16_t b, int t)
{
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
    int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    int r = ar + (br - ar) * t / 255, g = ag + (bg - ag) * t / 255, bl = ab + (bb - ab) * t / 255;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

void gfx_hgrad(surface_t *s, int x, int y, int w, int h, uint16_t a, uint16_t b)
{
    for (int i = 0; i < w; ++i) gfx_vline(s, x + i, y, h, gfx_lerp(a, b, w > 1 ? i * 255 / (w - 1) : 0));
}

static void glyph(surface_t *s, int x, int y, int ch, uint16_t c, int sc)
{
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t *g = g_font5x7[ch - 32];
    for (int col = 0; col < 5; ++col) {
        uint8_t bits = g[col];
        for (int row = 0; row < 8; ++row)
            if (bits & (1 << row)) {
                if (sc == 1) gfx_pixel(s, x + col, y + row, c);
                else gfx_fill(s, x + col * sc, y + row * sc, sc, sc, c);
            }
    }
}

int gfx_text_n(surface_t *s, int x, int y, const char *t, int maxch, uint16_t c, int sc)
{
    for (int i = 0; t[i] && i < maxch; ++i) {
        glyph(s, x, y, (unsigned char)t[i], c, sc);
        x += FONT_W * sc;
    }
    return x;
}

int gfx_text(surface_t *s, int x, int y, const char *t, uint16_t c, int sc)
{
    return gfx_text_n(s, x, y, t, 1 << 30, c, sc);
}

int gfx_text_w(const char *t, int sc) { return (int)strlen(t) * FONT_W * sc; }

int gfx_chip(surface_t *s, int x, int y, const char *t, uint16_t bg, uint16_t fg, int sc)
{
    int w = gfx_text_w(t, sc) + 3 * sc, h = 9 * sc;
    gfx_fill(s, x + sc, y, w - 2 * sc, h, bg);
    gfx_fill(s, x, y + sc, w, h - 2 * sc, bg);
    gfx_text(s, x + 2 * sc, y + sc, t, fg, sc);
    return x + w;
}
