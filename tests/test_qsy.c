/* Automatic frequency changer, against a fake rigctld on 127.0.0.1:4532.
 * The fake keeps a VFO, answers the extended protocol (+f, +F, +M, +T) and
 * can refuse frequencies in a range, like a radio lacking a band. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "core/app.h"
#include "core/clock.h"
#include "ui/input.h"
#include "ui/views.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok    " __VA_ARGS__); else { printf("  FAIL  " __VA_ARGS__); ++fails; } printf("\n"); } while (0)

static volatile long long g_vfo = 7150000;            /* radio starts on 40 m, off the FT8 spot */
static volatile long long g_refuse_lo, g_refuse_hi;   /* radio silently ignores these */
static volatile int g_sets, g_no_data_mode, g_mode_sets;
static char g_mode[16] = "USB";
static int g_pb = 2400;
static volatile int g_qmx_sim;      /* >0: a QMX whose SSB "Filter RX" is this many Hz (it ignores the passband asked for) */
static int g_lsock;

static void *fake_rigctld(void *arg)
{
    (void)arg;
    for (;;) {
        int c = accept(g_lsock, NULL, NULL);
        if (c < 0) return NULL;
        char buf[256];
        size_t got = 0;
        ssize_t r;
        while ((r = recv(c, buf + got, sizeof buf - 1 - got, 0)) > 0) {
            got += r;
            buf[got] = 0;
            char *nl;
            while ((nl = strchr(buf, '\n'))) {
                *nl = 0;
                char out[160];
                if (!strcmp(buf, "+f"))
                    snprintf(out, sizeof out, "get_freq:\nFrequency: %lld\nRPRT 0\n", g_vfo);
                else if (!strncmp(buf, "+F ", 3)) {
                    long long f = atoll(buf + 3);
                    ++g_sets;
                    if (!(f >= g_refuse_lo && f <= g_refuse_hi)) g_vfo = f;   /* refusal: VFO stays put */
                    snprintf(out, sizeof out, "set_freq: %lld\nRPRT 0\n", f);
                } else if (!strncmp(buf, "+M ", 3)) {
                    char m[16]; int pb = 0;
                    sscanf(buf + 3, "%15s %d", m, &pb);
                    ++g_mode_sets;
                    if (g_no_data_mode && !strncmp(m, "PKT", 3)) snprintf(out, sizeof out, "set_mode:\nRPRT -11\n");   /* like a G90 */
                    else {
                        snprintf(g_mode, sizeof g_mode, "%s", m);
                        if (g_qmx_sim) g_pb = g_qmx_sim;
                        else if (pb) g_pb = pb;
                        snprintf(out, sizeof out, "set_mode:\nRPRT 0\n");
                    }
                } else if (!strcmp(buf, "+m"))
                    snprintf(out, sizeof out, "get_mode:\nMode: %s\nPassband: %d\nRPRT 0\n", g_mode, g_pb);
                else
                    snprintf(out, sizeof out, "%s:\nRPRT 0\n", buf + 1);
                send(c, out, strlen(out), MSG_NOSIGNAL);
                memmove(buf, nl + 1, got - (nl + 1 - buf) + 1);
                got = strlen(buf);
            }
        }
        close(c);
    }
}

static void command(app_t *a, const char *c) { snprintf(a->input, sizeof a->input, "%s", c); app_submit_input(a); }

static void wait_s(double s) { struct timespec ts = {(time_t)s, (long)((s - (int)s) * 1e9)}; nanosleep(&ts, NULL); }

static int wait_for(volatile long long *v, long long want, double timeout)
{
    for (double t = 0; t < timeout; t += 0.1) { if (*v == want) return 1; wait_s(0.1); }
    return *v == want;
}

