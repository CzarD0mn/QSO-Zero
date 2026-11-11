/* QSO-Zero - "retro synth" palette: saturated neon on deep indigo, no glow. */
#ifndef QSOZERO_THEME_H
#define QSOZERO_THEME_H

#include "gfx.h"

#define C_BG       RGB(0x0d, 0x02, 0x21)   /* midnight indigo */
#define C_PANEL    RGB(0x1a, 0x0b, 0x3d)
#define C_PANEL2   RGB(0x24, 0x10, 0x52)
#define C_GRID     RGB(0x3a, 0x1c, 0x7a)   /* horizon grid purple */
#define C_MAGENTA  RGB(0xff, 0x2a, 0x6d)
#define C_PINK     RGB(0xff, 0x71, 0xce)
#define C_CYAN     RGB(0x05, 0xd9, 0xe8)
#define C_BLUE     RGB(0x01, 0x9c, 0xfe)
#define C_PURPLE   RGB(0xb9, 0x67, 0xff)
#define C_SUN      RGB(0xf9, 0xc8, 0x0e)   /* sunset yellow */
#define C_ORANGE   RGB(0xff, 0x8c, 0x42)
#define C_MINT     RGB(0x05, 0xff, 0xa1)
#define C_RED      RGB(0xff, 0x38, 0x64)
#define C_TEXT     RGB(0xe8, 0xe4, 0xff)
#define C_DIM      RGB(0x8a, 0x7a, 0xc0)
#define C_FAINT    RGB(0x55, 0x48, 0x88)
#define C_BLACK    RGB(0x00, 0x00, 0x00)

/* 256-entry waterfall map: indigo -> purple -> magenta -> orange -> sun -> white */
const uint16_t *theme_wf_palette(void);

#endif
