/* QSO-Zero - field digital-mode terminal for the M5Stack Cardputer Zero.
 *
 *   qso-zero [--config FILE] [--fb /dev/fbN] [--nohw]
 *
 * --nohw runs the UI and scheduler without radio, audio or GPS (demo/test).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <grp.h>
#include <pwd.h>
#include <unistd.h>
#include "net/logbook.h"
#include "core/log.h"
#include "core/bench.h"
#include <time.h>
#include "core/app.h"
#include "core/clock.h"
#include "ui/fb.h"
#include "ui/gfx.h"
#include "ui/input.h"
#include "ui/views.h"

#ifndef QSOZERO_VERSION
#define QSOZERO_VERSION "dev"
#endif

static void usage(void)
{
    fprintf(stderr, "usage: qso-zero [--config FILE] [--fb /dev/fbN|auto] [--nohw] [--version] [--bench]\n"
                    "       qso-zero [--config FILE] --logbook status|test|stations|upload|mark [qrz|wavelog|all]\n");
}

int main(int argc, char **argv)
{
    const char *cfg = NULL, *fbdev = NULL;
    int hw = 1;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) cfg = argv[++i];
        else if (!strcmp(argv[i], "--fb") && i + 1 < argc) fbdev = argv[++i];
        else if (!strcmp(argv[i], "--nohw")) hw = 0;
        else if (!strcmp(argv[i], "--version")) { printf("qso-zero %s\n", QSOZERO_VERSION); return 0; }
        else if (!strcmp(argv[i], "--bench")) return bench_run();
        else if (!strcmp(argv[i], "--logbook")) {
            /* QRZ / Wavelog uploads from the command line (qso-zero-setup --logbook / --upload) */
            config_t c;
            config_defaults(&c);
            config_load(&c, cfg ? cfg : config_default_path());
            return logbook_cli(argc - i - 1, argv + i + 1, logbook_conf_path(), c.log_path);
        }
        else { usage(); return 2; }
    }
    log_open(NULL);
    {
        char gl[256] = "";
        gid_t gs[32];
        int ng = getgroups(32, gs);
        for (int i = 0; i < ng; ++i) {
            struct group *g = getgrgid(gs[i]);
            snprintf(gl + strlen(gl), sizeof gl - strlen(gl), "%s%s", i ? "," : "", g ? g->gr_name : "?");
        }
        struct passwd *pw = getpwuid(getuid());
        qlog("start", "QSO-Zero %s  user %s (uid %d)  groups %s", QSOZERO_VERSION, pw ? pw->pw_name : "?", (int)getuid(), gl);
        char args[256] = "";
        for (int i = 1; i < argc; ++i) snprintf(args + strlen(args), sizeof args - strlen(args), " %s", argv[i]);
        qlog("start", "args:%s  config %s  hardware %s  fb from launcher %s", args[0] ? args : " (none)", cfg ? cfg : config_default_path(),
             hw ? "on" : "off (--nohw)", getenv("APPLAUNCH_LINUX_FBDEV_DEVICE") ? getenv("APPLAUNCH_LINUX_FBDEV_DEVICE") : "-");
    }
    static app_t app;
    app_t *a = &app;
    app_init(a, cfg ? cfg : config_default_path(), hw);

    fb_t fb;
    if (fb_open(&fb, fbdev ? fbdev : a->cfg.display)) {
        fprintf(stderr, "qso-zero: no framebuffer (is the LCD/HDMI fb enabled?)\n");
        qlog("display", "no framebuffer (%s)%s", fbdev ? fbdev : a->cfg.display, fb_hdmi_connected() ? "  HDMI monitor connected" : "");
        app_shutdown(a);
        return 1;
    }
    qlog("display", "framebuffer %s %dx%d %d bpp%s%s", fb.path, fb.w, fb.h, fb.bpp, fb.is_hdmi ? " HDMI" : " LCD", fb.rotate ? " rotated" : "");
    if (input_open(a->cfg.keyboard)) {
        fprintf(stderr, "qso-zero: no keyboard found (check /dev/input permissions)\n");
        qlog("input", "no keyboard found (check /dev/input permissions / input group)");
    } else {
        for (int i = 0; i < input_device_count(); ++i)
            qlog("input", "keyboard \"%s\"%s", input_device_name(i), input_device_builtin(i) ? " (Cardputer Zero keypad: Sym layer on)" : "");
        qlog("input", "Sym keymap: %s", input_keymap_from_file() ? "from /usr/share/keymaps/tca8418_keypad_m5stack_keymap.map" : "built-in table (keymap file not found)");
    }

    ui_start(a);                                        /* open in Setup, on the Radio line */
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
                if (fb_open(&fb, "auto") == 0) { was_hdmi = fb.is_hdmi; a->dirty = 1; qlog("display", "HDMI %s: now %s %dx%d", h ? "connected" : "removed", fb.path, fb.w, fb.h); }
                else { was_hdmi = h; qlog("display", "HDMI %s: no usable framebuffer", h ? "connected" : "removed"); }
            }
        }
        /* redraw on change, at most ~15 fps (keeps the 1 GHz A53 mostly idle) */
        if ((a->dirty || a->tx_active || a->esc_t0 > 0) && now - last_frame > 0.066) {
            a->dirty = 0;
            last_frame = now;
            int use_large = (fb.rotate ? fb.h : fb.w) >= LARGE_W;     /* automatic: large on HDMI */
            surface_t *s = use_large ? large : compact;
            ui_render(a, s);
            fb_present(&fb, s);
        }
    }
    qlog("exit", "quit from the app");
    app_shutdown(a);
    fb_close(&fb);
    input_close();
    log_close();
    surf_free(compact);
    surf_free(large);
    return 0;
}
