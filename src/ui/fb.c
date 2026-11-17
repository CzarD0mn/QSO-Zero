#include "fb.h"
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int fb_hdmi_connected(void)
{
    DIR *d = opendir("/sys/class/drm");
    if (!d) return 0;
    struct dirent *e;
    int on = 0;
    while ((e = readdir(d))) {
        if (!strstr(e->d_name, "HDMI")) continue;
        char p[300], st[32] = "";
        snprintf(p, sizeof p, "/sys/class/drm/%s/status", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(st, sizeof st, f) && !strncmp(st, "connected", 9)) on = 1;
        fclose(f);
    }
    closedir(d);
    return on;
}

static int fb_try(fb_t *fb, const char *path)
{
    memset(fb, 0, sizeof *fb);
    fb->fd = open(path, O_RDWR);
    if (fb->fd < 0) return -1;
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &v) || ioctl(fb->fd, FBIOGET_FSCREENINFO, &f)) { close(fb->fd); return -1; }
    fb->w = v.xres;
    fb->h = v.yres;
    fb->bpp = v.bits_per_pixel;
    fb->stride = f.line_length;
    fb->len = (size_t)f.line_length * v.yres_virtual;
    fb->mem = mmap(NULL, fb->len, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (fb->mem == MAP_FAILED) { close(fb->fd); return -1; }
    snprintf(fb->path, sizeof fb->path, "%s", path);
    fb->rotate = fb->h > fb->w;          /* a 170x320 panel is used landscape */
    return 0;
}

int fb_open(fb_t *fb, const char *want)
{
    if (want && strcmp(want, "auto")) return fb_try(fb, want);
    /* Started from the CardputerZero launcher: it names the LCD framebuffer.
     * An HDMI monitor, if connected, still takes priority (large layout). */
    const char *launcher_fb = getenv("APPLAUNCH_LINUX_FBDEV_DEVICE");
    if (launcher_fb && *launcher_fb && !fb_hdmi_connected() && fb_try(fb, launcher_fb) == 0) return 0;
    /* auto: classify every framebuffer by size; small = the LCD */
    int lcd = -1, big = -1;
    for (int i = 0; i < 4; ++i) {
        char p[32];
        snprintf(p, sizeof p, "/dev/fb%d", i);
        fb_t t;
        if (fb_try(&t, p)) continue;
        int small = (t.w <= 480 && t.h <= 480);
        if (small && lcd < 0) lcd = i;
        if (!small && big < 0) big = i;
        fb_close(&t);
    }
    int pick = (fb_hdmi_connected() && big >= 0) ? big : (lcd >= 0 ? lcd : big);
    if (pick < 0) return -1;
    char p[32];
    snprintf(p, sizeof p, "/dev/fb%d", pick);
    int rc = fb_try(fb, p);
    if (!rc) fb->is_hdmi = (pick == big);
    return rc;
}

void fb_close(fb_t *fb)
{
    if (fb->mem && fb->mem != MAP_FAILED) munmap(fb->mem, fb->len);
    if (fb->fd > 0) close(fb->fd);
    fb->mem = NULL;
    fb->fd = -1;
}

static inline uint32_t to888(uint16_t c)
{
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

void fb_present(fb_t *fb, const surface_t *s)
{
    int dw = fb->rotate ? fb->h : fb->w, dh = fb->rotate ? fb->w : fb->h;
    int sc = dw / s->w < dh / s->h ? dw / s->w : dh / s->h;
    if (sc < 1) sc = 1;
    int ox = (dw - s->w * sc) / 2, oy = (dh - s->h * sc) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    if (!fb->rotate) {
        for (int y = 0; y < s->h && oy + y * sc < fb->h; ++y) {
            const uint16_t *src = s->px + (size_t)y * s->w;
            for (int r = 0; r < sc; ++r) {
                uint8_t *row = fb->mem + (size_t)(oy + y * sc + r) * fb->stride;
                if (fb->bpp == 16) {
                    uint16_t *d = (uint16_t *)row + ox;
                    if (sc == 1) memcpy(d, src, sizeof(uint16_t) * s->w);
                    else for (int x = 0; x < s->w; ++x) for (int k = 0; k < sc; ++k) *d++ = src[x];
                } else {
                    uint32_t *d = (uint32_t *)row + ox;
                    for (int x = 0; x < s->w; ++x) { uint32_t c = to888(src[x]); for (int k = 0; k < sc; ++k) *d++ = c; }
                }
            }
        }
    } else {
        /* panel is portrait: rotate 90 degrees clockwise */
        for (int y = 0; y < s->h; ++y)
            for (int x = 0; x < s->w; ++x) {
                int fx = fb->w - 1 - (oy + y), fy = ox + x;
                if (fx < 0 || fy >= fb->h) continue;
                uint8_t *p = fb->mem + (size_t)fy * fb->stride;
                uint16_t c = s->px[(size_t)y * s->w + x];
                if (fb->bpp == 16) ((uint16_t *)p)[fx] = c;
                else ((uint32_t *)p)[fx] = to888(c);
            }
    }
}
