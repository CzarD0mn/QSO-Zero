/* QSO-Zero - screens. Compact layout targets the 320x170 ST7789; the large
 * layout is drawn on a 640x360 canvas and integer-scaled to HDMI. */
#include "views.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "core/clock.h"
#include "dsp/js8.h"
#include "dsp/wspr.h"
#include "gps/gps.h"
#include "rig/rigctl.h"
#include "theme.h"
#include "net/uplink.h"

float audio_rx_level(void);

/* ------------------------------------------------------------------ */
/* helpers                                                              */
/* ------------------------------------------------------------------ */

static void fmt_freq(int64_t hz, char *out)
{
    long long mhz = hz / 1000000, khz = (hz / 1000) % 1000, h = hz % 1000;
    sprintf(out, "%lld.%03lld.%03lld", mhz, khz, h);
}

static void fmt_utc(double t, char *out, int secs)
{
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    if (secs) sprintf(out, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    else sprintf(out, "%02d%02d%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static uint16_t line_color(const rxline_t *l)
{
    if (l->flags & LINE_TX) return C_MAGENTA;
    if (l->flags & LINE_TOME) return C_SUN;
    if (l->flags & LINE_SYS) return C_ORANGE;
    if (l->flags & LINE_CQ) return C_CYAN;
    if (l->flags & LINE_HB) return C_PURPLE;
    if (l->flags & LINE_PARTIAL) return C_DIM;
    return C_TEXT;
}

static int band_bw(const app_t *a)
{
    if (a->mode == MODE_FT8) return 50;
    if (a->mode == MODE_WSPR) return 6;
    return (int)js8_submode_info((js8_submode_t)a->js8_sub)->bandwidth;
}

static const char *sub_short(int s)
{
    static const char *n[] = {"NORM", "FAST", "TURBO", "SLOW"};
    return s >= 0 && s < 4 ? n[s] : "?";
}

/* striped synthwave sun, radius r, centred at (cx, cy), top half only */
static void draw_sun(surface_t *s, int cx, int cy, int r)
{
    for (int y = -r; y <= 0; ++y) {
        int d = y + r;                       /* 0 at top .. r at horizon */
        if (d > r * 2 / 5) {                 /* stripes on the lower part */
            int k = (d - r * 2 / 5) * 4 / (r - r * 2 / 5 + 1);   /* 0..3 */
            if ((d % 4) < 1 + k / 2 && d % 4 != 3) continue;
        }
        int half = (int)sqrt((double)(r * r - y * y));
        gfx_hline(s, cx - half, cy + y, 2 * half + 1, gfx_lerp(C_SUN, C_MAGENTA, d * 255 / (r + 1)));
    }
}

static void draw_waterfall(app_t *a, surface_t *s, int x, int y, int w, int h)
{
    const uint16_t *pal = theme_wf_palette();
    for (int r = 0; r < h; ++r) {
        int src = (a->wf_head - r + WF_ROWS) % WF_ROWS;
        uint16_t *dst = s->px + (size_t)(y + r) * s->w + x;
        for (int i = 0; i < w; ++i) dst[i] = pal[a->wf[src][i * WF_BINS / w]];
    }
    /* 500 Hz grid ticks */
    for (int f = 500; f < 3000; f += 500) {
        int px = x + (int)(f / WF_FMAX * w);
        for (int r = 0; r < h; r += 3) gfx_pixel(s, px, y + r, C_GRID);
    }
    /* RX (cyan, top) and TX (magenta, bottom) passband markers */
    int bw = band_bw(a);
    int rx0 = x + (int)(a->rx_off / WF_FMAX * w), rx1 = x + (int)((a->rx_off + bw) / WF_FMAX * w);
    int tx0 = x + (int)(a->tx_off / WF_FMAX * w), tx1 = x + (int)((a->tx_off + bw) / WF_FMAX * w);
    if (rx1 <= rx0) rx1 = rx0 + 1;
    if (tx1 <= tx0) tx1 = tx0 + 1;
    gfx_fill(s, rx0, y, rx1 - rx0 + 1, 2, C_CYAN);
    gfx_vline(s, rx0, y, 5, C_CYAN);
    gfx_vline(s, rx1, y, 5, C_CYAN);
    gfx_fill(s, tx0, y + h - 2, tx1 - tx0 + 1, 2, C_MAGENTA);
    gfx_vline(s, tx0, y + h - 5, 5, C_MAGENTA);
    gfx_vline(s, tx1, y + h - 5, 5, C_MAGENTA);
}

static void draw_period_bar(app_t *a, surface_t *s, int x, int y, int w, int h)
{
    int P = mode_period(a);
    double pos = clk_period_pos(P, NULL);
    int fill = (int)(pos / P * w);
    gfx_fill(s, x, y, w, h, C_PANEL);
    if (a->tx_active) gfx_hgrad(s, x, y, fill, h, C_RED, C_MAGENTA);
    else gfx_hgrad(s, x, y, fill, h, C_PURPLE, C_ORANGE);
}

/* word-wrap helper: returns length of the next row (<= room) */
static int wrap_len(const char *t, int room)
{
    int len = (int)strlen(t);
    if (len <= room) return len;
    int cut = room;
    while (cut > room / 2 && t[cut] != ' ') --cut;
    return cut > room / 2 ? cut : room;
}

static int wrap_rows(const char *t, int room, int maxrows)
{
    int rows = 0;
    while (*t && rows < maxrows) {
        int n = wrap_len(t, room);
        t += n;
        while (*t == ' ') ++t;
        ++rows;
    }
    return rows ? rows : 1;
}

static int draw_wrapped(surface_t *s, int x, int y, const char *t, int room, int maxrows, uint16_t c)
{
    int rows = 0;
    while (*t && rows < maxrows) {
        int n = wrap_len(t, room);
        gfx_text_n(s, x, y + rows * FONT_H, t, n, c, 1);
        t += n;
        while (*t == ' ') ++t;
        ++rows;
    }
    return rows;
}

static void draw_list(app_t *a, surface_t *s, int x, int y, int w, int rows, int wide)
{
    int cols = w / FONT_W;
    int prew = wide ? 21 : 9;               /* prefix columns */
    int room = cols - prew - 1;
    int last = a->nlines - 1 - a->scroll;
    if (a->sel >= 0 && a->sel < last - rows + 1) last = a->sel + rows - 1;
    if (a->sel > last) last = a->sel;
    if (last > a->nlines - 1) last = a->nlines - 1;
    /* walk back from 'last' until the rows are used (lines wrap up to 4 rows) */
    int first = last, used = 0;
    while (first >= 0) {
        int need = wrap_rows(a->lines[first].text, room, 4);
        if (used + need > rows) { if (first == last) { used = rows; } break; }
        used += need;
        --first;
    }
    ++first;
    int yy = y + (rows - used) * FONT_H;    /* newest line sits on the bottom row */
    if (a->nlines == 0) {
        gfx_text(s, x + 4, y + FONT_H, a->mode == MODE_WSPR ? (a->cfg.wspr_rx ? "WSPR: spots every 2 min   ^W beacon" : "WSPR RX off   ^W beacon") : "listening...", C_FAINT, 1);
        return;
    }
    for (int i = first; i <= last; ++i) {
        const rxline_t *l = &a->lines[i];
        int nr = wrap_rows(l->text, room, 4);
        if (yy + nr * FONT_H > y + rows * FONT_H) nr = (y + rows * FONT_H - yy) / FONT_H;
        if (nr <= 0) break;
        if (i == a->sel) {
            gfx_fill(s, x, yy - 1, w, FONT_H * nr, C_PANEL2);
            gfx_vline(s, x, yy - 1, FONT_H * nr, C_CYAN);
        }
        char pre[40];
        if (wide) {
            char t[12];
            fmt_utc(l->utc, t, 0);
            if (l->flags & LINE_TX) snprintf(pre, sizeof pre, "%s  TX      %4.0f ", t, l->freq);
            else if (l->flags & LINE_SYS) snprintf(pre, sizeof pre, "%s  **           ", t);
            else snprintf(pre, sizeof pre, "%s %+3d %+4.1f %4.0f ", t, l->snr, l->dt, l->freq);
        } else {
            if (l->flags & LINE_TX) snprintf(pre, sizeof pre, " TX %4.0f ", l->freq);
            else if (l->flags & LINE_SYS) snprintf(pre, sizeof pre, " **      ");
            else snprintf(pre, sizeof pre, "%+3d %4.0f ", l->snr, l->freq);
        }
        gfx_text(s, x + 2, yy, pre, (l->flags & LINE_TX) ? C_MAGENTA : C_FAINT, 1);
        draw_wrapped(s, x + 2 + prew * FONT_W, yy, l->text, room, nr, line_color(l));
        yy += nr * FONT_H;
    }
}

static void draw_input(app_t *a, surface_t *s, int x, int y, int w)
{
    gfx_fill(s, x, y, w, FONT_H + 2, C_PANEL);
    int cols = w / FONT_W - 3;
    if (a->input_len || a->status_until < clk_mono()) {
        int xx = gfx_text(s, x + 2, y + 2, ">", C_MAGENTA, 1) + 2;
        const char *t = a->input;
        if (a->input_len > cols) t += a->input_len - cols;
        xx = gfx_text_n(s, xx, y + 2, t, cols, C_TEXT, 1);
        if (((int)(clk_mono() * 2)) & 1) gfx_fill(s, xx, y + 1, 5, 8, C_CYAN);
        if (!a->input_len) {
            const char *hint = a->mode == MODE_FT8 ? "ENT reply  ^Q CQ  TAB mode  ESC setup"
                             : a->mode == MODE_JS8 ? "type msg  ^E HB  ^Q CQ  ESC setup"
                                                   : "^W beacon  ^U tune  ESC setup";
            gfx_text_n(s, xx + 8, y + 2, hint, cols - 3, C_FAINT, 1);
        }
    } else {
        gfx_text_n(s, x + 2, y + 2, a->status, cols + 2, C_SUN, 1);
    }
}

/* ------------------------------------------------------------------ */
/* compact 320x170                                                      */
/* ------------------------------------------------------------------ */

static void render_compact_main(app_t *a, surface_t *s)
{
    char buf[64];
    int nb;
    const band_t *bt = band_table(&nb);
    gfx_clear(s, C_BG);
    /* header */
    int x = gfx_chip(s, 0, 1, mode_name(a->mode), C_MAGENTA, C_BG, 1);
    if (a->mode == MODE_JS8) x = gfx_chip(s, x + 2, 1, sub_short(a->js8_sub), C_PURPLE, C_BG, 1);
    if (a->mode == MODE_JS8 && a->cfg.ghostnet) x = gfx_chip(s, x + 2, 1, "GN", C_MAGENTA, C_BG, 1);
    if (a->mode == MODE_JS8 && a->cfg.js8_autoreply) x = gfx_chip(s, x + 2, 1, "A", C_MINT, C_BG, 1);
    x = gfx_text(s, x + 4, 2, bt[a->band].name, C_CYAN, 1);
    fmt_freq(a->dial_hz, buf);
    gfx_text(s, x + 6, 2, buf, C_SUN, 1);
    fmt_utc(clk_now(), buf, 1);
    gfx_text(s, s->w - gfx_text_w(buf, 1) - 1, 2, buf, C_TEXT, 1);
    int gx = s->w - gfx_text_w(buf, 1) - 26;
    if (a->tx_active) gfx_chip(s, gx - 18, 1, "TX", C_RED, C_BG, 1);
    gfx_chip(s, gx, 1, clk_source() == CLK_GPS ? "GPS" : "SYS", clk_locked() ? C_MINT : C_ORANGE, C_BG, 1);
    gfx_hgrad(s, 0, 11, s->w, 1, C_MAGENTA, C_CYAN);

    draw_waterfall(a, s, 0, 12, s->w, 42);
    draw_period_bar(a, s, 0, 55, s->w, 2);
    draw_list(a, s, 0, 59, s->w, 11, 0);
    draw_input(a, s, 0, s->h - FONT_H - 2, s->w);
}

/* ------------------------------------------------------------------ */
/* large 640x360                                                        */
/* ------------------------------------------------------------------ */

static void meter(surface_t *s, int x, int y, int w, const char *label, float v, uint16_t c, const char *val)
{
    gfx_text(s, x, y, label, C_DIM, 1);
    int bx = x + 30, bw = w - 30 - 40;
    gfx_fill(s, bx, y + 1, bw, 6, C_PANEL2);
    int f = (int)(fmax(0, fmin(1, v)) * bw);
    gfx_hgrad(s, bx, y + 1, f, 6, C_PURPLE, c);
    gfx_text(s, bx + bw + 4, y, val, C_TEXT, 1);
}

static void render_large_main(app_t *a, surface_t *s)
{
    char buf[96];
    int nb;
    const band_t *bt = band_table(&nb);
    gfx_clear(s, C_BG);

    /* header with sun logo */
    gfx_fill(s, 0, 0, s->w, 28, C_PANEL);
    draw_sun(s, 20, 24, 14);
    gfx_text(s, 40, 4, "QSO", C_MAGENTA, 2);
    gfx_text(s, 40 + 3 * 12, 4, "ZERO", C_CYAN, 2);
    int x = gfx_chip(s, 128, 5, mode_name(a->mode), C_MAGENTA, C_BG, 2);
    if (a->mode == MODE_JS8) x = gfx_chip(s, x + 4, 5, sub_short(a->js8_sub), C_PURPLE, C_BG, 2);
    if (a->mode == MODE_JS8 && a->cfg.ghostnet) x = gfx_chip(s, x + 4, 5, "GHOSTNET", C_MAGENTA, C_BG, 2);
    if (a->mode == MODE_JS8 && a->cfg.js8_autoreply) x = gfx_chip(s, x + 4, 5, "AUTO", C_MINT, C_BG, 2);
    x = gfx_text(s, x + 8, 7, bt[a->band].name, C_CYAN, 2);
    fmt_freq(a->dial_hz, buf);
    gfx_text(s, x + 10, 7, buf, C_SUN, 2);
    fmt_utc(clk_now(), buf, 1);
    gfx_text(s, s->w - gfx_text_w(buf, 2) - 6, 7, buf, C_TEXT, 2);
    gfx_text(s, s->w - gfx_text_w(buf, 2) - 30, 11, "UTC", C_DIM, 1);
    gfx_hgrad(s, 0, 28, s->w, 2, C_MAGENTA, C_CYAN);

    /* waterfall */
    int wx = 8, wy = 36, ww = 320, wh = 112;
    gfx_rect(s, wx - 1, wy - 1, ww + 2, wh + 2, C_GRID);
    draw_waterfall(a, s, wx, wy, ww, wh);
    for (int f = 500; f < 3000; f += 500) {
        int px = wx + f * ww / 3000;
        snprintf(buf, sizeof buf, "%d", f);
        gfx_text(s, px - gfx_text_w(buf, 1) / 2, wy + wh + 3, buf, C_FAINT, 1);
    }
    draw_period_bar(a, s, wx, wy + wh + 13, ww, 3);

    /* station panel */
    int px = 340, py = 36, pw = 292;
    gfx_fill(s, px, py - 1, pw, 132, C_PANEL);
    gfx_text(s, px + 6, py + 4, a->cfg.callsign, C_CYAN, 2);
    gfx_text(s, px + 6 + gfx_text_w(a->cfg.callsign, 2) + 10, py + 8, a->cfg.grid, C_SUN, 1);
    gfx_text(s, px + 6, py + 24, a->radio->name, C_TEXT, 1);
    snprintf(buf, sizeof buf, "RIG %s  PTT %s", a->rig_ok ? "ONLINE" : "OFFLINE", ptt_name(a->radio->ptt));
    gfx_text(s, px + 6, py + 34, buf, a->rig_ok ? C_MINT : C_ORANGE, 1);
    gps_state_t g;
    gps_get(&g);
    if (clk_source() == CLK_GPS)
        snprintf(buf, sizeof buf, "GPS %s %d sats  clk %+.3fs", g.fix ? "FIX" : "NOFIX", g.sats, clk_offset());
    else
        snprintf(buf, sizeof buf, "CLOCK: system (no GPS time)");
    gfx_text(s, px + 6, py + 44, buf, clk_locked() ? C_MINT : C_ORANGE, 1);

    float lvl = audio_rx_level();
    snprintf(buf, sizeof buf, "%3.0f%%", lvl * 100);
    meter(s, px + 6, py + 58, pw - 12, "RX", lvl / 0.7f, lvl > 0.7f ? C_RED : C_MINT, buf);
    snprintf(buf, sizeof buf, "%3.0f%%", a->cfg.tx_level * 100);
    meter(s, px + 6, py + 68, pw - 12, "TXL", a->cfg.tx_level, C_MAGENTA, buf);
    snprintf(buf, sizeof buf, "%.1f", a->swr);
    meter(s, px + 6, py + 78, pw - 12, "SWR", a->tx_active ? (a->swr - 1) / 2 : 0, a->swr > 2 ? C_RED : C_CYAN, a->tx_active ? buf : "--");

    const char *seqn[] = {"idle", "calling CQ", "calling", "sent report", "sent R-report", "sent RR73", "sent 73"};
    if (a->mode == MODE_FT8 && a->seq.st != SEQ_IDLE)
        snprintf(buf, sizeof buf, "QSO %s: %s (try %d)", a->seq.dx[0] ? a->seq.dx : "-", seqn[a->seq.st], a->seq.tries);
    else if (a->mode == MODE_WSPR)
        snprintf(buf, sizeof buf, "WSPR beacon %s  %d dBm  %d%%", a->wspr_beacon ? "ON" : "off", wspr_valid_dbm(a->cfg.wspr_dbm), a->cfg.wspr_tx_pct);
    else if (a->mode == MODE_JS8 && a->cfg.js8_autoreply)
        snprintf(buf, sizeof buf, "AUTO-REPLY on%s  sent %d  queue %d", a->cfg.js8_hb_ack ? " +HB ACK" : "", a->autoreplies, a->ntxq);
    else
        snprintf(buf, sizeof buf, "TX queue: %d", a->ntxq);
    gfx_text(s, px + 6, py + 94, buf, a->ntxq ? C_PINK : C_DIM, 1);
    snprintf(buf, sizeof buf, "RX %4d Hz   TX %4d Hz   decode %.0f ms", a->rx_off, a->tx_off, a->last_decode_ms);
    gfx_text(s, px + 6, py + 106, buf, C_DIM, 1);
    if (a->mode == MODE_WSPR && a->cfg.wsprnet_upload) {
        uplink_stats_t u;
        uplink_get(&u);
        snprintf(buf, sizeof buf, "wsprnet: %s  sent %d%s  dropped %d", u.last_check == 0 ? "checking" : u.online ? "ONLINE" : "offline",
                 u.sent_https + u.sent_http, u.sent_http ? " (http)" : u.sent_https ? " (https)" : "", u.dropped);
        gfx_text(s, px + 6, py + 118, buf, u.online ? C_MINT : C_DIM, 1);
    }
    if (a->tx_active) gfx_chip(s, px + pw - 40, py + 4, "TX", C_RED, C_BG, 2);
    else if (a->decoding) gfx_chip(s, px + pw - 52, py + 4, "DEC", C_PURPLE, C_BG, 2);

    /* decode table */
    int ty = 176;
    gfx_hgrad(s, 8, ty - 4, s->w - 16, 1, C_GRID, C_BG);
    gfx_text(s, 10, ty, "UTC    SNR  DT  FREQ MESSAGE", C_FAINT, 1);
    draw_list(a, s, 8, ty + 11, s->w - 16, 16, 1);
    draw_input(a, s, 0, s->h - FONT_H - 3, s->w);
}

/* ------------------------------------------------------------------ */
/* menu / tools / help                                                  */
/* ------------------------------------------------------------------ */

static const char *dev_root(void);
static void open_picker(app_t *a, int menu_item);
static void render_picker(app_t *a, surface_t *s, int big);

enum { MI_MODE, MI_SUB, MI_BAND, MI_QSY, MI_GHOST, MI_GN80, MI_GN40, MI_GN20, MI_CALL, MI_GRID, MI_RADIO, MI_PORT, MI_AIN, MI_AOUT, MI_TXL, MI_AUTO, MI_HBACK, MI_INFO, MI_STATUS, MI_WRX, MI_WPWR, MI_WPCT, MI_AGRID, MI_LAYOUT, MI_TUNE, MI_SAVE, MI_QUIT, MI_COUNT };

static void menu_value(app_t *a, int i, char *out)
{
    int nb, nr;
    const band_t *bt = band_table(&nb);
    const radio_profile_t *rl = radio_list(&nr);
    (void)rl;
    switch (i) {
    case MI_MODE: strcpy(out, mode_name(a->mode)); break;
    case MI_SUB: strcpy(out, js8_submode_info((js8_submode_t)a->js8_sub)->name); break;
    case MI_BAND: strcpy(out, bt[a->band].name); break;
    case MI_CALL: strcpy(out, a->cfg.callsign); break;
    case MI_GRID: strcpy(out, a->cfg.grid); break;
    case MI_RADIO: strcpy(out, a->radio->name); break;
    case MI_PORT: {
        char n[48];
        dev_short_name(dev_root(), a->cfg.rig_device, n, sizeof n);
        if (a->cfg.rig_device[0]) snprintf(out, 64, "%s", n);
        else snprintf(out, 64, "auto (%s)", a->radio->serial_default + 5);
        break;
    }
    case MI_AIN: case MI_AOUT: {
        const char *v = i == MI_AIN ? a->cfg.audio_in : a->cfg.audio_out;
        char n[48];
        dev_short_name(dev_root(), v, n, sizeof n);
        if (v[0]) snprintf(out, 64, "%s", n);
        else if (i == MI_AOUT) snprintf(out, 64, "same as input");
        else { dev_short_name(dev_root(), a->audio_in_dev, n, sizeof n); snprintf(out, 64, a->audio_in_dev[0] ? "auto (%s)" : "auto", n); }
        break;
    }
    case MI_TXL: sprintf(out, "%.0f%%", a->cfg.tx_level * 100); break;
    case MI_WPWR: { int d = wspr_valid_dbm(a->cfg.wspr_dbm); double mw = pow(10, d / 10.0);
                    if (mw >= 1000) sprintf(out, "%d dBm (%.0f W)", d, mw / 1000); else sprintf(out, "%d dBm (%.0f mW)", d, mw); } break;
    case MI_WPCT: sprintf(out, "%d%%", a->cfg.wspr_tx_pct); break;
    case MI_AGRID: strcpy(out, a->cfg.auto_grid ? "ON" : "off"); break;
    case MI_QSY: strcpy(out, a->cfg.auto_qsy ? "ON" : "off"); break;
    case MI_GHOST: strcpy(out, a->cfg.ghostnet ? (a->mode == MODE_JS8 ? "ON" : "ON (in JS8)") : "off"); break;
    case MI_GN80: case MI_GN40: case MI_GN20: {
        int b = i == MI_GN80 ? 1 : i == MI_GN40 ? 3 : 5;
        int here = a->cfg.ghostnet && a->mode == MODE_JS8 && a->band == b;
        strcpy(out, !(a->band_mask & (1u << b)) ? "not on this radio" : here ? "USB-D  < tuned" : "USB-D");
        break;
    }
    case MI_AUTO: strcpy(out, a->cfg.js8_autoreply ? "ON" : "off"); break;
    case MI_HBACK: strcpy(out, !a->cfg.js8_autoreply ? "(needs auto)" : a->cfg.js8_hb_ack ? "ON" : "off"); break;
    case MI_INFO: snprintf(out, 64, "%.20s", a->cfg.js8_info[0] ? a->cfg.js8_info : "(none)"); break;
    case MI_STATUS: snprintf(out, 64, "%.20s", a->cfg.js8_status[0] ? a->cfg.js8_status : "(none)"); break;
    case MI_WRX: strcpy(out, a->cfg.wspr_rx ? "ON" : "off"); break;
    case MI_LAYOUT: strcpy(out, a->large == 0 ? "auto" : a->large == 1 ? "large" : "compact"); break;
    default: out[0] = 0;
    }
}

static const char *k_menu_names[MI_COUNT] = {"Mode", "JS8 speed", "Band", "Auto frequency", "GHOSTNET", "  80m  3.575", "  40m  7.107", "  20m 14.107", "Callsign", "Grid", "Radio", "CAT port", "Audio in", "Audio out", "TX audio level",
                                            "JS8 auto-reply", "JS8 heartbeat ACK", "JS8 INFO text", "JS8 STATUS text",
                                            "WSPR receive", "WSPR power", "WSPR TX %", "Grid from GPS", "Layout", "Tune 5 s",
                                            "Save settings", "Quit"};

static void render_menu(app_t *a, surface_t *s, int sc_large)
{
    gfx_clear(s, C_BG);
    int big = sc_large;
    gfx_fill(s, 0, 0, s->w, big ? 26 : 12, C_PANEL);
    if (big) draw_sun(s, 20, 22, 12);
    gfx_text(s, big ? 40 : 2, big ? 5 : 2, "SETUP", C_MAGENTA, big ? 2 : 1);
    gfx_text(s, s->w - gfx_text_w("ESC back", 1) - 4, big ? 9 : 2, "ESC back", C_DIM, 1);
    gfx_hgrad(s, 0, big ? 26 : 12, s->w, 1, C_MAGENTA, C_CYAN);
    int y0 = big ? 36 : 15, rowh = big ? 20 : 11, sc = big ? 2 : 1;
    int vis = (s->h - y0 - 12) / rowh;
    int first = a->menu_sel - vis + 1;
    if (first < 0) first = 0;
    char v[64];
    for (int i = first; i < MI_COUNT && i < first + vis; ++i) {
        int y = y0 + (i - first) * rowh;
        if (i == a->menu_sel) { gfx_fill(s, 0, y - 1, s->w, rowh - 1, C_PANEL2); gfx_fill(s, 0, y - 1, 3 * sc, rowh - 1, C_MAGENTA); }
        uint16_t nc = i == a->menu_sel ? C_TEXT : i == MI_GHOST ? C_MAGENTA : C_DIM;
        gfx_text(s, 8 * sc, y, k_menu_names[i], nc, sc);
        menu_value(a, i, v);
        int vx = s->w - gfx_text_w(v, sc) - 6 * sc;
        if (v[0]) gfx_text(s, vx, y, v, i == a->menu_sel ? C_SUN : C_CYAN, sc);
    }
    const char *hint = (a->menu_sel == MI_RADIO || a->menu_sel == MI_PORT || a->menu_sel == MI_AIN || a->menu_sel == MI_AOUT)
                         ? "ENTER: choose from what's connected"
                     : (a->menu_sel == MI_CALL || a->menu_sel == MI_GRID || a->menu_sel == MI_INFO || a->menu_sel == MI_STATUS) ? "ENTER to edit"
                     : a->menu_sel == MI_QSY ? "tune the radio to each mode's frequency"
                     : a->menu_sel == MI_GHOST ? "JS8 on the GHOSTNET frequencies"
                     : (a->menu_sel >= MI_GN80 && a->menu_sel <= MI_GN20) ? "ENTER: JS8 here (GHOSTNET)"
                     : a->menu_sel == MI_AUTO ? "answers SNR? GRID? INFO? STATUS? HEARING? AGN? MSG"
                     : a->menu_sel == MI_HBACK ? "ACK heartbeats, max 1 per station / 15 min"
                                                                          : "</> change  ENTER select";
    gfx_text(s, 4, s->h - 10, hint, C_FAINT, 1);
}

static void render_tools(app_t *a, surface_t *s, int big)
{
    char b[96];
    gfx_clear(s, C_BG);
    gfx_fill(s, 0, 0, s->w, 12, C_PANEL);
    gfx_text(s, 2, 2, "TOOLS", C_CYAN, 1);
    gfx_text(s, s->w - gfx_text_w("ESC back", 1) - 4, 2, "ESC back", C_DIM, 1);
    gfx_hgrad(s, 0, 12, s->w, 1, C_CYAN, C_MAGENTA);
    int y = 16, sc = big ? 2 : 1, lh = 10 * sc;
    gps_state_t g;
    gps_get(&g);
    gfx_text(s, 4, y, "TIME / GPS", C_MAGENTA, sc); y += lh;
    snprintf(b, sizeof b, "source %s  %s  offset %+.3f s", clk_source() == CLK_GPS ? "GPS" : "system",
             clk_locked() ? "LOCKED" : "unlocked", clk_offset());
    gfx_text(s, 4, y, b, clk_locked() ? C_MINT : C_ORANGE, 1); y += 10;
    snprintf(b, sizeof b, "fix %s  sats %d  %.4f %.4f  %s", g.fix ? "yes" : "no", g.sats, g.lat, g.lon, g.grid[0] ? g.grid : "------");
    gfx_text(s, 4, y, b, C_TEXT, 1); y += 14;
    gfx_text(s, 4, y, "RADIO", C_MAGENTA, sc); y += lh;
    snprintf(b, sizeof b, "%s  hamlib %d  %s", a->radio->name, a->radio->hamlib_model, a->rig_ok ? "online" : rig_error());
    gfx_text_n(s, 4, y, b, s->w / FONT_W - 1, a->rig_ok ? C_MINT : C_ORANGE, 1); y += 10;
    {
        int nb, n = 0;
        const band_t *bt = band_table(&nb);
        n = snprintf(b, sizeof b, "bands:");
        for (int i = 0; i < nb && n < (int)sizeof b - 6; ++i)
            if (app_bands(a) & (1u << i)) n += snprintf(b + n, sizeof b - n, " %.*s", (int)strlen(bt[i].name) - 1, bt[i].name);
        snprintf(b + n, sizeof b - n, "   auto QSY %s", a->cfg.auto_qsy ? "on" : "off");
        gfx_text_n(s, 4, y, b, s->w / FONT_W - 1, C_TEXT, 1); y += 10;
    }
    {
        char p[48];
        dev_short_name(dev_root(), a->cfg.rig_device[0] ? a->cfg.rig_device : a->radio->serial_default, p, sizeof p);
        snprintf(b, sizeof b, "CAT %s   audio in %s  out %s", p, a->audio_in_dev[0] ? a->audio_in_dev : "-", a->audio_out_dev[0] ? a->audio_out_dev : "-");
        gfx_text_n(s, 4, y, b, s->w / FONT_W - 1, C_TEXT, 1); y += 10;
    }
    y += draw_wrapped(s, 4, y, a->radio->notes, s->w / FONT_W - 1, 3, C_DIM) * FONT_H + 4;
    gfx_text(s, 4, y, "AUDIO / DSP", C_MAGENTA, sc); y += lh;
    snprintf(b, sizeof b, "RX peak %.0f%%  last decode %.0f ms  TX level %.0f%%", audio_rx_level() * 100, a->last_decode_ms, a->cfg.tx_level * 100);
    gfx_text(s, 4, y, b, C_TEXT, 1); y += 14;
    {
        uplink_stats_t u;
        uplink_get(&u);
        if (!a->cfg.wsprnet_upload) snprintf(b, sizeof b, "NET  wsprnet upload off");
        else snprintf(b, sizeof b, "NET  %s (every 5 min)  https %d  http %d  dropped %d", u.last_check == 0 ? "not checked yet" : u.online ? "ONLINE" : "offline",
                      u.sent_https, u.sent_http, u.dropped);
        gfx_text(s, 4, y, b, u.online ? C_MINT : C_DIM, 1); y += 10;
        if (u.last_error[0]) { gfx_text_n(s, 4, y, u.last_error, s->w / FONT_W - 1, C_ORANGE, 1); y += 10; }
        y += 4;
    }
    gfx_text(s, 4, y, "U tune 5s   W WSPR beacon   X halt TX   +/- TX level", C_CYAN, 1);
}

static void render_help(app_t *a, surface_t *s)
{
    (void)a;
    static const char *lines[] = {
        "KEYS            hold ESC: exit",
        "ENTER   send / reply to selected",
        "UP/DN   select decode   L/R move RX+TX",
        "TAB     next mode (FT8 JS8 WSPR)",
        "PGUP/DN band            ESC setup",
        "^Q CQ   ^E JS8 heartbeat   ^X HALT TX",
        "^U tune  ^W WSPR beacon  ^T tools",
        "^L layout compact/large",
        "/band 40  /mode js8  /sub fast  /qsy on|off",
        "/ghost 80|40|20|off  GHOSTNET JS8",
        "/freq 7.074  /call K1ABC  /grid FN42",
        "/level 40  /pwr 23  /save  /quit",
        "/auto on|off  /hback on|off",
        "/info TEXT  /status TEXT  /net",
    };
    gfx_clear(s, C_BG);
    int sc = s->w >= 640 ? 2 : 1;
    for (size_t i = 0; i < sizeof lines / sizeof *lines; ++i)
        gfx_text(s, 6, 4 + (int)i * 10 * sc + (i ? 4 : 0), lines[i], i ? C_TEXT : C_MAGENTA, sc);
}

static void draw_exit_toast(app_t *a, surface_t *s)
{
    int sc = s->w >= LARGE_W ? 2 : 1;
    const char *t = a->tx_active ? "Hold ESC to exit (stops TX)" : "Hold ESC to exit";
    int w = gfx_text_w(t, sc) + 16 * sc, h = 22 * sc;
    int x = (s->w - w) / 2, y = (s->h - h) / 2;
    gfx_fill(s, x, y, w, h, C_PANEL2);
    gfx_rect(s, x, y, w, h, C_MAGENTA);
    gfx_text(s, x + 8 * sc, y + 4 * sc, t, C_TEXT, sc);
    double f = (clk_mono() - a->esc_t0) / ESC_HOLD_S;
    if (f > 1) f = 1;
    gfx_fill(s, x + 8 * sc, y + 15 * sc, w - 16 * sc, 3 * sc, C_PANEL);
    gfx_hgrad(s, x + 8 * sc, y + 15 * sc, (int)((w - 16 * sc) * f), 3 * sc, C_PURPLE, C_MAGENTA);
}

void ui_render(app_t *a, surface_t *s)
{
    int big = s->w >= LARGE_W;
    switch (a->screen) {
    case 1: render_menu(a, s, big); break;
    case 2: render_tools(a, s, 0); break;
    case 3: render_help(a, s); break;
    case 4: render_picker(a, s, big); break;
    default: if (big) render_large_main(a, s); else render_compact_main(a, s);
    }
    if (a->esc_t0 > 0) draw_exit_toast(a, s);
}

/* ------------------------------------------------------------------ */
/* keys                                                                 */
/* ------------------------------------------------------------------ */

/* ---- device picker (screen 4): radio, CAT port, audio in, audio out ---- */

static const char *dev_root(void)
{
    const char *r = getenv("QSOZERO_DEVROOT");     /* tests / screenshots only */
    return r ? r : "";
}

static const char *k_pick_title[] = {"RADIO", "CAT PORT", "AUDIO IN", "AUDIO OUT"};

static const char *pick_current(app_t *a)
{
    switch (a->pick_kind) {
    case 0: return a->radio->id;
    case 1: return a->cfg.rig_device;
    case 2: return a->cfg.audio_in;
    default: return a->cfg.audio_out;
    }
}

static void fill_picker(app_t *a)
{
    char d[96];
    switch (a->pick_kind) {
    case 0: {
        int n;
        const radio_profile_t *rl = radio_list(&n);
        a->pick_n = 0;
        for (int i = 0; i < n && i < DEV_MAX; ++i, ++a->pick_n) {
            dev_opt_t *o = &a->pick[i];
            snprintf(o->value, sizeof o->value, "%s", rl[i].id);
            snprintf(o->label, sizeof o->label, "%s", rl[i].name);
            snprintf(o->desc, sizeof o->desc, "%s, %s PTT", rl[i].serial_default, ptt_name(rl[i].ptt));
        }
        break;
    }
    case 1:
        snprintf(d, sizeof d, "%s default: %s", a->radio->name, a->radio->serial_default);
        a->pick_n = dev_serial_list(dev_root(), d, a->pick, DEV_MAX);
        break;
    case 2:
        snprintf(d, sizeof d, "the card that matches the %s", a->radio->name);
        a->pick_n = dev_audio_list(dev_root(), 1, d, a->pick, DEV_MAX);
        break;
    default:
        a->pick_n = dev_audio_list(dev_root(), 0, "the same card as Audio in", a->pick, DEV_MAX);
    }
    /* put the cursor on the current choice */
    const char *cur = pick_current(a);
    a->pick_sel = 0;
    for (int i = 0; i < a->pick_n; ++i)
        if (!strcmp(a->pick[i].value, cur) || (cur[0] && !strcmp(a->pick[i].label, cur))) a->pick_sel = i;
}

static void open_picker(app_t *a, int menu_item)
{
    a->pick_kind = menu_item == MI_RADIO ? 0 : menu_item == MI_PORT ? 1 : menu_item == MI_AIN ? 2 : 3;
    fill_picker(a);
    a->screen = 4;
}

static void picker_apply(app_t *a)
{
    const char *v = a->pick[a->pick_sel].value;
    switch (a->pick_kind) {
    case 0: app_use_radio(a, v); break;
    case 1: app_use_rig_device(a, v); break;
    case 2: app_use_audio(a, v, NULL); break;
    default: app_use_audio(a, NULL, v);
    }
    a->screen = 1;
}

static void render_picker(app_t *a, surface_t *s, int big)
{
    int sc = big ? 2 : 1;
    gfx_clear(s, C_BG);
    gfx_fill(s, 0, 0, s->w, 12 * sc, C_PANEL);
    gfx_text(s, 2 * sc, 2 * sc, k_pick_title[a->pick_kind], C_MAGENTA, sc);
    gfx_text(s, s->w - gfx_text_w("ESC back", 1) - 4, 2 * sc, "ESC back", C_DIM, 1);
    gfx_hgrad(s, 0, 12 * sc, s->w, 1, C_MAGENTA, C_CYAN);
    if (a->pick_kind > 0 && a->pick_n <= (a->pick_kind == 1 ? 1 : 2)) {
        gfx_text(s, 8, 18 * sc, a->pick_kind == 1 ? "no USB serial ports found" : "no sound cards found", C_ORANGE, sc);
        gfx_text(s, 8, 30 * sc, "plug the radio in, then press R", C_DIM, sc);
    }
    int y0 = 16 * sc, rowh = 21 * sc;
    int vis = (s->h - y0 - 12) / rowh;
    int first = a->pick_sel - vis + 1;
    if (first < 0) first = 0;
    const char *cur = pick_current(a);
    int cols = s->w / (FONT_W * sc) - 3;
    for (int i = first; i < a->pick_n && i < first + vis; ++i) {
        const dev_opt_t *o = &a->pick[i];
        int y = y0 + (i - first) * rowh;
        int is_cur = !strcmp(o->value, cur) || (cur[0] && !strcmp(o->label, cur));
        if (i == a->pick_sel) { gfx_fill(s, 0, y - 1, s->w, rowh - 2, C_PANEL2); gfx_fill(s, 0, y - 1, 3 * sc, rowh - 2, C_MAGENTA); }
        gfx_text_n(s, 8 * sc, y, o->label, cols - 10, i == a->pick_sel ? C_TEXT : C_CYAN, sc);
        if (is_cur) gfx_text(s, s->w - gfx_text_w("in use", sc) - 6 * sc, y, "in use", C_MINT, sc);
        gfx_text_n(s, 8 * sc, y + 10 * sc, o->desc, cols, C_DIM, sc);
    }
    gfx_text(s, 4, s->h - 10, "ENTER use  R rescan  then Setup > Save settings", C_FAINT, 1);
}

static void menu_change(app_t *a, int dir)
{
    int nb;
    band_table(&nb);
    switch (a->menu_sel) {
    case MI_MODE: app_set_mode(a, (op_mode_t)((a->mode + MODE_COUNT + dir) % MODE_COUNT)); break;
    case MI_SUB: a->js8_sub = (a->js8_sub + 4 + dir) % 4; break;
    case MI_BAND: app_step_band(a, dir); break;
    case MI_RADIO: case MI_PORT: case MI_AIN: case MI_AOUT: open_picker(a, a->menu_sel); break;
    case MI_TXL: a->cfg.tx_level = (float)fmax(0.05, fmin(1.0, a->cfg.tx_level + dir * 0.05)); break;
    case MI_WPWR: {
        static const int v[] = {0,3,7,10,13,17,20,23,27,30,33,37,40,43,47};
        int k = 0;
        for (int i = 0; i < 15; ++i) if (v[i] == wspr_valid_dbm(a->cfg.wspr_dbm)) k = i;
        k = k + dir < 0 ? 0 : k + dir > 14 ? 14 : k + dir;
        a->cfg.wspr_dbm = v[k];
        break;
    }
    case MI_WPCT: a->cfg.wspr_tx_pct = (int)fmax(0, fmin(100, a->cfg.wspr_tx_pct + dir * 10)); break;
    case MI_AGRID: a->cfg.auto_grid = !a->cfg.auto_grid; break;
    case MI_GHOST: app_set_ghostnet(a, !a->cfg.ghostnet); break;
    case MI_GN80: app_ghostnet_band(a, 1); break;
    case MI_GN40: app_ghostnet_band(a, 3); break;
    case MI_GN20: app_ghostnet_band(a, 5); break;
    case MI_QSY:
        a->cfg.auto_qsy = !a->cfg.auto_qsy;
        if (a->cfg.auto_qsy) app_set_band(a, a->band);    /* snap to the mode frequency now */
        break;
    case MI_AUTO: a->cfg.js8_autoreply = !a->cfg.js8_autoreply; break;
    case MI_HBACK: a->cfg.js8_hb_ack = !a->cfg.js8_hb_ack; break;
    case MI_WRX: a->cfg.wspr_rx = !a->cfg.wspr_rx; break;
    case MI_LAYOUT: a->large = (a->large + 3 + dir) % 3; break;
    }
}

static void menu_enter(app_t *a)
{
    switch (a->menu_sel) {
    case MI_CALL: snprintf(a->input, sizeof a->input, "/call "); a->input_len = 6; a->screen = 0; break;
    case MI_GRID: snprintf(a->input, sizeof a->input, "/grid "); a->input_len = 6; a->screen = 0; break;
    case MI_INFO: snprintf(a->input, sizeof a->input, "/info "); a->input_len = 6; a->screen = 0; break;
    case MI_STATUS: snprintf(a->input, sizeof a->input, "/status "); a->input_len = 8; a->screen = 0; break;
    case MI_TUNE: a->screen = 0; app_tune(a, 5); break;
    case MI_SAVE: app_status(a, config_save(&a->cfg, config_default_path()) ? "save failed" : "settings saved"); a->screen = 0; break;
    case MI_QUIT: a->quit = 1; break;
    default: menu_change(a, 1);
    }
}

void ui_key(app_t *a, const keyev_t *k)
{
    a->dirty = 1;
    /* launcher convention: hold ESC to exit (from any screen) */
    if (k->key == K_ESC_HOLDING) { a->esc_t0 = clk_mono(); return; }
    if (k->key == K_ESC_HOLD) { a->esc_t0 = 0; a->quit = 1; return; }
    if (k->key == K_ESC) a->esc_t0 = 0;
    int ctrl = k->mods & MOD_CTRL;
    /* global hot keys */
    if (ctrl && k->key == K_CHAR) {
        switch (k->ch) {
        case 'x': app_halt_tx(a); return;
        case 'l': a->large = a->large == 1 ? 2 : 1; return;
        case 't': a->screen = a->screen == 2 ? 0 : 2; return;
        case 'u': app_tune(a, 5); return;
        case 'w': if (a->mode != MODE_WSPR) app_set_mode(a, MODE_WSPR); app_toggle_wspr(a); return;
        case 'q': app_call_cq(a); return;
        case 'e': app_send_heartbeat(a); return;
        case 'r': app_reply_selected(a); return;
        case 'n': app_step_band(a, 1); return;
        case 'p': app_step_band(a, -1); return;
        /* Cardputer has no arrow keys: Ctrl + ; . , / are the printed arrows */
        case ';': { keyev_t e = {K_UP, 0, 0}; ui_key(a, &e); return; }
        case '.': { keyev_t e = {K_DOWN, 0, 0}; ui_key(a, &e); return; }
        case ',': { keyev_t e = {K_LEFT, 0, 0}; ui_key(a, &e); return; }
        case '/': { keyev_t e = {K_RIGHT, 0, 0}; ui_key(a, &e); return; }
        }
    }
    if (k->key == K_F1) { a->screen = a->screen == 3 ? 0 : 3; return; }

    if (a->screen == 1) {
        switch (k->key) {
        case K_ESC: a->screen = 0; break;
        case K_UP: a->menu_sel = (a->menu_sel + MI_COUNT - 1) % MI_COUNT; break;
        case K_DOWN: a->menu_sel = (a->menu_sel + 1) % MI_COUNT; break;
        case K_LEFT: menu_change(a, -1); break;
        case K_RIGHT: menu_change(a, 1); break;
        case K_ENTER: menu_enter(a); break;
        }
        return;
    }
    if (a->screen == 2) {
        if (k->key == K_ESC) a->screen = 0;
        else if (k->key == K_CHAR) {
            switch (k->ch) {
            case 'u': case 'U': app_tune(a, 5); break;
            case 'w': case 'W': if (a->mode != MODE_WSPR) app_set_mode(a, MODE_WSPR); app_toggle_wspr(a); break;
            case 'x': case 'X': app_halt_tx(a); break;
            case '+': case '=': a->cfg.tx_level = (float)fmin(1.0, a->cfg.tx_level + 0.05); break;
            case '-': a->cfg.tx_level = (float)fmax(0.05, a->cfg.tx_level - 0.05); break;
            }
        }
        return;
    }
    if (a->screen == 3) { a->screen = 0; return; }
    if (a->screen == 4) {
        switch (k->key) {
        case K_ESC: a->screen = 1; break;
        case K_UP: if (a->pick_sel > 0) --a->pick_sel; break;
        case K_DOWN: if (a->pick_sel + 1 < a->pick_n) ++a->pick_sel; break;
        case K_ENTER: if (a->pick_n) picker_apply(a); break;
        case K_CHAR: if (k->ch == 'r' || k->ch == 'R') fill_picker(a); break;
        }
        return;
    }

    /* main screen */
    int step = (k->mods & MOD_SHIFT) ? 100 : 10;
    switch (k->key) {
    case K_CHAR:
        if (a->input_len < (int)sizeof a->input - 1 && k->ch >= 32 && k->ch < 127) {
            a->input[a->input_len++] = (char)k->ch;
            a->input[a->input_len] = 0;
        }
        break;
    case K_BKSP: if (a->input_len) a->input[--a->input_len] = 0; break;
    case K_ENTER: app_submit_input(a); break;
    case K_TAB: app_set_mode(a, (op_mode_t)((a->mode + 1) % MODE_COUNT)); break;
    case K_ESC:
        if (a->input_len) { a->input_len = 0; a->input[0] = 0; }
        else if (a->sel >= 0) a->sel = -1;
        else { a->screen = 1; }
        break;
    case K_UP:
        if (a->nlines) a->sel = a->sel < 0 ? a->nlines - 1 : (a->sel > 0 ? a->sel - 1 : 0);
        break;
    case K_DOWN:
        if (a->sel >= 0) a->sel = a->sel + 1 >= a->nlines ? -1 : a->sel + 1;
        break;
    case K_LEFT: a->rx_off = (int)fmax(200, a->rx_off - step); a->tx_off = a->rx_off; break;
    case K_RIGHT: a->rx_off = (int)fmin(2900, a->rx_off + step); a->tx_off = a->rx_off; break;
    case K_PGUP: app_step_band(a, 1); break;
    case K_PGDN: app_step_band(a, -1); break;
    }
}
