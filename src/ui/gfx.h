/* QSO-Zero - tiny software renderer on an RGB565 surface. */
#ifndef QSOZERO_GFX_H
#define QSOZERO_GFX_H

#include <stdint.h>

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

typedef struct { int w, h; uint16_t *px; } surface_t;

#define FONT_W 6   /* 5 px glyph + 1 px gap */
#define FONT_H 9   /* 7 px glyph + descender row + 1 px gap */

surface_t *surf_new(int w, int h);
void surf_free(surface_t *s);
void gfx_clear(surface_t *s, uint16_t c);
void gfx_fill(surface_t *s, int x, int y, int w, int h, uint16_t c);
void gfx_rect(surface_t *s, int x, int y, int w, int h, uint16_t c);
void gfx_hline(surface_t *s, int x, int y, int w, uint16_t c);
void gfx_vline(surface_t *s, int x, int y, int h, uint16_t c);
void gfx_pixel(surface_t *s, int x, int y, uint16_t c);
uint16_t gfx_lerp(uint16_t a, uint16_t b, int t255);
void gfx_hgrad(surface_t *s, int x, int y, int w, int h, uint16_t a, uint16_t b);
/* draws text, returns x after the last glyph; scale >= 1 */
int  gfx_text(surface_t *s, int x, int y, const char *t, uint16_t c, int scale);
int  gfx_text_n(surface_t *s, int x, int y, const char *t, int maxch, uint16_t c, int scale);
int  gfx_text_w(const char *t, int scale);
/* a filled pill/chip with text */
int  gfx_chip(surface_t *s, int x, int y, const char *t, uint16_t bg, uint16_t fg, int scale);

#endif
