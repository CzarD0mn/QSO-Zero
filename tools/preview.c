/* Render the real UI code to PPM screenshots with synthetic band activity.
 * Usage: build/preview <outdir> */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/app.h"
#include "core/clock.h"
#include "ui/gfx.h"
#include "ui/views.h"

static void save_ppm(const surface_t *s, int scale, const char *path)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", s->w * scale, s->h * scale);
    for (int y = 0; y < s->h * scale; ++y)
        for (int x = 0; x < s->w * scale; ++x) {
            uint16_t c = s->px[(y / scale) * s->w + x / scale];
            unsigned char rgb[3] = {(unsigned char)((c >> 11) << 3), (unsigned char)(((c >> 5) & 63) << 2), (unsigned char)((c & 31) << 3)};
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

static void fake_waterfall(app_t *a)
{
    srand(7);
    float sig[][3] = {{620, 50, 22}, {980, 50, 14}, {1210, 50, 28}, {1500, 50, 18}, {1655, 50, 9},
                      {2015, 50, 25}, {2390, 50, 12}, {2700, 50, 16}};
    for (int r = 0; r < WF_ROWS; ++r) {
        int slot = (r / 47) & 1;
        for (int b = 0; b < WF_BINS; ++b) {
            float f = b * WF_FMAX / WF_BINS;
            float v = 60 + 35 * ((rand() % 1000) / 1000.0f) + 25 * expf(-(f - 1500) * (f - 1500) / 2e6f) - 20 * (f < 250);
            for (int k = 0; k < 8; ++k) {
                if (((k & 1) != slot) || (r % 47) > 40) continue;
                if (f >= sig[k][0] && f <= sig[k][0] + sig[k][1]) {
                    int tone = (int)((f - sig[k][0]) / 6.25f), want = (r * 7 + k * 3) % 8;
                    v += tone == want || tone == want + 1 ? sig[k][2] * 6 : sig[k][2] * 1.5f;
                }
            }
            a->wf[r][b] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
    a->wf_head = WF_ROWS - 1;
}

static void add(app_t *a, int snr, float dt, float f, uint8_t flags, const char *t, double utc)
{
    rxline_t l = {.utc = utc, .snr = snr, .dt = dt, .freq = f, .mode = a->mode, .flags = flags};
    snprintf(l.text, sizeof l.text, "%s", t);
    app_add_line(a, &l);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    static app_t a;
    app_init(&a, "/nonexistent", 0);
    snprintf(a.cfg.callsign, sizeof a.cfg.callsign, "KN4CRD");
    snprintf(a.cfg.grid, sizeof a.cfg.grid, "EM73tu");
    fake_waterfall(&a);
    double t = floor(clk_now() / 15) * 15 - 60;
    add(&a, -12, 0.1f, 620, LINE_CQ, "CQ POTA W1AW FN31", t);
    add(&a, -3, 0.2f, 980, 0, "K1ABC W9XYZ R-08", t);
    add(&a, -18, -0.3f, 1210, LINE_CQ, "CQ DX VE3XYZ FN03", t);
    add(&a, 0, 0, 1500, LINE_TX, "VE3XYZ KN4CRD EM73", t + 15);
    add(&a, -9, 0.1f, 1655, 0, "JA1ABC KH6XX -15", t + 30);
    add(&a, -15, -0.1f, 1212, LINE_TOME, "KN4CRD VE3XYZ -11", t + 30);
    add(&a, 0, 0, 1500, LINE_TX, "VE3XYZ KN4CRD R-15", t + 45);
    add(&a, -21, 0.4f, 2015, LINE_CQ, "CQ SOTA G4ABC IO91", t + 30);
    add(&a, -7, 0.0f, 2390, 0, "DL1XX EA8ZZ RR73", t + 30);
    add(&a, -14, -0.1f, 1213, LINE_TOME, "KN4CRD VE3XYZ RR73", t + 60);
    add(&a, -11, 0.2f, 2700, LINE_CQ, "CQ K5QRP EM12", t + 60);
    a.rx_off = 1210;
    a.tx_off = 1210;
    a.seq.st = SEQ_RR73;
    snprintf(a.seq.dx, sizeof a.seq.dx, "VE3XYZ");
    a.rig_ok = 1;
    a.last_decode_ms = 840;
    a.cfg.tx_level = 0.45f;
    snprintf(a.input, sizeof a.input, "VE3XYZ KN4CRD 73");
    a.input_len = (int)strlen(a.input);
    a.sel = 6;

    char p[512];
    surface_t *c = surf_new(320, 170), *L = surf_new(LARGE_W, LARGE_H);
    ui_render(&a, c);
    snprintf(p, sizeof p, "%s/compact_ft8.ppm", dir); save_ppm(c, 3, p);
    ui_render(&a, L);
    snprintf(p, sizeof p, "%s/large_ft8.ppm", dir); save_ppm(L, 2, p);

    /* JS8 */
    a.mode = MODE_JS8; a.js8_sub = 0; a.nlines = 0; a.sel = -1; a.band = 3; a.dial_hz = 7078000;
    a.rx_off = 1450; a.tx_off = 1450;
    add(&a, -16, 0.1f, 820, LINE_HB, "W1AW: @HB HEARTBEAT FN31 ", t);
    add(&a, -8, 0.0f, 1450, 0, "VE3XYZ: @ALLCALL CQ CQ CQ FN03 ", t + 15);
    add(&a, 0, 0, 1450, LINE_TX, "KN4CRD: VE3XYZ SNR -08 ", t + 30);
    add(&a, -10, 0.1f, 1452, LINE_TOME, "VE3XYZ: KN4CRD  QTH ON A HILLTOP, 5W QMX AND EFHW. TNX FOR THE CALL 73", t + 45);
    add(&a, -19, 0.3f, 2210, LINE_PARTIAL, "G4ABC: @ALLCALL  QRV FOR POTA G-0123 ON 40", t + 60);
    a.input[0] = 0; a.input_len = 0;
    app_status(&a, "JS8: 4 frames (60 s)");
    ui_render(&a, c);
    snprintf(p, sizeof p, "%s/compact_js8.ppm", dir); save_ppm(c, 3, p);
    ui_render(&a, L);
    snprintf(p, sizeof p, "%s/large_js8.ppm", dir); save_ppm(L, 2, p);

    /* JS8 with auto-reply armed */
    a.cfg.js8_autoreply = 1; a.cfg.js8_hb_ack = 1; a.autoreplies = 3;
    add(&a, -9, 0.1f, 1452, LINE_TOME, "VE3XYZ: KN4CRD SNR? ", t + 75);
    add(&a, 0, 0, 1450, LINE_SYS, "AUTO-REPLY queued: VE3XYZ SNR -09", t + 75);
    add(&a, 0, 0, 1450, LINE_TX, "KN4CRD: VE3XYZ SNR -09 ", t + 90);
    ui_render(&a, L);
    snprintf(p, sizeof p, "%s/large_js8_auto.ppm", dir); save_ppm(L, 2, p);

    /* WSPR spots */
    a.mode = MODE_WSPR; a.nlines = 0; a.sel = -1; a.band = 5; a.dial_hz = 14095600; a.wspr_beacon = 1;
    a.rx_off = a.tx_off = 1500; a.cfg.js8_autoreply = 0;
    add(&a, -12, 0.2f, 1432.5f, 0, "K1ABC   FN42   37dBm  1579km dr+0", t);
    add(&a, -24, -0.1f, 1465.5f, 0, "G4JNT   IO90   23dBm  6808km dr+0", t);
    add(&a, -19, 0.4f, 1520.1f, 0, "VK2XYZ  QF56   30dBm 14710km dr-1", t);
    add(&a, -27, 0.0f, 1561.0f, 0, "JA1ABC  PM95   10dBm 10880km dr+0", t);
    add(&a, 0, 0, 1500, LINE_TX, "WSPR KN4CRD EM73 37", t + 120);
    add(&a, -16, 0.1f, 1488.8f, 0, "W1AW    FN31   37dBm  1438km dr+0", t + 240);
    app_status(&a, "WSPR beacon ON (20%)");
    ui_render(&a, c);
    snprintf(p, sizeof p, "%s/compact_wspr.ppm", dir); save_ppm(c, 3, p);

    /* hold-ESC exit prompt over the FT8 screen */
    a.mode = MODE_FT8; a.esc_t0 = clk_mono() - 0.55;
    ui_render(&a, c);
    snprintf(p, sizeof p, "%s/compact_exit.ppm", dir); save_ppm(c, 3, p);
    a.esc_t0 = 0;

    a.tuning = 1; a.tune_until = clk_now() + 3.2;
    ui_render(&a, c);
    snprintf(p, sizeof p, "%s/compact_tune.ppm", dir); save_ppm(c, 3, p);
    a.tuning = 0;

    a.screen = 1; a.menu_sel = 7;
    ui_render(&a, c);
    snprintf(p, sizeof p, "%s/compact_menu.ppm", dir); save_ppm(c, 3, p);
    a.screen = 2;
    ui_render(&a, c);
    snprintf(p, sizeof p, "%s/compact_tools.ppm", dir); save_ppm(c, 3, p);
    printf("wrote previews to %s\n", dir);
    return 0;
}
