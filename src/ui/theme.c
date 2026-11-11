#include "theme.h"

const uint16_t *theme_wf_palette(void)
{
    static uint16_t pal[256];
    static int ready;
    if (ready) return pal;
    struct { int at; uint16_t c; } stops[] = {
        {0, C_BG}, {70, RGB(0x2a, 0x0a, 0x5e)}, {120, RGB(0x7a, 0x12, 0xa8)},
        {165, C_MAGENTA}, {205, C_ORANGE}, {235, C_SUN}, {255, RGB(0xff, 0xff, 0xf0)},
    };
    int ns = (int)(sizeof stops / sizeof *stops);
    for (int i = 0; i < 256; ++i) {
        int k = 0;
        while (k < ns - 2 && i > stops[k + 1].at) ++k;
        int span = stops[k + 1].at - stops[k].at;
        int t = span ? (i - stops[k].at) * 255 / span : 0;
        if (t < 0) t = 0;
        if (t > 255) t = 255;
        pal[i] = gfx_lerp(stops[k].c, stops[k + 1].c, t);
    }
    ready = 1;
    return pal;
}
