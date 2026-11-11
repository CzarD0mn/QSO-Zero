/* NeonQRP - Linux framebuffer output with LCD/HDMI auto-selection. */
#ifndef NEONQRP_FB_H
#define NEONQRP_FB_H

#include <stddef.h>
#include "gfx.h"

typedef struct {
    int fd;
    int w, h, bpp, stride;
    int rotate;          /* 1 if the panel is portrait and we draw landscape */
    uint8_t *mem;
    size_t len;
    char path[64];
    int is_hdmi;
} fb_t;

/* "auto": HDMI if a monitor is connected, else the small LCD. */
int  fb_open(fb_t *fb, const char *want);
void fb_close(fb_t *fb);
/* Copy a surface to the screen, integer-scaled and centred. */
void fb_present(fb_t *fb, const surface_t *s);
/* 1 if an HDMI monitor is connected (DRM connector status). */
int  fb_hdmi_connected(void);

#endif
