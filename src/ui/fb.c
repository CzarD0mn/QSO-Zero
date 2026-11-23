#include "fb.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "core/log.h"

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

/* the kernel's name for a framebuffer, e.g. "vc4drmfb" (HDMI) or the LCD driver */
static void fb_name(const char *path, char *out, size_t n)
{
    out[0] = 0;
    char real[64] = "";
    ssize_t k = readlink(path, real, sizeof real - 1);      /* /dev/fb_lcd -> fb1 */
    const char *base = strrchr(k > 0 ? real : path, '/');
    base = base ? base + 1 : (k > 0 ? real : path);
    char p[128];
    snprintf(p, sizeof p, "/sys/class/graphics/%s/name", base);
    FILE *f = fopen(p, "r");
    if (f) { if (fgets(out, (int)n, f)) out[strcspn(out, "\n")] = 0; fclose(f); }
}

static int fb_try(fb_t *fb, const char *path)
{
    memset(fb, 0, sizeof *fb);
    fb->fd = open(path, O_RDWR);
    if (fb->fd < 0) {
        if (errno != ENOENT) qlog("display", "%s: cannot open: %s", path, strerror(errno));
        return -1;
    }
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &v) || ioctl(fb->fd, FBIOGET_FSCREENINFO, &f)) {
        qlog("display", "%s: not a usable framebuffer: %s", path, strerror(errno));
        close(fb->fd); fb->fd = -1; return -1;
    }
    char nm[40];
    fb_name(path, nm, sizeof nm);
    if (!v.xres || !v.yres || (v.bits_per_pixel != 16 && v.bits_per_pixel != 24 && v.bits_per_pixel != 32)) {
        qlog("display", "%s (%s): %ux%u at %u bpp - not supported", path, nm, v.xres, v.yres, v.bits_per_pixel);
        close(fb->fd); fb->fd = -1; return -1;
    }
    fb->w = v.xres;
    fb->h = v.yres;
    fb->bpp = v.bits_per_pixel;
    fb->stride = f.line_length;
    fb->len = (size_t)f.line_length * v.yres_virtual;
    fb->mem = mmap(NULL, fb->len, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (fb->mem == MAP_FAILED) {
        qlog("display", "%s (%s): cannot map %zu bytes: %s", path, nm, fb->len, strerror(errno));
        fb->mem = NULL; close(fb->fd); fb->fd = -1; return -1;
    }
    snprintf(fb->path, sizeof fb->path, "%s", path);
    fb->rotate = fb->h > fb->w;          /* a 170x320 panel is used landscape */
    return 0;
}

int fb_open(fb_t *fb, const char *want)
{
    if (want && strcmp(want, "auto")) return fb_try(fb, want);
    int hdmi = fb_hdmi_connected();
    /* Started from the CardputerZero launcher: it names the LCD framebuffer.
     * An HDMI monitor, if connected, still takes priority (large layout). */
    const char *launcher_fb = getenv("APPLAUNCH_LINUX_FBDEV_DEVICE");
    if (launcher_fb && *launcher_fb && !hdmi && fb_try(fb, launcher_fb) == 0) return 0;
    /* auto: classify every framebuffer by size; small = the LCD */
    int lcd = -1, big = -1;
    for (int i = 0; i < 4; ++i) {
        char p[32], nm[40];
        snprintf(p, sizeof p, "/dev/fb%d", i);
        fb_t t;
        if (fb_try(&t, p)) continue;
        fb_name(p, nm, sizeof nm);
        int small = (t.w <= 480 && t.h <= 480);
        if (hdmi) qlog("display", "found %s (%s): %dx%d at %d bpp -> %s", p, nm[0] ? nm : "?", t.w, t.h, t.bpp, small ? "LCD" : "large (HDMI)");
        if (small && lcd < 0) lcd = i;
        if (!small && big < 0) big = i;
        fb_close(&t);
    }
    if (hdmi && big < 0) qlog("display", "HDMI monitor connected, but no large framebuffer for it: using the LCD");
    int pick = (hdmi && big >= 0) ? big : (lcd >= 0 ? lcd : big);
    char p[32];
    if (pick >= 0) {
        snprintf(p, sizeof p, "/dev/fb%d", pick);
        if (fb_try(fb, p) == 0) { fb->is_hdmi = (pick == big); return 0; }
    }
    /* the HDMI framebuffer didn't work: fall back to the LCD rather than not starting */
    if (launcher_fb && *launcher_fb && fb_try(fb, launcher_fb) == 0) { qlog("display", "falling back to the LCD (%s)", launcher_fb); return 0; }
    if (lcd >= 0 && lcd != pick) {
        snprintf(p, sizeof p, "/dev/fb%d", lcd);
        if (fb_try(fb, p) == 0) { qlog("display", "falling back to the LCD (%s)", p); return 0; }
    }
    return -1;
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

static inline void put_px(const fb_t *fb, uint8_t *row, int x, uint16_t c)
{
    if (fb->bpp == 16) ((uint16_t *)row)[x] = c;
    else if (fb->bpp == 32) ((uint32_t *)row)[x] = to888(c);
    else { uint32_t v = to888(c); uint8_t *q = row + x * 3; q[0] = v & 255; q[1] = (v >> 8) & 255; q[2] = v >> 16; }
}

void fb_present(fb_t *fb, const surface_t *s)
{
    if (!fb->mem) return;
    int dw = fb->rotate ? fb->h : fb->w, dh = fb->rotate ? fb->w : fb->h;
    int sc = dw / s->w < dh / s->h ? dw / s->w : dh / s->h;
    if (sc < 1) sc = 1;
    int ox = (dw - s->w * sc) / 2, oy = (dh - s->h * sc) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    if (!fb->rotate) {
        for (int y = 0; y < s->h && oy + y * sc < fb->h; ++y) {
            const uint16_t *src = s->px + (size_t)y * s->w;
            for (int r = 0; r < sc && oy + y * sc + r < fb->h; ++r) {
                uint8_t *row = fb->mem + (size_t)(oy + y * sc + r) * fb->stride;
                if (fb->bpp == 16 && sc == 1 && ox + s->w <= fb->w) { memcpy((uint16_t *)row + ox, src, sizeof(uint16_t) * s->w); continue; }
                for (int x = 0; x < s->w; ++x)
                    for (int k = 0; k < sc; ++k) {
                        int fx = ox + x * sc + k;
                        if (fx < fb->w) put_px(fb, row, fx, src[x]);
                    }
            }
        }
    } else {
        /* panel is portrait: rotate 90 degrees clockwise */
        for (int y = 0; y < s->h; ++y)
            for (int x = 0; x < s->w; ++x) {
                int fx = fb->w - 1 - (oy + y), fy = ox + x;
                if (fx < 0 || fy >= fb->h) continue;
                put_px(fb, fb->mem + (size_t)fy * fb->stride, fx, s->px[(size_t)y * s->w + x]);
            }
    }
}