int main(void)
{
    g_lsock = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(g_lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(4532)};
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_lsock, (struct sockaddr *)&sa, sizeof sa) || listen(g_lsock, 4)) { perror("bind 4532"); return 1; }
    pthread_t th;
    pthread_create(&th, NULL, fake_rigctld, NULL);

    char cfg[] = "/tmp/qsoz-qsyXXXXXX";
    int fd = mkstemp(cfg);
    const char *text = "radio = qmx\nspawn_rigctld = 0\nwsprnet_upload = 0\nwspr_rx = 0\n"
                       "gps_device = /nonexistent\naudio_in = none\naudio_out = none\nlog_path = /tmp/qsoz-qsy.adi\n";
    if (write(fd, text, strlen(text)) < 0) return 1;
    close(fd);

    static app_t a;
    printf("[1] startup: WSPR on 20 m\n");
    app_init(&a, cfg, 1);
    CHECK(a.rig_ok, "rig connected");
    CHECK(a.mode == MODE_WSPR && a.band == 5, "starts in WSPR on 20m (mode %s, band %d)", mode_name(a.mode), a.band);
    CHECK(g_vfo == 14095600 && a.dial_hz == 14095600, "radio (was on 7.150) tuned to 20m WSPR 14.0956 (vfo %lld)", g_vfo);
    app_set_band(&a, 3);
    app_set_mode(&a, MODE_FT8);
    CHECK(g_vfo == 7074000, "then 40m FT8 7.074 as before (vfo %lld)", g_vfo);

    CHECK(!strcmp(g_mode, "USB") && g_pb == 3200, "on connect: QMX in USB, asked for 3.2 kHz (mode %s, %d Hz)", g_mode, g_pb);

    printf("[2] mode change retunes\n");
    app_set_mode(&a, MODE_JS8);
    CHECK(g_vfo == 7078000, "JS8 -> 7.078 (vfo %lld)", g_vfo);
    app_set_mode(&a, MODE_WSPR);
    CHECK(g_vfo == 7038600, "WSPR -> 7.0386 (vfo %lld)", g_vfo);
    app_set_mode(&a, MODE_FT8);

    printf("[3] band stepping stays on the bands the QMX (80-20 m version) has\n");
    app_set_band(&a, 5);                        /* 20 m */
    CHECK(g_vfo == 14074000, "20m FT8 14.074");
    app_step_band(&a, 1);                       /* 17-6 m and 160 m not on this QMX -> wraps to 80 m */
    CHECK(a.band == 1 && g_vfo == 3573000, "20m -> next is 80m (band %d vfo %lld)", a.band, g_vfo);
    app_step_band(&a, -1);
    CHECK(a.band == 5, "80m -> previous is 20m (band %d)", a.band);
    int before = g_sets;
    app_set_band(&a, 9);
    CHECK(a.band == 5 && g_sets == before, "explicit 10m on the 80-20 m QMX refused without touching the VFO");

    printf("[4] radio refusing a band: reported and skipped\n");
    g_refuse_lo = 10100000; g_refuse_hi = 10150000;  /* this QMX build has no 30 m */
    app_set_band(&a, 5);                        /* 20 m */
    app_set_band(&a, 4);                        /* 30 m: refused */
    CHECK(strstr(a.status, "won't tune") != NULL, "status says so: \"%s\"", a.status);
    CHECK(a.dial_hz == 14074000 && a.band == 5, "display stays on 20m where the radio really is (%lld)", (long long)a.dial_hz);
    app_step_band(&a, -1);
    CHECK(a.band == 3 && g_vfo == 7074000, "20m -> previous skips 30m to 40m (band %d)", a.band);

    g_refuse_lo = 7000000; g_refuse_hi = 7300000;    /* and now no 40 m: stepping down passes it */
    app_set_band(&a, 4);
    app_step_band(&a, -1);
    CHECK(a.band == 2 && g_vfo == 5357000, "30m -> previous tries 40m, refused, lands on 60m (band %d)", a.band);
    g_refuse_lo = g_refuse_hi = 0;

    printf("[5] VFO knob onto another band is followed\n");
    app_start_threads(&a);
    g_vfo = 24950000;                           /* operator turns the radio to 12 m */
    CHECK(wait_for(&g_vfo, 24915000, 6), "QSY to 12m FT8 24.915 (vfo %lld)", g_vfo);
    CHECK(a.band == 8, "band shows 12m");
    g_vfo = 24916500;                           /* small tune within the band */
    wait_s(4.5);
    CHECK(g_vfo == 24916500 && a.dial_hz == 24916500, "tuning inside the band is left alone");

    printf("[6] auto frequency off\n");
    command(&a, "/qsy off");
    CHECK(!a.cfg.auto_qsy, "toggled off");
    app_set_mode(&a, MODE_JS8);
    CHECK(g_vfo == 24916500, "mode change leaves the VFO alone");
    g_vfo = 14090000;
    wait_s(4.5);
    CHECK(g_vfo == 14090000 && a.band == 5, "VFO on 20m: band follows, frequency untouched");
    command(&a, "/qsy on");
    CHECK(g_vfo == 14078000, "turning it on snaps to 20m JS8 14.078");

    printf("[7] no QSY while transmitting\n");
    a.tx_active = 1;
    before = g_sets;
    app_set_band(&a, 3);
    app_set_mode(&a, MODE_FT8);
    CHECK(g_sets == before && a.band == 5 && a.mode == MODE_JS8, "band and mode held during TX");
    a.tx_active = 0;

    printf("[9] GHOSTNET\n");
    command(&a, "/ghost 40");
    CHECK(a.mode == MODE_JS8 && a.band == 3 && g_vfo == 7107000, "40m GHOSTNET 7.107 (vfo %lld)", g_vfo);
    app_step_band(&a, 1);
    CHECK(a.band == 5 && g_vfo == 14107000, "next band skips 30m -> 20m 14.107 (band %d vfo %lld)", a.band, g_vfo);
    app_step_band(&a, 1);
    CHECK(a.band == 1 && g_vfo == 3575000, "wraps to 80m 3.575 (vfo %lld)", g_vfo);
    app_set_mode(&a, MODE_FT8);
    CHECK(g_vfo == 3573000, "FT8 still uses the normal 3.573");
    app_set_mode(&a, MODE_JS8);
    CHECK(g_vfo == 3575000, "back to JS8 returns to GHOSTNET 3.575");
    g_vfo = 7120000;                            /* net moved: operator turns to 40m, 7.120 */
    for (int i = 0; i < 60 && a.band != 3; ++i) wait_s(0.1);
    wait_s(2.5);
    CHECK(a.band == 3 && g_vfo == 7120000, "VFO onto 40m: band follows, VFO left on 7.120 (vfo %lld)", g_vfo);
    g_vfo = 7110500;                            /* fine-tune within the band */
    wait_s(4.5);
    CHECK(g_vfo == 7110500, "fine-tuning left alone");
    app_set_mode(&a, MODE_FT8);
    CHECK(g_vfo == 7074000, "FT8 goes to 7.074");
    app_set_mode(&a, MODE_JS8);
    CHECK(g_vfo == 7110500, "back to JS8 returns to where the net was (7.1105, vfo %lld)", g_vfo);
    command(&a, "/ghost 40");
    CHECK(g_vfo == 7107000, "choosing GHOSTNET 40m again resets to 7.107");
    command(&a, "/ghost off");
    CHECK(!a.cfg.ghostnet && g_vfo == 7078000, "off -> normal JS8 7.078 (vfo %lld)", g_vfo);

    printf("[12] USB-D for every digital mode\n");
    {
        op_mode_t ms[] = {MODE_FT8, MODE_FT4, MODE_JS8, MODE_WSPR};
        int all = 1;
        const char *dm = a.radio->data_mode;               /* PKTUSB; plain USB on the QMX */
        int wpb = a.radio->qmx_ssb ? 3200 : 3000;
        for (int k = 0; k < 4; ++k) {
            snprintf(g_mode, sizeof g_mode, "LSB");          /* operator changed it on the radio */
            app_set_mode(&a, ms[k]);
            all &= !strcmp(g_mode, dm) && g_pb == wpb;
        }
        CHECK(all, "FT8, FT4, JS8, WSPR each put the radio back in %s %d Hz", dm, wpb);
        a.cfg.auto_qsy = 0;
        snprintf(g_mode, sizeof g_mode, "LSB");
        app_set_mode(&a, MODE_FT8);
        CHECK(!strcmp(g_mode, dm), "auto frequency off: VFO left alone but still %s", dm);
        a.cfg.auto_qsy = 1;
    }

    printf("[11] FT4\n");
    a.tx_active = 0;
    app_set_mode(&a, MODE_FT8);
    app_set_band(&a, 5);
    app_set_mode(&a, MODE_FT4);
    CHECK(g_vfo == 14080000, "FT4 on 20m -> 14.080 (vfo %lld)", g_vfo);
    app_set_band(&a, 3);
    CHECK(g_vfo == 7047500, "FT4 on 40m -> 7.0475 (vfo %lld)", g_vfo);
    app_step_band(&a, -1);
    CHECK(a.band == 1 && g_vfo == 3575000, "40m -> previous skips 60m (no FT4) to 80m 3.575 (band %d)", a.band);
    long long before4 = g_vfo;
    app_set_band(&a, 2);
    CHECK(g_vfo == before4 && strstr(a.status, "no FT4 frequency on 60m"), "60m: no FT4 frequency, VFO left alone (\"%s\")", a.status);
    app_set_mode(&a, MODE_JS8);
    app_set_band(&a, 5);

    printf("[10] Setup lists only the current mode's options\n");
    {
        /* item 1 is FT8's (CQ modifier), 2-15 JS8's (incl. GHOSTNET + APRS), 16-18 WSPR's (see views.c) */
        op_mode_t md[] = {MODE_FT8, MODE_JS8, MODE_WSPR};
        int want_ft8[] = {1, 0, 0}, want_js8[] = {0, 14, 0}, want_wspr[] = {0, 0, 3};
        for (int k = 0; k < 3; ++k) {
            a.mode = md[k]; a.screen = 1; a.menu_sel = 0;
            int seen[64] = {0}, ft8 = 0, js8 = 0, wspr = 0;
            keyev_t dn = {K_DOWN, 0, 0};
            for (int n = 0; n < 50; ++n) { ui_key(&a, &dn); seen[a.menu_sel] = 1; }
            ft8 = seen[1];
            for (int i = 2; i <= 15; ++i) js8 += seen[i];
            for (int i = 16; i <= 18; ++i) wspr += seen[i];
            CHECK(ft8 == want_ft8[k] && js8 == want_js8[k] && wspr == want_wspr[k],
                  "%s: %d FT8, %d JS8, %d WSPR items reachable", mode_name(md[k]), ft8, js8, wspr);
        }
        a.mode = MODE_JS8; a.screen = 1; a.menu_sel = 3;      /* on a JS8 item... */
        a.mode = MODE_FT8;                                    /* ...then the mode changes */
        keyev_t dn = {K_DOWN, 0, 0};
        ui_key(&a, &dn);
        CHECK(a.menu_sel == 19, "cursor on a hidden item moves to the next shown one (Band), got %d", a.menu_sel);
        a.screen = 0; a.mode = MODE_JS8;
    }

    printf("[8] switching the CAT port and audio from Setup\n");
    app_use_rig_device(&a, "/dev/ttyUSB1");
    CHECK(a.rig_busy && !a.rig_ok, "reconnecting in the background (UI not blocked)");
    for (int i = 0; i < 60 && a.rig_busy; ++i) wait_s(0.1);
    CHECK(!a.rig_busy && a.rig_ok, "back online: \"%s\"", a.status);
    CHECK(!strcmp(a.cfg.rig_device, "/dev/ttyUSB1"), "port stored in settings");
    {
        /* fake sound cards: a QMX and a Digirig; the built-in codec must never be auto-picked */
        char r[] = "/tmp/qsoz-sndXXXXXX", p[256];
        if (!mkdtemp(r)) return 1;
        snprintf(p, sizeof p, "mkdir -p %s/proc/asound", r); if (system(p)) {}
        snprintf(p, sizeof p, "%s/proc/asound/cards", r);
        FILE *f = fopen(p, "w");
        fputs(" 0 [ES8388Audio    ]: simple-card - ES8388-Audio\n  x\n"
              " 2 [QMX            ]: USB-Audio - QMX Transceiver\n  x\n"
              " 3 [Device         ]: USB-Audio - USB Audio Device\n  x\n", f);
        fclose(f);
        snprintf(p, sizeof p, "%s/proc/asound/pcm", r);
        f = fopen(p, "w");
        fputs("00-00: ES8388 : ES8388 : playback 1 : capture 1\n02-00: USB Audio : USB Audio : playback 1 : capture 1\n"
              "03-00: USB Audio : USB Audio : playback 1 : capture 1\n", f);
        fclose(f);
        setenv("QSOZERO_DEVROOT", r, 1);
        a.cfg.audio_in[0] = a.cfg.audio_out[0] = 0;
        app_rescan_audio(&a);                   /* radio is the QMX here */
        CHECK(!strcmp(a.audio_in_dev, "plughw:CARD=QMX,DEV=0") && !strcmp(a.audio_out_dev, "plughw:CARD=QMX,DEV=0"),
              "rescan: auto picks the QMX for in and out (%s / %s)", a.audio_in_dev, a.audio_out_dev);
        app_use_audio(&a, NULL, "plughw:CARD=Device,DEV=0");
        CHECK(!strcmp(a.audio_out_dev, "plughw:CARD=Device,DEV=0") && !strcmp(a.cfg.audio_out, "plughw:CARD=Device,DEV=0"),
              "audio out switched to the Digirig");
        app_use_audio(&a, "plughw:CARD=Gone,DEV=0", NULL);
        CHECK(!strcmp(a.audio_in_dev, "plughw:CARD=QMX,DEV=0") && strstr(a.status, "not found"),
              "unplugged input falls back to auto: \"%s\"", a.status);
        snprintf(p, sizeof p, "%s/proc/asound/pcm", r);
        f = fopen(p, "w"); fputs("00-00: ES8388 : ES8388 : playback 1 : capture 1\n", f); fclose(f);
        a.cfg.audio_in[0] = a.cfg.audio_out[0] = 0;
        app_rescan_audio(&a);
        CHECK(!a.audio_in_dev[0] && strstr(a.status, "no radio sound card"), "radio unplugged: says so, doesn't use the built-in: \"%s\"", a.status);
        unsetenv("QSOZERO_DEVROOT");
        snprintf(p, sizeof p, "rm -rf %s", r); if (system(p)) {}
    }
    g_no_data_mode = 1;                                    /* the G90 has no data mode */
    app_use_radio(&a, "g90");
    for (int i = 0; i < 60 && a.rig_busy; ++i) wait_s(0.1);
    CHECK(!strcmp(g_mode, "USB"), "radio without a data mode: falls back to USB (mode %s)", g_mode);
    g_no_data_mode = 0;
    CHECK(a.rig_ok && a.radio && !strcmp(a.radio->id, "g90") && (a.band_mask & 1u), "radio switched live to G90 (160 m now allowed)");
    CHECK(a.cfg.rig_device[0] == 0, "the CAT port pinned for the QMX went back to auto for the G90");
    snprintf(a.cfg.rig_device, sizeof a.cfg.rig_device, "/dev/ttyUSB1");
    a.tx_active = 1;
    app_use_rig_device(&a, "/dev/ttyUSB0");
    CHECK(!strcmp(a.cfg.rig_device, "/dev/ttyUSB1") && a.rig_ok, "no port change while transmitting");
    a.tx_active = 0;

    app_stop_threads(&a);
    unlink(cfg);
    printf("[14] QMX: USB (SSB) mode, 3.2 kHz filter, radio changes\n");
    {
        a.tx_active = 0;
        app_use_radio(&a, "qmx");
        for (int i = 0; i < 60 && a.rig_busy; ++i) wait_s(0.1);
        a.tx_active = 0;
        g_qmx_sim = 2700;                                /* QMX with the default 2.7 kHz SSB filter */
        app_use_radio(&a, "qmxmid");
        for (int i = 0; i < 60 && a.rig_busy; ++i) wait_s(0.1);
        app_use_radio(&a, "qmx");
        for (int i = 0; i < 60 && a.rig_busy; ++i) wait_s(0.1);
        CHECK(!strcmp(g_mode, "USB"), "QMX put in USB (SSB) mode, not Digi (%s)", g_mode);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        app_set_band(&a, 3);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        CHECK(!strcmp(g_mode, "USB") && ms < 500, "band change: still USB, %.0f ms", ms);
        g_qmx_sim = 3200;                                /* Filter RX set to 3200 on the radio */
        app_use_radio(&a, "qmxhigh");
        for (int i = 0; i < 60 && a.rig_busy; ++i) wait_s(0.1);
        a.status[0] = 0;
        app_use_radio(&a, "qmx");
        for (int i = 0; i < 60 && a.rig_busy; ++i) wait_s(0.1);
        CHECK(!strcmp(g_mode, "USB"), "QMX stays in USB (%s)", g_mode);
        g_qmx_sim = 0;
        /* choosing a QMX: a 30 s pop-up says what to set on the radio; one esc closes it */
        a.notice[0] = 0;
        app_use_radio(&a, "x6200");
        for (int i = 0; i < 60 && a.rig_busy; ++i) wait_s(0.1);
        CHECK(!a.notice[0], "X6200: no pop-up");
        app_use_radio(&a, "qmx");
        for (int i = 0; i < 60 && a.rig_busy; ++i) wait_s(0.1);
        double left = a.notice_until - clk_mono();
        CHECK(strstr(a.notice, "Filter RX and Filter TX to 3200 Hz") && left > 25 && left <= 30, "QMX: pop-up to set Filter RX/TX 3200 Hz, for 30 s (%.0f s left)", left);
        a.screen = 1;
        keyev_t esc = {K_ESC, 0, 0};
        ui_key(&a, &esc);
        CHECK(!a.notice[0] && a.screen == 1, "one esc closes the pop-up and nothing else (still in Setup)");
        /* changing radio: a pinned CAT port goes back to auto; the QMX gets 100% TX audio */
        snprintf(a.cfg.rig_device, sizeof a.cfg.rig_device, "/dev/serial/by-id/usb-1a86_USB_Dual_Serial-if02");
        a.cfg.tx_level = 0.5f;
        app_use_radio(&a, "x6200");
        for (int i = 0; i < 60 && a.rig_busy; ++i) wait_s(0.1);
        CHECK(a.cfg.rig_device[0] == 0, "radio changed: pinned CAT port reset to auto");
        CHECK(!strcmp(g_mode, "PKTUSB") && g_pb == 3000, "X6200 still gets PKTUSB (USB-D) 3 kHz (%s %d)", g_mode, g_pb);
        CHECK(fabsf(a.cfg.tx_level - 0.5f) < 1e-6, "X6200: TX audio level left alone (%.2f)", a.cfg.tx_level);
        app_use_radio(&a, "qmx");
        CHECK(fabsf(a.cfg.tx_level - 1.0f) < 1e-6, "QMX selected: TX audio level 100%% (%.2f)", a.cfg.tx_level);
        app_use_radio(&a, "qmxplus");
        CHECK(fabsf(a.cfg.tx_level - 1.0f) < 1e-6, "QMX+ keeps whatever it was (%.2f)", a.cfg.tx_level);
        /* the three QMX hardware versions */
        static const struct { const char *id; unsigned want; } qv[] = {
            {"qmx", 0x03E}, {"qmxmid", 0x0FC}, {"qmxhigh", 0x3E0}};
        int okv = 1;
        for (int i = 0; i < 3; ++i) {
            app_use_radio(&a, qv[i].id);
            for (int k = 0; k < 60 && a.rig_busy; ++k) wait_s(0.1);
            okv &= (a.band_mask == qv[i].want) && fabsf(a.cfg.tx_level - 1.0f) < 1e-6 && a.radio->qmx_ssb;
        }
        CHECK(okv, "QMX 80-20m / 60-15m / 20-10m: each offers only its own bands, 100%% TX audio, Digi mode");
        app_use_radio(&a, "qmxplus");
        for (int k = 0; k < 60 && a.rig_busy; ++k) wait_s(0.1);
        snprintf(a.cfg.rig_device, sizeof a.cfg.rig_device, "/dev/ttyACM0");
        app_use_radio(&a, "qmxplus");
        CHECK(!strcmp(a.cfg.rig_device, "/dev/ttyACM0"), "same radio picked again: CAT port kept");
    }

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}
