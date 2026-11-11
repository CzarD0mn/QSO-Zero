/* QSO-Zero - field digital-mode terminal for the M5Stack Cardputer Zero.
 *
 *   qso-zero [--config FILE] [--fb /dev/fbN] [--nohw]
 *
 * --nohw runs the UI and scheduler without radio, audio or GPS (demo/test).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "core/app.h"
#include "core/clock.h"
#include "ui/fb.h"
#include "ui/gfx.h"
#include "ui/input.h"
#include "ui/views.h"

static void usage(void)
{
    fprintf(stderr, "usage: qso-zero [--config FILE] [--fb /dev/fbN|auto] [--nohw]\n");
}

int main(int argc, char **argv)
{
    const char *cfg = NULL, *fbdev = NULL;
    int hw = 1;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) cfg = argv[++i];
        else if (!strcmp(argv[i], "--fb") && i + 1 < argc) fbdev = argv[++i];
        else if (!strcmp(argv[i], "--nohw")) hw = 0;
        else { usage(); return 2; }
    }
    static app_t app;
    app_t *a = &app;
    app_init(a, cfg ? cfg : config_default_path(), hw);

    fb_t fb;
    if (fb_open(&fb, fbdev ? fbdev : a->cfg.display)) {
        fprintf(stderr, "qso-zero: no framebuffer (is the LCD/HDMI fb enabled?)\n");
        app_shutdown(a);
        return 1;
    }
    if (input_open(a->cfg.keyboard)) fprintf(stderr, "qso-zero: no keyboard found (check /dev/input permissions)\n");

    surface_t *compact = surf_new(320, 170), *large = surf_new(LARGE_W, LARGE_H);
    app_start_threads(a);

    double last_hdmi_check = 0, last_frame = 0;
    int was_hdmi = fb.is_hdmi;
    while (!a->quit) {
        keyev_t k;
        while (input_poll(&k, 30)) ui_key(a, &k);

        double now = clk_mono();
        /* follow HDMI hot-plug: big screen when a monitor appears */
        if (!fbdev && !strcmp(a->cfg.display, "auto") && now - last_hdmi_check > 2.0) {
            last_hdmi_check = now;
            int h = fb_hdmi_connected();
            if (h != was_hdmi) {
                fb_close(&fb);
                if (fb_open(&fb, "auto") == 0) { was_hdmi = fb.is_hdmi; a->dirty = 1; }
            }
        }
        /* redraw on change, at most ~15 fps (keeps the 1 GHz A53 mostly idle) */
        if ((a->dirty || a->tx_active) && now - last_frame > 0.066) {
            a->dirty = 0;
            last_frame = now;
            int use_large = a->large == 1 || (a->large == 0 && (fb.rotate ? fb.h : fb.w) >= LARGE_W);
            surface_t *s = use_large ? large : compact;
            ui_render(a, s);
            fb_present(&fb, s);
        }
    }
    app_shutdown(a);
    fb_close(&fb);
    input_close();
    surf_free(compact);
    surf_free(large);
    return 0;
}
