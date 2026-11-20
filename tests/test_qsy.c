/* Automatic frequency changer, against a fake rigctld on 127.0.0.1:4532.
 * The fake keeps a VFO, answers the extended protocol (+f, +F, +M, +T) and
 * can refuse frequencies in a range, like a radio lacking a band. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "core/app.h"
#include "ui/input.h"
#include "ui/views.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok    " __VA_ARGS__); else { printf("  FAIL  " __VA_ARGS__); ++fails; } printf("\n"); } while (0)

static volatile long long g_vfo = 7150000;            /* radio starts on 40 m, off the FT8 spot */
static volatile long long g_refuse_lo, g_refuse_hi;   /* radio silently ignores these */
static volatile int g_sets;
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
                } else
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
    printf("[1] startup follows the radio's band\n");
    app_init(&a, cfg, 1);
    CHECK(a.rig_ok, "rig connected");
    CHECK(a.band == 3, "radio on 7.150 -> band 40m (got %d)", a.band);
    CHECK(g_vfo == 7074000 && a.dial_hz == 7074000, "VFO moved to 40m FT8 7.074 (vfo %lld)", g_vfo);

    printf("[2] mode change retunes\n");
    app_set_mode(&a, MODE_JS8);
    CHECK(g_vfo == 7078000, "JS8 -> 7.078 (vfo %lld)", g_vfo);
    app_set_mode(&a, MODE_WSPR);
    CHECK(g_vfo == 7038600, "WSPR -> 7.0386 (vfo %lld)", g_vfo);
    app_set_mode(&a, MODE_FT8);

    printf("[3] band stepping skips bands the QMX lacks\n");
    app_set_band(&a, 9);                        /* 10 m */
    CHECK(g_vfo == 28074000, "10m FT8 28.074");
    app_step_band(&a, 1);                       /* 6 m not on a QMX, 160 m neither -> wraps to 80 m */
    CHECK(a.band == 1 && g_vfo == 3573000, "10m -> next is 80m (band %d vfo %lld)", a.band, g_vfo);
    app_step_band(&a, -1);
    CHECK(a.band == 9, "80m -> previous is 10m (band %d)", a.band);
    int before = g_sets;
    app_set_band(&a, 10);
    CHECK(a.band == 9 && g_sets == before, "explicit 6m on a QMX refused without touching the VFO");

    printf("[4] radio refusing a band: reported and skipped\n");
    g_refuse_lo = 10100000; g_refuse_hi = 10150000;  /* this QMX build has no 30 m */
    app_set_band(&a, 5);                        /* 20 m */
    app_set_band(&a, 4);                        /* 30 m: refused */
    CHECK(strstr(a.status, "won't tune") != NULL, "status says so: \"%s\"", a.status);
    CHECK(a.dial_hz == 14074000 && a.band == 5, "display stays on 20m where the radio really is (%lld)", (long long)a.dial_hz);
    app_step_band(&a, -1);
    CHECK(a.band == 3 && g_vfo == 7074000, "20m -> previous skips 30m to 40m (band %d)", a.band);

    g_refuse_lo = 18068000; g_refuse_hi = 18168000;  /* and no 17 m: stepping up passes it */
    app_set_band(&a, 5);
    app_step_band(&a, 1);
    CHECK(a.band == 7 && g_vfo == 21074000, "20m -> next tries 17m, refused, lands on 15m (band %d)", a.band);
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

    printf("[10] Setup lists only the current mode's options\n");
    {
        /* item numbers 1-9 are JS8 options, 10-12 WSPR (see views.c) */
        op_mode_t md[] = {MODE_FT8, MODE_JS8, MODE_WSPR};
        int want_js8[] = {0, 9, 0}, want_wspr[] = {0, 0, 3};
        for (int k = 0; k < 3; ++k) {
            a.mode = md[k]; a.screen = 1; a.menu_sel = 0;
            int seen[64] = {0}, js8 = 0, wspr = 0;
            keyev_t dn = {K_DOWN, 0, 0};
            for (int n = 0; n < 40; ++n) { ui_key(&a, &dn); seen[a.menu_sel] = 1; }
            for (int i = 1; i <= 9; ++i) js8 += seen[i];
            for (int i = 10; i <= 12; ++i) wspr += seen[i];
            CHECK(js8 == want_js8[k] && wspr == want_wspr[k], "%s: %d JS8 items, %d WSPR items reachable", mode_name(md[k]), js8, wspr);
        }
        a.mode = MODE_JS8; a.screen = 1; a.menu_sel = 3;      /* on a JS8 item... */
        a.mode = MODE_FT8;                                    /* ...then the mode changes */
        keyev_t dn = {K_DOWN, 0, 0};
        ui_key(&a, &dn);
        CHECK(a.menu_sel == 13, "cursor on a hidden item moves to the next shown one (Band), got %d", a.menu_sel);
        a.screen = 0; a.mode = MODE_JS8;
    }

    printf("[8] switching the CAT port and audio from Setup\n");
    app_use_rig_device(&a, "/dev/ttyUSB1");
    CHECK(a.rig_busy && !a.rig_ok, "reconnecting in the background (UI not blocked)");
    for (int i = 0; i < 60 && a.rig_busy; ++i) wait_s(0.1);
    CHECK(!a.rig_busy && a.rig_ok, "back online: \"%s\"", a.status);
    CHECK(!strcmp(a.cfg.rig_device, "/dev/ttyUSB1"), "port stored in settings");
    app_use_audio(&a, NULL, "plughw:CARD=Device");
    CHECK(!strcmp(a.audio_out_dev, "plughw:CARD=Device") && !strcmp(a.cfg.audio_out, "plughw:CARD=Device"), "audio out switched");
    app_use_radio(&a, "g90");
    for (int i = 0; i < 60 && a.rig_busy; ++i) wait_s(0.1);
    CHECK(a.rig_ok && a.radio && !strcmp(a.radio->id, "g90") && (a.band_mask & 1u), "radio switched live to G90 (160 m now allowed)");
    a.tx_active = 1;
    app_use_rig_device(&a, "/dev/ttyUSB0");
    CHECK(!strcmp(a.cfg.rig_device, "/dev/ttyUSB1") && a.rig_ok, "no port change while transmitting");
    a.tx_active = 0;

    app_stop_threads(&a);
    unlink(cfg);
    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}
