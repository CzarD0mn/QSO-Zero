/* QSO-Zero - engine: audio ring, waterfall, decode and transmit threads. */
#include "app.h"
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include "audio/audio.h"
#include "core/clock.h"
#include "core/log.h"
#include "dsp/ft8w.h"
#include "dsp/js8.h"
#include "dsp/js8_varicode.h"
#include "dsp/wspr.h"
#include "dsp/wsprrx.h"
#include "net/uplink.h"
#include "fft/kiss_fftr.h"
#include "gps/gps.h"
#include "rig/rigctl.h"

#define RING_SECONDS 128   /* a full 2-minute WSPR cycle */
#define WF_NFFT 1280

/* Standard dial frequencies (USB) per mode, and the band edges used to work
 * out which band the radio's VFO is on. FT8/FT4/WSPR: WSJT-X defaults
 * (no FT4 on 160 or 60 m); JS8: JS8Call defaults. Edges cover all three
 * IARU regions. Columns: FT8, JS8, WSPR, low edge, high edge, FT4. */
static const band_t k_bands[] = {
    {"160m",  1840000,  1842000,  1836600,  1800000,  2000000,         0},
    {"80m",   3573000,  3578000,  3568600,  3500000,  4000000,   3575000},
    {"60m",   5357000,  5357000,  5287200,  5250000,  5450000,         0},
    {"40m",   7074000,  7078000,  7038600,  7000000,  7300000,   7047500},
    {"30m",  10136000, 10130000, 10138700, 10100000, 10150000,  10140000},
    {"20m",  14074000, 14078000, 14095600, 14000000, 14350000,  14080000},
    {"17m",  18100000, 18104000, 18104600, 18068000, 18168000,  18104000},
    {"15m",  21074000, 21078000, 21094600, 21000000, 21450000,  21140000},
    {"12m",  24915000, 24922000, 24924600, 24890000, 24990000,  24919000},
    {"10m",  28074000, 28078000, 28124600, 28000000, 29700000,  28180000},
    {"6m",   50313000, 50318000, 50293000, 50000000, 54000000,  50318000},
};

const band_t *band_table(int *count)
{
    *count = (int)(sizeof k_bands / sizeof *k_bands);
    return k_bands;
}

int band_from_freq(int64_t hz)
{
    for (int i = 0; i < (int)(sizeof k_bands / sizeof *k_bands); ++i)
        if (hz >= k_bands[i].lo && hz <= k_bands[i].hi) return i;
    return -1;
}

/* GHOSTNET JS8 frequencies (dial, USB-D), by band_table() index */
static const struct { int band; int64_t hz; } k_ghostnet[] = {
    {1, 3575000},     /* 80 m */
    {3, 7107000},     /* 40 m */
    {5, 14107000},    /* 20 m */
};
#define GHOSTNET_BANDS ((1u << 1) | (1u << 3) | (1u << 5))

int64_t ghostnet_freq(int band)
{
    for (size_t i = 0; i < sizeof k_ghostnet / sizeof *k_ghostnet; ++i)
        if (k_ghostnet[i].band == band) return k_ghostnet[i].hz;
    return 0;
}

static int in_ghostnet(const app_t *a) { return a->mode == MODE_JS8 && a->cfg.ghostnet; }

static int64_t mode_freq(const app_t *a, int band)
{
    const band_t *b = &k_bands[band];
    /* GHOSTNET isn't locked: nets move off frequency to dodge QRM, so use
     * wherever the operator last tuned on this band, else the net default */
    if (in_ghostnet(a) && ghostnet_freq(band)) return a->ghost_hz[band] ? a->ghost_hz[band] : ghostnet_freq(band);
    return a->mode == MODE_FT8 ? b->ft8 : a->mode == MODE_FT4 ? b->ft4 : a->mode == MODE_JS8 ? b->js8 : b->wspr;
}

/* config "bands = 40,30,20" (with or without the m) -> bit mask; 0 = no limit */
static unsigned parse_bands(const char *list)
{
    unsigned m = 0;
    char buf[64];
    snprintf(buf, sizeof buf, "%s", list);
    for (char *t = strtok(buf, ", "); t; t = strtok(NULL, ", "))
        for (int i = 0; i < (int)(sizeof k_bands / sizeof *k_bands); ++i) {
            size_t n = strlen(t);
            if (!strncasecmp(k_bands[i].name, t, n) && (k_bands[i].name[n] == 'm' || k_bands[i].name[n] == 0))
                m |= 1u << i;
        }
    return m;
}

unsigned app_bands(const app_t *a)
{
    unsigned m = a->band_mask & ~a->band_rejected;
    if (!m) m = a->band_mask;
    if (in_ghostnet(a) && (m & GHOSTNET_BANDS)) m &= GHOSTNET_BANDS;   /* band keys cycle 80/40/20 */
    if (a->mode == MODE_FT4) {                                          /* no FT4 on 160 / 60 m */
        unsigned f = 0;
        for (int i = 0; i < (int)(sizeof k_bands / sizeof *k_bands); ++i) if (k_bands[i].ft4) f |= 1u << i;
        if (m & f) m &= f;
    }
    return m;
}

const char *mode_name(op_mode_t m)
{
    static const char *n[] = {"FT8", "FT4", "JS8", "WSPR"};
    return m < MODE_COUNT ? n[m] : "?";
}

double mode_period(const app_t *a)
{
    if (a->mode == MODE_FT8) return 15;
    if (a->mode == MODE_FT4) return 7.5;
    if (a->mode == MODE_WSPR) return WSPR_PERIOD_S;
    return js8_submode_info((js8_submode_t)a->js8_sub)->period_s;
}

void app_status(app_t *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->status, sizeof a->status, fmt, ap);
    va_end(ap);
    qlog("status", "%s", a->status);
    a->status_until = clk_mono() + 6.0;
    a->dirty = 1;
}

void app_add_line(app_t *a, const rxline_t *l)
{
    if (a->nlines == MAX_LINES) {
        memmove(a->lines, a->lines + 1, sizeof(rxline_t) * (MAX_LINES - 1));
        a->nlines--;
        if (a->sel > 0) a->sel--;
    }
    a->lines[a->nlines++] = *l;
    a->dirty = 1;
}

/* ------------------------------------------------------------------ */
/* Audio in: ring + waterfall                                           */
/* ------------------------------------------------------------------ */

static kiss_fftr_cfg g_wf_cfg;
static float g_wf_win[WF_NFFT], g_wf_buf[WF_NFFT];
static int g_wf_fill;

void app_audio_in(const int16_t *pcm, int n, double utc_first, void *user)
{
    app_t *a = user;
    pthread_mutex_lock(&a->lock);
    double utc0 = utc_first - (double)a->ring_total / AUDIO_DSP_RATE;
    if (!a->have_utc0 || fabs(utc0 - a->ring_utc0) > 0.5) { a->ring_utc0 = utc0; a->have_utc0 = 1; }
    else a->ring_utc0 += 0.02 * (utc0 - a->ring_utc0);    /* smooth ALSA jitter */
    for (int i = 0; i < n; ++i) a->ring[(a->ring_total + i) % a->ring_len] = pcm[i];
    a->ring_total += n;
    pthread_mutex_unlock(&a->lock);

    for (int i = 0; i < n; ++i) {
        g_wf_buf[g_wf_fill++] = pcm[i];
        if (g_wf_fill == WF_NFFT) {
            float in[WF_NFFT];
            kiss_fft_cpx out[WF_NFFT / 2 + 1];
            for (int k = 0; k < WF_NFFT; ++k) in[k] = g_wf_buf[k] * g_wf_win[k];
            kiss_fftr(g_wf_cfg, in, out);
            float db[WF_BINS], sorted[WF_BINS];
            for (int b = 0; b < WF_BINS; ++b) {
                float p = out[b].r * out[b].r + out[b].i * out[b].i;
                db[b] = sorted[b] = 10.0f * log10f(p + 1e-3f);
            }
            /* noise floor = 20th percentile, tracked slowly */
            for (int x = 1; x < WF_BINS; ++x) {
                float v = sorted[x];
                int y = x - 1;
                while (y >= 0 && sorted[y] > v) { sorted[y + 1] = sorted[y]; --y; }
                sorted[y + 1] = v;
            }
            float fl = sorted[WF_BINS / 5];
            a->wf_floor = a->wf_floor == 0 ? fl : a->wf_floor + 0.05f * (fl - a->wf_floor);
            int row = (a->wf_head + 1) % WF_ROWS;
            for (int b = 0; b < WF_BINS; ++b) {
                float v = (db[b] - a->wf_floor + 3.0f) * (255.0f / 33.0f);   /* 33 dB span */
                a->wf[row][b] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
            a->wf_head = row;
            g_wf_fill = 0;
            a->dirty = 1;
        }
    }
}

/* great-circle distance between two Maidenhead grids (4 or 6 chars), km */
int grid_distance_km(const char *g1, const char *g2)
{
    double la[2], lo[2];
    const char *g[2] = {g1, g2};
    for (int k = 0; k < 2; ++k) {
        if (strlen(g[k]) < 4) return -1;
        int A = toupper((unsigned char)g[k][0]) - 'A', B = toupper((unsigned char)g[k][1]) - 'A';
        if (A < 0 || A > 17 || B < 0 || B > 17 || !isdigit((unsigned char)g[k][2]) || !isdigit((unsigned char)g[k][3])) return -1;
        lo[k] = A * 20 - 180 + (g[k][2] - '0') * 2 + 1.0;
        la[k] = B * 10 - 90 + (g[k][3] - '0') + 0.5;
        if (strlen(g[k]) >= 6 && isalpha((unsigned char)g[k][4]) && isalpha((unsigned char)g[k][5])) {
            lo[k] += -1.0 + (tolower((unsigned char)g[k][4]) - 'a' + 0.5) * (2.0 / 24);
            la[k] += -0.5 + (tolower((unsigned char)g[k][5]) - 'a' + 0.5) * (1.0 / 24);
        }
    }
    double p1 = la[0] * M_PI / 180, p2 = la[1] * M_PI / 180, dl = (lo[1] - lo[0]) * M_PI / 180;
    double c = sin(p1) * sin(p2) + cos(p1) * cos(p2) * cos(dl);
    if (c > 1) c = 1;
    if (c < -1) c = -1;
    return (int)lround(6371.0 * acos(c));
}

/* Copy n samples starting at UTC t into out; returns 0 if fully available. */
static int ring_extract(app_t *a, double t, int n, int16_t *out)
{
    pthread_mutex_lock(&a->lock);
    int rc = -1;
    if (a->have_utc0) {
        int64_t k0 = (int64_t)llround((t - a->ring_utc0) * AUDIO_DSP_RATE);
        int64_t oldest = (int64_t)a->ring_total - a->ring_len;
        if (k0 >= 0 && k0 >= oldest && k0 + n <= (int64_t)a->ring_total) {
            for (int i = 0; i < n; ++i) out[i] = a->ring[(k0 + i) % a->ring_len];
            rc = 0;
        }
    }
    pthread_mutex_unlock(&a->lock);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Init / mode / band                                                   */
/* ------------------------------------------------------------------ */

static int g_hw;
static app_t *g_app;
static const char *my_call(void) { return g_app->cfg.callsign; }
static const char *my_grid(void) { return g_app->cfg.grid; }
static pthread_t g_dec_thr, g_tx_thr, g_misc_thr;

static void fmt_mhz(int64_t hz, char *out) { sprintf(out, "%.3f", hz / 1e6); }

/* Put the radio in its digital mode: USB-D / DATA-U (Hamlib PKTUSB) with a
 * 3 kHz passband, read it back, and fall back to plain USB at the widest
 * passband the radio accepts on radios without a data mode (e.g. the G90).
 * The working choice is remembered. This is the rule for every radio. */
static char g_rig_mode[16];
static void set_data_mode(app_t *a)
{
    if (!(g_hw && a->rig_ok)) return;
    const char *want = g_rig_mode[0] ? g_rig_mode : a->radio->data_mode;
    char got[24] = "";
    int rc = rig_set_mode(want, 3000);
    if (rc) rc = rig_set_mode(want, 0);                  /* radio has no 3 kHz filter choice: its default */
    if (rc == 0 && rig_get_mode(got, sizeof got, NULL) == 0 && !strcmp(got, want)) {
        if (strcmp(g_rig_mode, want)) qlog("rig", "mode %s (%s)", want, !strcmp(want, "PKTUSB") ? "USB-D / DATA-U" : "plain USB");
        snprintf(g_rig_mode, sizeof g_rig_mode, "%s", want);
        return;
    }
    if (strcmp(want, "USB")) {                           /* no data mode on this radio: plain USB */
        qlog("rig", "radio refused %s (%s, reads back '%s'): using USB", want, rc ? rig_error() : "set ok", got);
        snprintf(g_rig_mode, sizeof g_rig_mode, "USB");
        /* widest USB the radio accepts: 3 kHz is usually the top filter */
        static const int pbw[] = {3000, 2900, 2800, 2700, 2400, 0};
        for (int i = 0; i < 6; ++i) if (rig_set_mode("USB", pbw[i]) == 0) { qlog("rig", "USB passband %d Hz", pbw[i]); break; }
        app_status(a, "%s has no data mode here: using USB", a->radio->name);
    } else qlog("rig", "setting USB failed: %s", rig_error());
}

/* Tune the radio and confirm it took the frequency. Returns 0 on success. */
static int do_qsy(app_t *a, int band, int64_t hz)
{
    char f[16];
    fmt_mhz(hz, f);
    if (!(g_hw && a->rig_ok)) { a->dial_hz = hz; return 0; }
    int rc = rig_set_freq(hz);
    int64_t back = rc ? -1 : rig_get_freq();
    if (rc || back < 0 || llabs(back - hz) > 100) {
        a->band_rejected |= 1u << band;           /* skip it from now on */
        if (back > 0) a->dial_hz = back;
        app_status(a, "%s: radio won't tune %s MHz - band skipped", k_bands[band].name, f);
        return -1;
    }
    set_data_mode(a);
    a->dial_hz = hz;
    qlog("rig", "QSY %s %.6f MHz %s %s", k_bands[band].name, hz / 1e6, mode_name(a->mode), g_rig_mode);
    app_status(a, "QSY %s %s MHz %s", k_bands[band].name, f, mode_name(a->mode));
    return 0;
}

static void clear_tx_for_qsy(app_t *a)
{
    pthread_mutex_lock(&a->lock);
    a->ntxq = 0;                                  /* nothing queued for the old band goes out on the new one */
    seq_reset(a);
    pthread_mutex_unlock(&a->lock);
}

void app_set_band(app_t *a, int band)
{
    int nb;
    band_table(&nb);
    if (band < 0 || band >= nb) return;
    if (a->tx_active) { app_status(a, "can't change band while transmitting"); return; }
    if (!(a->band_mask & (1u << band))) {
        app_status(a, "%s doesn't cover %s", a->radio->name, k_bands[band].name);
        return;
    }
    if (a->mode == MODE_FT4 && !k_bands[band].ft4) {
        a->band = band;                                   /* follow the radio, but there's no FT4 spot to tune to */
        app_status(a, "no FT4 frequency on %s", k_bands[band].name);
        a->dirty = 1;
        return;
    }
    clear_tx_for_qsy(a);
    int prev = a->band;
    a->band = band;
    if (do_qsy(a, band, mode_freq(a, band))) {    /* refused: show the band the radio is really on */
        int b = band_from_freq(a->dial_hz);
        a->band = b >= 0 ? b : prev;
    }
    a->dirty = 1;
}

void app_step_band(app_t *a, int dir)
{
    int nb;
    band_table(&nb);
    unsigned m = app_bands(a);
    int b = a->band;
    for (int i = 0; i < nb; ++i) {
        b = (b + dir + nb) % nb;
        if (m & (1u << b)) {
            app_set_band(a, b);
            if (a->band == b) return;
            m &= ~a->band_rejected;                   /* refused: keep going to the next one */
        }
    }
}

/* ---- battery ------------------------------------------------------ */

void app_poll_battery(app_t *a)
{
    const char *root = getenv("QSOZERO_DEVROOT");
    batt_update(root ? root : "", &a->batt, clk_mono());
    const batt_t *b = &a->batt.now;
    if (!b->present || b->percent < 0) return;
    if (b->charging || b->full) { a->batt.warned = 0; return; }
    int lvl = b->percent <= 5 ? 5 : b->percent <= 15 ? 15 : 0;
    if (lvl && (!a->batt.warned || lvl < a->batt.warned)) {
        a->batt.warned = lvl;
        if (b->hours_left > 0) app_status(a, "BATTERY %d%% - about %d min left", b->percent, (int)(b->hours_left * 60));
        else app_status(a, "BATTERY %d%% - charge soon", b->percent);
        a->status_until = clk_mono() + 30;
    }
    a->dirty = 1;
}

/* ---- FT8 CQ modifier ---------------------------------------------- */

int cq_mod_valid(const char *mod)
{
    int n = (int)strlen(mod), nd = 0, nl = 0;
    if (n == 0) return 1;
    for (int i = 0; i < n; ++i) {
        if (mod[i] >= '0' && mod[i] <= '9') ++nd;
        else if (mod[i] >= 'A' && mod[i] <= 'Z') ++nl;
        else return 0;
    }
    return (nl == n && n <= 4) || (nd == 3 && n == 3);
}

int app_set_cq_mod(app_t *a, const char *mod)
{
    char m[16];
    int n = 0;
    for (const char *p = mod; *p && n < (int)sizeof m - 1; ++p)
        if (*p != ' ') m[n++] = (char)toupper((unsigned char)*p);
    m[n] = 0;
    if (!strcmp(m, "NONE") || !strcmp(m, "OFF")) m[0] = 0;
    if (!cq_mod_valid(m)) {
        app_status(a, "CQ modifier: 1-4 letters (POTA) or 3 digits (290)");
        return -1;
    }
    snprintf(a->cfg.cq_mod, sizeof a->cfg.cq_mod, "%s", m);
    app_status(a, m[0] ? "FT8 calls CQ %s" : "FT8 calls plain CQ", m);
    return 0;
}

/* ---- JS8 -> APRS (@APRSIS) ----------------------------------------- */
/* JS8Call relays "@APRSIS CMD <text>" to APRS-IS as
 *   FROM>APJ8CL,qAS,RELAY:<text>
 * so <text> is an APRS message: ":" + addressee padded to 9 + ":" +
 * message + "{" + number. APRS limits the message to 67 characters and
 * does not allow | ~ { in it. */

static int aprs_text_ok(app_t *a, const char *body)
{
    if (!a->cfg.callsign[0] || !strcmp(a->cfg.callsign, "N0CALL")) { app_status(a, "set your callsign first"); return 0; }
    if (a->mode != MODE_JS8) { app_status(a, "APRS goes out over JS8: switch to JS8 first"); return 0; }
    if (strlen(body) > 67) { app_status(a, "APRS message too long (%d/67)", (int)strlen(body)); return 0; }
    if (strpbrk(body, "|~{")) { app_status(a, "APRS messages can't contain | ~ {"); return 0; }
    return 1;
}

static int aprs_send(app_t *a, const char *addressee, const char *body)
{
    if (!aprs_text_ok(a, body)) return -1;
    if (!a->aprs_msgno) a->aprs_msgno = 1 + (int)(time(NULL) / 60 % 900);   /* differs between sessions */
    char line[160], to[16];
    snprintf(to, sizeof to, "%s", addressee);
    for (char *p = to; *p; ++p) *p = (char)toupper((unsigned char)*p);
    snprintf(line, sizeof line, "@APRSIS CMD :%-9.9s:%s{%d", to, body, a->aprs_msgno);
    a->aprs_msgno = a->aprs_msgno % 999 + 1;
    snprintf(a->input, sizeof a->input, "%s", line);
    app_submit_input(a);
    return 0;
}

int app_aprs_position(app_t *a)
{
    char g[16];
    snprintf(g, sizeof g, "%s", a->cfg.grid);
    if (strlen(g) < 4) { app_status(a, "set your grid first (or Grid from GPS)"); return -1; }
    if (!aprs_text_ok(a, "")) return -1;
    snprintf(a->input, sizeof a->input, "@APRSIS GRID %s", g);
    app_submit_input(a);
    return 0;
}

int app_aprs_message(app_t *a, const char *to, const char *text)
{
    int c;
    char base[16];
    snprintf(base, sizeof base, "%s", to);
    for (char *p = base; *p; ++p) *p = (char)toupper((unsigned char)*p);
    char *dash = strchr(base, '-');
    if (dash) *dash = 0;
    if (!to[0] || strlen(to) > 9 || !js8v_is_valid_callsign(base, &c) || !text[0]) {
        app_status(a, "usage: /aprs CALL[-SSID] message");
        return -1;
    }
    return aprs_send(a, to, text);
}

int app_aprs_sms(app_t *a, const char *number, const char *text)
{
    if (!number[0] || !text[0]) { app_status(a, "usage: /sms NUMBER-or-alias message"); return -1; }
    char body[160];
    snprintf(body, sizeof body, "@%s %s", number[0] == '@' ? number + 1 : number, text);
    return aprs_send(a, "SMS", body);
}

int app_aprs_email(app_t *a, const char *addr, const char *text)
{
    if (!strchr(addr, '@') || !text[0]) { app_status(a, "usage: /email name@domain message"); return -1; }
    char body[160];
    snprintf(body, sizeof body, "%s %s", addr, text);
    return aprs_send(a, "EMAIL-2", body);
}

static void clear_decodes(app_t *a);

void app_ghostnet_band(app_t *a, int band)
{
    if (!ghostnet_freq(band)) return;
    if (a->tx_active) { app_status(a, "can't change band while transmitting"); return; }
    a->cfg.ghostnet = 1;
    if (a->mode != MODE_JS8) clear_decodes(a);
    a->mode = MODE_JS8;
    a->ghost_hz[band] = 0;                        /* chosen from the menu: the net default */
    app_set_band(a, band);                        /* explicit: tunes even with auto QSY off */
    if (a->band == band) app_status(a, "GHOSTNET %s %.3f MHz USB-D", k_bands[band].name, ghostnet_freq(band) / 1e6);
}

void app_set_ghostnet(app_t *a, int on)
{
    if (a->tx_active) { app_status(a, "can't change while transmitting"); return; }
    if (!on) {
        a->cfg.ghostnet = 0;
        if (a->mode == MODE_JS8 && (a->cfg.auto_qsy || !(g_hw && a->rig_ok))) app_set_band(a, a->band);
        app_status(a, "GHOSTNET off");
        return;
    }
    /* stay on 80/40/20 if already there, otherwise go to 40 m (or what the radio has) */
    int b = ghostnet_freq(a->band) ? a->band : 3;
    if (!(a->band_mask & (1u << b))) b = (a->band_mask & (1u << 5)) ? 5 : 1;
    app_ghostnet_band(a, b);
}

/* New mode: the stations in the list were heard on the old one, so start
 * the decode list afresh. */
static void clear_decodes(app_t *a)
{
    pthread_mutex_lock(&a->lock);
    a->nlines = 0;
    a->sel = -1;
    pthread_mutex_unlock(&a->lock);
    a->dirty = 1;
}

void app_set_mode(app_t *a, op_mode_t m)
{
    if (a->tx_active) { app_status(a, "can't change mode while transmitting"); return; }
    if (m != a->mode) clear_decodes(a);
    a->mode = m;
    if (m == MODE_WSPR) { a->tx_off = 1500; a->rx_off = 1500; }
    clear_tx_for_qsy(a);
    if (a->cfg.auto_qsy || !(g_hw && a->rig_ok)) app_set_band(a, a->band);
    else set_data_mode(a);                               /* VFO left alone, but still USB-D */
    app_status(a, "%s %s", mode_name(m), m == MODE_JS8 ? js8_submode_info(a->js8_sub)->name : "");
}

/* ---- hardware connections (startup and the Setup pickers) ---------- */

void app_rig_port(const app_t *a, char *out, int outsz)
{
    const char *r = getenv("QSOZERO_DEVROOT");
    if (a->cfg.rig_device[0]) { snprintf(out, outsz, "%s", a->cfg.rig_device); return; }
    dev_serial_auto(r ? r : "", a->radio->serial_hint, a->radio->serial_default, out, outsz);
}

static void apply_band_mask(app_t *a)
{
    a->band_mask = a->radio->bands ? a->radio->bands : BANDS_ALL;
    a->band_rejected = 0;
    unsigned want = parse_bands(a->cfg.bands);
    if (want && (a->band_mask & want)) a->band_mask &= want;
    if (!(a->band_mask & (1u << a->band)))               /* current band not covered */
        for (int i = 0; i < 11; ++i) if (a->band_mask & (1u << i)) { a->band = i; break; }
}

/* (Re)open rigctld for the configured radio and port. Blocking: a failed
 * attempt can take a few seconds. On success, adopts the radio's band. */
static void rig_connect(app_t *a)
{
    a->rig_ok = 0;
    rig_close();
    rig_config_t rc = {.profile = a->radio, .baud = a->cfg.rig_baud, .spawn = a->cfg.spawn_rigctld, .port = 4532};
    app_rig_port(a, rc.serial, sizeof rc.serial);
    rc.ptt = a->radio->ptt;
    if (!strcmp(a->cfg.ptt, "cat")) rc.ptt = PTT_CAT;
    if (!strcmp(a->cfg.ptt, "rts")) rc.ptt = PTT_RTS;
    if (!strcmp(a->cfg.ptt, "dtr")) rc.ptt = PTT_DTR;
    snprintf(rc.ptt_device, sizeof rc.ptt_device, "%s", a->cfg.ptt_device);
    qlog("rig", "%s: connecting on %s%s", a->radio->name, rc.serial, a->cfg.rig_device[0] ? "" : " (auto)");
    int ok = rig_open(&rc) == 0;
    if (!ok && !a->cfg.rig_device[0]) {                   /* auto: try the radio's other serial interfaces */
        const char *r = getenv("QSOZERO_DEVROOT");
        char c[4][160];
        int nc = dev_serial_candidates(r ? r : "", a->radio->serial_hint, c, 4);
        for (int i = 0; i < nc && !ok; ++i) {
            if (!strcmp(c[i], rc.serial)) continue;
            qlog("rig", "no answer on %s, trying %s", rc.serial, c[i]);
            snprintf(rc.serial, sizeof rc.serial, "%s", c[i]);
            ok = rig_open(&rc) == 0;
        }
    }
    if (!ok) { app_status(a, "RIG %s: %s", rc.serial, rig_error()); a->dirty = 1; return; }
    int64_t f = rig_get_freq();
    int b = band_from_freq(f);
    if (f > 0) a->dial_hz = f;
    if (b >= 0 && (a->band_mask & (1u << b))) a->band = b;
    a->rig_ok = 1;
    qlog("rig", "online, VFO %.6f MHz", f / 1e6);
    g_rig_mode[0] = 0;
    set_data_mode(a);
    a->dirty = 1;
}

static const char *dev_root_env(void)
{
    const char *r = getenv("QSOZERO_DEVROOT");
    return r ? r : "";
}

/* card id out of plughw:CARD=<id>[,DEV=n] */
static void card_of(const char *v, char *out, size_t sz)
{
    const char *c = strstr(v, "CARD=");
    if (!c) { out[0] = 0; return; }
    c += 5;
    snprintf(out, sz, "%.*s", (int)strcspn(c, ","), c);
}

/* Sound card levels: apply the saved ones (no alsactl / root needed), else read. */
static void apply_levels(app_t *a)
{
    if (a->cfg.audio_in_level >= 0) mixer_set(a->audio_in_dev, 1, a->cfg.audio_in_level, &a->mix_in);
    else mixer_get(a->audio_in_dev, 1, &a->mix_in);
    if (a->cfg.audio_out_level >= 0) mixer_set(a->audio_out_dev, 0, a->cfg.audio_out_level, &a->mix_out);
    else mixer_get(a->audio_out_dev, 0, &a->mix_out);
    for (int k = 0; k < 2; ++k) {
        const mixer_level_t *m = k ? &a->mix_out : &a->mix_in;
        if (m->ok) qlog("audio", "%s level: %s %d%%%s%s", k ? "out" : "in", m->control, m->percent, m->muted ? " (MUTED)" : "",
                        (k ? a->cfg.audio_out_level : a->cfg.audio_in_level) >= 0 ? " (from settings)" : "");
        else qlog("audio", "%s level: n/a (%s)", k ? "out" : "in", m->error);
    }
}

/* Pick the input (recording) and output (playback) devices: the saved
 * choice if it's plugged in, otherwise automatic (the radio's USB sound
 * card). Then (re)open capture. */
static void audio_connect(app_t *a)
{
    const char *root = dev_root_env();
    char in[96] = "", out[96] = "", card[40], note[96] = "";
    {
        dev_opt_t o[DEV_MAX];
        for (int cap = 1; cap >= 0; --cap) {
            int n = dev_audio_list(root, cap, "", o, DEV_MAX);
            char l[400] = "";
            for (int i = 1; i < n; ++i) snprintf(l + strlen(l), sizeof l - strlen(l), "%s%s [%s]", i > 1 ? ", " : "", o[i].value, o[i].desc);
            qlog("audio", "%s devices: %s", cap ? "input" : "output", n > 1 ? l : "none");
        }
    }
    if (a->cfg.audio_in[0] && dev_audio_present(root, a->cfg.audio_in, 1)) snprintf(in, sizeof in, "%s", a->cfg.audio_in);
    else {
        if (a->cfg.audio_in[0]) snprintf(note, sizeof note, "audio in %s not found, using auto. ", a->cfg.audio_in);
        dev_audio_auto(root, a->radio->audio_hint, 1, NULL, in, sizeof in);
    }
    card_of(in, card, sizeof card);
    if (a->cfg.audio_out[0] && dev_audio_present(root, a->cfg.audio_out, 0)) snprintf(out, sizeof out, "%s", a->cfg.audio_out);
    else {
        if (a->cfg.audio_out[0]) snprintf(note + strlen(note), sizeof note - strlen(note), "audio out %s not found, using auto. ", a->cfg.audio_out);
        dev_audio_auto(root, a->radio->audio_hint, 0, card, out, sizeof out);   /* same radio, its playback side */
    }
    qlog("audio", "using input %s%s, output %s%s", in[0] ? in : "(none)", a->cfg.audio_in[0] ? "" : " (auto)",
         out[0] ? out : "(none)", a->cfg.audio_out[0] ? "" : " (auto)");
    audio_close();
    a->audio_ok = 0;
    snprintf(a->audio_in_dev, sizeof a->audio_in_dev, "%s", in);
    snprintf(a->audio_out_dev, sizeof a->audio_out_dev, "%s", out);
    if (!in[0] || !out[0]) {
        audio_set_output(out);
        app_status(a, "%sno radio sound card found: plug it in, then Setup > Rescan audio", note);
        a->dirty = 1;
        return;
    }
    a->audio_ok = audio_open(in, out, app_audio_in, a) == 0;
    if (!a->audio_ok) app_status(a, "%sAUDIO: %s", note, audio_error());
    else if (note[0]) app_status(a, "%s", note);
    apply_levels(a);
    a->dirty = 1;
}

void app_audio_level(app_t *a, int capture, int delta)
{
    mixer_level_t *m = capture ? &a->mix_in : &a->mix_out;
    const char *dev = capture ? a->audio_in_dev : a->audio_out_dev;
    if (!g_hw) { app_status(a, "hardware is off (--nohw)"); return; }
    if (!m->ok) { app_status(a, "%s", m->error[0] ? m->error : "no level control"); return; }
    int v = m->percent + delta;
    if (m->muted && delta > 0) v = m->percent;                /* first press just unmutes */
    if (mixer_set(dev, capture, v, m) == 0) {
        if (capture) a->cfg.audio_in_level = m->percent; else a->cfg.audio_out_level = m->percent;
        app_status(a, "%s level %d%% (%s)", capture ? "audio in" : "audio out", m->percent, m->control);
    } else app_status(a, "%s", m->error);
    a->dirty = 1;
}

void app_rescan_audio(app_t *a)
{
    if (!g_hw) { app_status(a, "hardware is off (--nohw)"); return; }
    if (a->tx_active) { app_status(a, "can't change while transmitting"); return; }
    audio_connect(a);
    if (a->audio_ok) app_status(a, "audio in %s, out %s", a->audio_in_dev, a->audio_out_dev);
}

static void *rig_reconnect_thread(void *arg)
{
    app_t *a = arg;
    rig_connect(a);
    if (a->rig_ok) {
        app_set_mode(a, a->mode);                        /* tunes (auto QSY) and sets USB */
        app_status(a, "%s online", a->radio->name);
    }
    a->rig_busy = 0;
    a->dirty = 1;
    return NULL;
}

static int hw_change_ok(app_t *a)
{
    if (!g_hw) { app_status(a, "saved (hardware is off: --nohw)"); return 0; }
    if (a->tx_active) { app_status(a, "can't change while transmitting"); return 0; }
    if (a->rig_busy) { app_status(a, "still connecting to the radio..."); return 0; }
    return 1;
}

static void rig_reconnect(app_t *a)
{
    if (!hw_change_ok(a)) return;
    a->rig_busy = 1;
    a->rig_ok = 0;
    char port[160];
    app_rig_port(a, port, sizeof port);
    app_status(a, "connecting to %s on %s...", a->radio->name, port);
    pthread_t t;
    if (pthread_create(&t, NULL, rig_reconnect_thread, a)) { a->rig_busy = 0; return; }
    pthread_detach(t);
}

void app_use_radio(app_t *a, const char *id)
{
    const radio_profile_t *r = radio_find(id);
    if (!r) return;
    if (a->tx_active) { app_status(a, "can't change while transmitting"); return; }
    a->radio = r;
    snprintf(a->cfg.radio, sizeof a->cfg.radio, "%s", r->id);
    apply_band_mask(a);
    if (!hw_change_ok(a)) return;
    if (!a->cfg.audio_in[0]) audio_connect(a);           /* "auto" audio follows the radio */
    rig_reconnect(a);
}

void app_use_rig_device(app_t *a, const char *dev)
{
    if (a->tx_active) { app_status(a, "can't change while transmitting"); return; }
    snprintf(a->cfg.rig_device, sizeof a->cfg.rig_device, "%s", dev);
    rig_reconnect(a);
}

void app_use_audio(app_t *a, const char *in, const char *out)
{
    if (a->tx_active) { app_status(a, "can't change while transmitting"); return; }
    if (in) snprintf(a->cfg.audio_in, sizeof a->cfg.audio_in, "%s", in);
    if (out) snprintf(a->cfg.audio_out, sizeof a->cfg.audio_out, "%s", out);
    if (!hw_change_ok(a)) return;
    audio_connect(a);                                    /* resolves auto and reopens */
    if (a->audio_ok) app_status(a, "audio in %s, out %s", a->audio_in_dev, a->audio_out_dev);
}

/* ADIF log location: ~/qso-zero/qso-zero.adi by default; the folder is
 * created when missing. */
int log_dir_make(const char *file)
{
    char d[200];
    snprintf(d, sizeof d, "%s", file);
    char *slash = strrchr(d, '/');
    if (!slash || slash == d) return 0;
    *slash = 0;
    for (char *p = d + 1; *p; ++p)
        if (*p == '/') { *p = 0; mkdir(d, 0755); *p = '/'; }
    return mkdir(d, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static void log_path_setup(app_t *a)
{
    if (log_dir_make(a->cfg.log_path)) qlog("config", "cannot create the folder for %s", a->cfg.log_path);
    if (log_dir_make(a->cfg.inbox_path)) qlog("config", "cannot create the folder for %s", a->cfg.inbox_path);
    /* JS8 messages kept in the inbox's previous default place move into ~/qso-zero/js8/ */
    config_t def;
    config_defaults(&def);
    if (!strcmp(a->cfg.inbox_path, def.inbox_path)) {
        char prev[200];
        size_t n = strlen(def.inbox_path) - strlen("/qso-zero/js8/qso-zero-inbox.txt");
        snprintf(prev, sizeof prev, "%.*s/qso-zero-inbox.txt", (int)n, def.inbox_path);
        if (access(prev, F_OK) == 0 && access(a->cfg.inbox_path, F_OK) != 0 && rename(prev, a->cfg.inbox_path) == 0)
            qlog("config", "JS8 inbox moved to %s", a->cfg.inbox_path);
    }
}

int app_init(app_t *a, const char *cfgpath, int hw)
{
    memset(a, 0, sizeof *a);
    g_app = a;
    pthread_mutex_init(&a->lock, NULL);
    g_hw = hw;
    config_defaults(&a->cfg);
    config_load(&a->cfg, cfgpath);
    log_path_setup(a);
    a->radio = radio_find(a->cfg.radio);
    if (!a->radio) { int n; a->radio = radio_list(&n); }
    a->js8_sub = a->cfg.js8_submode;
    a->rx_off = a->cfg.rx_offset;
    a->tx_off = a->cfg.tx_offset;
    a->sel = -1;
    a->tx_enabled = 1;
    a->band = 5; /* 20 m */
    a->ring_len = RING_SECONDS * AUDIO_DSP_RATE;
    a->ring = calloc(a->ring_len, sizeof(int16_t));
    g_wf_cfg = kiss_fftr_alloc(WF_NFFT, 0, NULL, NULL);
    for (int i = 0; i < WF_NFFT; ++i) g_wf_win[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / WF_NFFT);
    clk_init();
    if (js8v_load_jsc(a->cfg.jsc_path) && js8v_load_jsc("/usr/share/qso-zero/js8.jsc") &&
        js8v_load_jsc("/usr/local/share/qso-zero/js8.jsc") && js8v_load_jsc("data/js8.jsc"))
        app_status(a, "js8.jsc not found: JS8 free text limited to Huffman");

    apply_band_mask(a);
    if (hw) {
        rig_connect(a);
        rig_install_safety();
        audio_connect(a);
        a->gps_ok = gps_start(a->cfg.gps_device, a->cfg.gps_baud, a->cfg.gps_latency_ms) == 0;
        if (a->cfg.wsprnet_upload) uplink_start(a->cfg.wsprnet_host, 443, 80, my_call, my_grid);
    }
    /* Start in WSPR on 20 m: in the field you spot yourself first (beacon
     * with Ctrl+W, check wsprnet / the band) before calling anyone. With
     * Auto frequency on, the radio is tuned to 14.0956 MHz. */
    if (a->band_mask & (1u << 5)) a->band = 5;
    app_set_mode(a, MODE_WSPR);
    if (!strcmp(a->cfg.callsign, "N0CALL")) {
        app_status(a, "Welcome! Set your callsign: ESC > Callsign");
        a->status_until = clk_mono() + 60;
    }
    app_poll_battery(a);                                 /* a low-battery warning wins */
    qlog("config", "callsign %s grid %s  radio %s  CAT %s  audio in %s out %s  levels %d/%d  GPS %s  auto QSY %d  GHOSTNET %d  CQ mod '%s'",
         a->cfg.callsign, a->cfg.grid, a->radio->id, a->cfg.rig_device[0] ? a->cfg.rig_device : "auto",
         a->cfg.audio_in[0] ? a->cfg.audio_in : "auto", a->cfg.audio_out[0] ? a->cfg.audio_out : "auto",
         a->cfg.audio_in_level, a->cfg.audio_out_level, a->cfg.gps_device, a->cfg.auto_qsy, a->cfg.ghostnet, a->cfg.cq_mod);
    return 0;
}

void app_shutdown(app_t *a)
{
    qlog("exit", "shutting down");
    a->quit = 1;
    app_stop_threads(a);
    if (g_hw) {
        uplink_stop();
        rig_close();
        audio_close();
        gps_stop();
    }
    js8v_unload_jsc();
    free(a->ring);
}

/* ------------------------------------------------------------------ */
/* Decode thread                                                        */
/* ------------------------------------------------------------------ */

/* Transmit timing: sleep until exactly `utc` (only quitting cuts it short). */
static void tx_sleep_until(app_t *a, double utc)
{
    for (;;) {
        double d = utc - clk_now();
        if (d <= 0 || a->quit) return;
        struct timespec ts = {0, (long)(fmin(d, 0.1) * 1e9)};
        nanosleep(&ts, NULL);
    }
}

/* Decode timing: sleep until `utc`, but wake at once if the mode changes, so
 * e.g. leaving WSPR (whose decode waits up to 2 minutes) for FT8 doesn't
 * stall decoding. Never used for transmit timing. */
static void sleep_until(app_t *a, double utc)
{
    op_mode_t m = a->mode;
    for (;;) {
        double d = utc - clk_now();
        if (d <= 0 || a->quit || a->mode != m) return;
        struct timespec ts = {0, (long)(fmin(d, 0.1) * 1e9)};
        nanosleep(&ts, NULL);
    }
}

/* One FT8/FT4 receive cycle in two passes:
 *  early - just after an on-time transmission ends (FT8 0.5 + 12.64 s =
 *          13.14 s -> 13.35 s; FT4 0.5 + 5.04 s = 5.54 s -> 5.65 s), so most
 *          decodes appear ~0.65 s / ~0.25 s sooner and the sequencer can
 *          queue its reply earlier;
 *  final - at `ready` (FT8 14.0 s, FT4 5.9 s) over the longer window, for
 *          stations whose clocks run late; only new decodes are added.
 * The sequencer's per-cycle step (repeat / give up) runs once, after the
 * final pass. Each cycle's timing goes to the activity log. */
#define FT_MAX_SEEN 64
/* A message we sent ourselves: "CQ [MOD] MYCALL ...", or "DX MYCALL ...".
 * Radios like the X6200 return the TX audio on USB, so without this our own
 * transmission shows up as a decode. */
static int ft_is_own(const app_t *a, const char *text)
{
    char tok[4][16] = {{0}};
    int n = sscanf(text, "%15s %15s %15s %15s", tok[0], tok[1], tok[2], tok[3]);
    const char *me = a->cfg.callsign;
    if (n >= 2 && !strcmp(tok[0], "CQ")) return !strcmp(tok[1], me) || (n >= 3 && !strcmp(tok[2], me));
    return n >= 2 && !strcmp(tok[1], me);
}

static void ft_add_decodes(app_t *a, op_mode_t mode, double start, int odd, const ft8w_decode_t *d, int nd,
                           char seen[][40], int *nseen, int *added, int *own)
{
    pthread_mutex_lock(&a->lock);
    for (int i = 0; i < nd; ++i) {
        int dup = 0;
        for (int k = 0; k < *nseen; ++k) if (!strcmp(seen[k], d[i].text)) { dup = 1; break; }
        if (dup) continue;
        if (ft_is_own(a, d[i].text)) { ++*own; continue; }   /* our own signal heard back through the radio */
        if (*nseen < FT_MAX_SEEN) snprintf(seen[(*nseen)++], 40, "%s", d[i].text);
        rxline_t l = {.utc = start, .snr = d[i].snr, .dt = d[i].dt, .freq = d[i].freq, .mode = mode, .slot_odd = odd};
        snprintf(l.text, sizeof l.text, "%s", d[i].text);
        if (!strncmp(l.text, "CQ ", 3)) l.flags |= LINE_CQ;
        size_t cl = strlen(a->cfg.callsign);
        if (!strncmp(l.text, a->cfg.callsign, cl) && l.text[cl] == ' ') l.flags |= LINE_TOME;
        app_add_line(a, &l);
        seq_on_decode(a, &l);
        ++*added;
    }
    pthread_mutex_unlock(&a->lock);
    a->dirty = 1;
}

static void ft_cycle(app_t *a, op_mode_t mode, double start, double P, double ready, ft8w_t *dec, int16_t *buf)
{
    double early = mode == MODE_FT4 ? 5.65 : 13.35;
    int odd = ((long long)llround(start / P)) & 1;
    char seen[FT_MAX_SEEN][40];
    int nseen = 0, n1 = 0, n2 = 0, ran_early = 0, nown = 0;
    double ms1 = 0, ms2 = 0, at1 = 0, at2 = 0;
    ft8w_decode_t d[60];
    if (clk_now() < start + early - 0.02) {
        sleep_until(a, start + early);
        if (a->quit || a->mode != mode) return;
        int n = (int)((early - 0.1) * 12000);
        if (ring_extract(a, start, n, buf) != 0) qlog("decode", "%s early pass skipped: audio up to +%.2f s not in yet", mode_name(mode), early - 0.1);
        else {
            a->decoding = 1;
            a->dirty = 1;
            at1 = clk_now() - start;
            double t0 = clk_mono();
            int nd = ft8w_decode(dec, buf, n, d, 60);
            ms1 = (clk_mono() - t0) * 1000;
            ft_add_decodes(a, mode, start, odd, d, nd, seen, &nseen, &n1, &nown);
            ran_early = 1;
        }
    }
    sleep_until(a, start + ready);
    if (a->quit || a->mode != mode) { a->decoding = 0; return; }
    int n = (int)((ready - 0.1) * 12000);
    a->decoding = 1;
    a->dirty = 1;
    int ran_final = 0;
    if (ring_extract(a, start, n, buf) == 0) {
        ran_final = 1;
        at2 = clk_now() - start;
        double t0 = clk_mono();
        int nd = ft8w_decode(dec, buf, n, d, 60);
        ms2 = (clk_mono() - t0) * 1000;
        ft_add_decodes(a, mode, start, odd, d, nd, seen, &nseen, &n2, &nown);
        pthread_mutex_lock(&a->lock);
        seq_after_cycle(a, odd);
        pthread_mutex_unlock(&a->lock);
    }
    a->last_decode_ms = ms2 > ms1 ? ms2 : ms1;
    a->decoding = 0;
    a->dirty = 1;
    time_t ts = (time_t)start;
    struct tm tm;
    gmtime_r(&ts, &tm);
    const char *own = nown ? "  (our own transmission heard back: hidden)" : "";
    if (!ran_final && !ran_early)
        qlog("decode", "%s %02d:%02d:%04.1f  skipped: not a full cycle of audio yet (just switched mode or band, or the input restarted)",
             mode_name(mode), tm.tm_hour, tm.tm_min, fmod(start, 60.0));
    else if (!ran_final)
        qlog("decode", "%s %02d:%02d:%04.1f  early pass at +%.2f s: %d in %.0f ms (on screen +%.2f s)  final pass skipped: audio not in%s",
             mode_name(mode), tm.tm_hour, tm.tm_min, fmod(start, 60.0), at1, n1, ms1, at1 + ms1 / 1000, own);
    else if (ran_early)
        qlog("decode", "%s %02d:%02d:%04.1f  early pass at +%.2f s: %d in %.0f ms (on screen +%.2f s)  final at +%.2f s: %d more in %.0f ms (+%.2f s)%s",
             mode_name(mode), tm.tm_hour, tm.tm_min, fmod(start, 60.0), at1, n1, ms1, at1 + ms1 / 1000, at2, n2, ms2, at2 + ms2 / 1000, own);
    else
        qlog("decode", "%s %02d:%02d:%04.1f  final at +%.2f s: %d in %.0f ms (+%.2f s)%s",
             mode_name(mode), tm.tm_hour, tm.tm_min, fmod(start, 60.0), at2, n2, ms2, at2 + ms2 / 1000, own);
}

/* One JS8 receive period, in two passes at Fast and Turbo:
 *  first - 0.35 s after an on-time transmission ends (as before);
 *  late  - about 1 s later (never past the end of the period), over a longer
 *          window, for stations whose clocks run late.
 * Measured with the decoder: at Normal and Slow the first pass already copies
 * frames up to ~2.4 s late, and nothing later is copied by either pass, so a
 * second pass would only cost CPU. At Turbo (and Fast, on weak signals) it
 * recovers frames 1.6-3 s late that the first pass misses.
 * Frames are assembled into messages in the order they're decoded. The late
 * pass skips any frame the early pass already had, so a multi-frame message
 * is never joined twice; frames arriving over several periods are stitched
 * together by js8_assemble(), which follows each station's audio frequency
 * until the frame marked LAST. */
#define JS8_MAX_SEEN 48
static int js8_add_frames(app_t *a, double start, const js8_decode_t *d, int nd, char seen[][16], int *nseen)
{
    int added = 0;
    pthread_mutex_lock(&a->lock);
    for (int i = 0; i < nd; ++i) {
        char key[16];
        snprintf(key, sizeof key, "%.12s%d", d[i].text, d[i].frame_type & 7);
        int dup = 0;
        for (int k = 0; k < *nseen; ++k) if (!strcmp(seen[k], key)) { dup = 1; break; }
        if (dup) continue;
        if (*nseen < JS8_MAX_SEEN) snprintf(seen[(*nseen)++], 16, "%s", key);
        js8_assemble(a, d[i].text, d[i].frame_type, d[i].snr, d[i].freq, d[i].dt, start);
        ++added;
    }
    pthread_mutex_unlock(&a->lock);
    a->dirty = 1;
    return added;
}

static void js8_cycle(app_t *a, js8_decoder_t *js8, int sub, double start, double P, double ready, int16_t *buf)
{
    double late = (sub == JS8_FAST || sub == JS8_TURBO) ? fmin(ready + 1.0, P - 0.25) : ready;
    int nmax = js8_decoder_samples(js8);
    char seen[JS8_MAX_SEEN][16];
    int nseen = 0, n1 = 0, n2 = 0, ran1 = 0, ran2 = 0;
    double ms1 = 0, ms2 = 0, at1 = 0, at2 = 0;
    js8_decode_t d[40];
    sleep_until(a, start + ready);
    if (a->quit || a->mode != MODE_JS8 || a->js8_sub != sub) return;
    a->decoding = 1;
    a->dirty = 1;
    int n = (int)((ready - 0.1) * 12000);
    if (n > nmax) n = nmax;
    if (ring_extract(a, start, n, buf) == 0) {
        at1 = clk_now() - start;
        double t0 = clk_mono();
        int nd = js8_decode(js8, buf, n, 200, 2900, a->rx_off, d, 40);
        ms1 = (clk_mono() - t0) * 1000;
        n1 = js8_add_frames(a, start, d, nd, seen, &nseen);
        ran1 = 1;
    }
    if (late > ready + 0.2) {
        sleep_until(a, start + late);
        if (a->quit || a->mode != MODE_JS8 || a->js8_sub != sub) { a->decoding = 0; return; }
        n = (int)((late - 0.1) * 12000);
        if (n > nmax) n = nmax;
        if (ring_extract(a, start, n, buf) == 0) {
            at2 = clk_now() - start;
            double t0 = clk_mono();
            int nd = js8_decode(js8, buf, n, 200, 2900, a->rx_off, d, 40);
            ms2 = (clk_mono() - t0) * 1000;
            n2 = js8_add_frames(a, start, d, nd, seen, &nseen);
            ran2 = 1;
        }
    }
    a->last_decode_ms = ms2 > ms1 ? ms2 : ms1;
    a->decoding = 0;
    a->dirty = 1;
    time_t ts = (time_t)start;
    struct tm tm;
    gmtime_r(&ts, &tm);
    const char *nm = js8_submode_info((js8_submode_t)sub)->name;
    if (!ran1 && !ran2)
        qlog("decode", "JS8 %s %02d:%02d:%04.1f  skipped: not a full period of audio yet (just switched mode or band, or the input restarted)",
             nm, tm.tm_hour, tm.tm_min, fmod(start, 60.0));
    else if (ran2)
        qlog("decode", "JS8 %s %02d:%02d:%04.1f  first pass at +%.2f s: %d frame%s in %.0f ms  late pass at +%.2f s: %d more in %.0f ms",
             nm, tm.tm_hour, tm.tm_min, fmod(start, 60.0), at1, n1, n1 == 1 ? "" : "s", ms1, at2, n2, ms2);
    else
        qlog("decode", "JS8 %s %02d:%02d:%04.1f  at +%.2f s: %d frame%s in %.0f ms",
             nm, tm.tm_hour, tm.tm_min, fmod(start, 60.0), at1, n1, n1 == 1 ? "" : "s", ms1);
}

static void *decode_thread(void *arg)
{
    app_t *a = arg;
    ft8w_t *ft8 = ft8w_new(FT8W_FT8, 12000, 100, 3000), *ft4 = ft8w_new(FT8W_FT4, 12000, 100, 3000);
    js8_decoder_t *js8 = NULL;
    int js8_sub = -1;
    int16_t *buf = malloc(sizeof(int16_t) * WSPRRX_SAMPLES);   /* largest window: WSPR 114 s */
    while (!a->quit) {
        op_mode_t mode = a->mode;
        double P = mode_period(a);
        if (mode == MODE_WSPR) {
            /* receive cycle: even minute .. +114 s, decode at +115 s */
            double start = floor(clk_now() / 120.0) * 120.0;
            if (clk_now() > start + 115.0) start += 120.0;
            sleep_until(a, start + 115.0);
            if (a->quit || a->mode != MODE_WSPR || !a->cfg.wspr_rx) continue;
            if (fabs(a->wspr_tx_slot - start) < 1.0) continue;     /* we were transmitting */
            if (ring_extract(a, start, WSPRRX_SAMPLES, buf) != 0) continue;
            a->decoding = 1;
            a->dirty = 1;
            double t0 = clk_mono();
            wsprrx_spot_t sp[40];
            int nd = wsprrx_decode(buf, WSPRRX_SAMPLES, sp, 40);
            pthread_mutex_lock(&a->lock);
            for (int i = 0; i < nd; ++i) {
                rxline_t l = {.utc = start, .snr = sp[i].snr, .dt = sp[i].dt, .freq = sp[i].freq, .mode = MODE_WSPR};
                int km = grid_distance_km(a->cfg.grid, sp[i].grid);
                if (km >= 0) snprintf(l.text, sizeof l.text, "%-7s %-6s %2ddBm %5dkm dr%+d", sp[i].call, sp[i].grid, sp[i].dbm, km, sp[i].drift);
                else snprintf(l.text, sizeof l.text, "%-7s %-6s %2ddBm dr%+d", sp[i].call, sp[i].grid, sp[i].dbm, sp[i].drift);
                if (!strcmp(sp[i].call, a->cfg.callsign)) l.flags |= LINE_TOME;
                app_add_line(a, &l);
                if (a->cfg.wsprnet_upload && strcmp(sp[i].call, "<...>")) {
                    uplink_spot_t u = {.utc = start, .dial_hz = a->dial_hz, .audio_hz = sp[i].freq, .snr = sp[i].snr,
                                       .drift = sp[i].drift, .dbm = sp[i].dbm, .dt = sp[i].dt};
                    snprintf(u.call, sizeof u.call, "%s", sp[i].call);
                    snprintf(u.grid, sizeof u.grid, "%.6s", sp[i].grid);
                    uplink_add_wspr(&u);
                }
            }
            if (!nd) app_status(a, "WSPR: no spots this cycle");
            pthread_mutex_unlock(&a->lock);
            a->last_decode_ms = (clk_mono() - t0) * 1000.0;
            a->decoding = 0;
            a->dirty = 1;
            continue;
        }
        double start;
        clk_period_pos(P, &start);
        /* Decode as soon as the last symbol is in: FT8 ends at 13.14 s, JS8 at
         * start_delay + 79 symbols. Saves ~1 s, which matters on a 1 GHz A53
         * because the reply must start within ~2 s of the next slot. */
        double ready;
        if (mode == MODE_FT8) ready = 14.0;
        else if (mode == MODE_FT4) ready = 5.9;        /* 0.5 s + 105 x 48 ms = 5.54 s, + margin */
        else {
            const js8_submode_info_t *in = js8_submode_info((js8_submode_t)a->js8_sub);
            ready = in->start_delay_ms / 1000.0 + 79.0 * in->symbol_samples / 12000.0 + 0.35;
        }
        if (clk_now() > start + ready) start += P;      /* already past it: next period */
        if (mode_is_ft(mode)) {
            ft_cycle(a, mode, start, P, ready, mode == MODE_FT4 ? ft4 : ft8, buf);
            continue;
        }
        if (mode == MODE_JS8) {
            if (js8_sub != a->js8_sub) {
                js8_decoder_free(js8);
                js8_sub = a->js8_sub;
                js8 = js8_decoder_new((js8_submode_t)js8_sub);
            }
            if (js8) js8_cycle(a, js8, js8_sub, start, P, ready, buf);
            continue;
        }
        sleep_until(a, start + ready);
    }
    ft8w_free(ft8);
    ft8w_free(ft4);
    js8_decoder_free(js8);
    free(buf);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Transmit thread                                                      */
/* ------------------------------------------------------------------ */

static double next_tx_start(app_t *a, const txitem_t *t, double now)
{
    if (t->kind == TXK_TUNE) return now;
    if (t->kind == TXK_WSPR) {
        double s = ceil((now - 1.0) / 120.0) * 120.0 + 1.0;
        return s;
    }
    if (t->kind == TXK_FT8) {                      /* FT8 (15 s) or FT4 (7.5 s): the current mode */
        double P = a->mode == MODE_FT4 ? 7.5 : 15.0, late = a->mode == MODE_FT4 ? 1.0 : 2.0;
        double s = floor(now / P) * P;
        int idx = (int)((long long)llround(s / P) & 1);
        double pos = now - s;
        /* late start is fine up to ~2 s FT8 / ~1 s FT4 (the decoders' DT search range) */
        if ((t->slot_odd < 0 || idx == t->slot_odd) && pos < late) return pos < 0.5 ? s + 0.5 : now + 0.05;
        for (int k = 1; k <= 2; ++k) {
            double c = s + P * k;
            int ci = (int)((long long)llround(c / P) & 1);
            if (t->slot_odd < 0 || ci == t->slot_odd) return c + 0.5;
        }
        return s + P + 0.5;
    }
    /* JS8 */
    const js8_submode_info_t *in = js8_submode_info((js8_submode_t)a->js8_sub);
    double s = floor(now / in->period_s) * in->period_s;
    double pos = now - s, d = in->start_delay_ms / 1000.0;
    return pos < d - 0.05 ? s + d : s + in->period_s + d;
}

static float *build_audio(app_t *a, const txitem_t *t, size_t *n)
{
    const int R = AUDIO_HW_RATE;
    float f0 = (float)a->tx_off;
    float *buf = NULL;
    if (t->kind == TXK_FT8) {
        uint8_t tones[105];
        ft8w_proto_t pr = a->mode == MODE_FT4 ? FT8W_FT4 : FT8W_FT8;
        int nt = ft8w_encode(pr, t->text, tones);
        if (nt < 0) return NULL;
        buf = malloc(sizeof(float) * (size_t)(nt * 0.16 * R + R));
        *n = ft8w_synth(pr, tones, nt, f0, R, 1.0f, buf);
    } else if (t->kind == TXK_JS8) {
        uint8_t tones[79];
        if (js8_encode((js8_submode_t)a->js8_sub, t->bits, t->text, tones)) return NULL;
        const js8_submode_info_t *in = js8_submode_info((js8_submode_t)a->js8_sub);
        buf = malloc(sizeof(float) * (size_t)(79.0 * in->symbol_samples / 12000.0 * R + R));
        *n = js8_synth((js8_submode_t)a->js8_sub, tones, f0, R, 1.0f, buf);
    } else if (t->kind == TXK_WSPR) {
        uint8_t sym[WSPR_SYMBOLS];
        char grid4[5];
        snprintf(grid4, sizeof grid4, "%s", a->cfg.grid);
        if (wspr_encode(a->cfg.callsign, grid4, a->cfg.wspr_dbm, sym)) return NULL;
        buf = malloc(sizeof(float) * (size_t)(WSPR_SYMBOLS * 8192.0 / 12000.0 * R + R));
        float wf = f0 < 1410 ? 1410 : f0 > 1590 ? 1590 : f0;
        *n = wspr_synth(sym, wf, R, 1.0f, buf);
    } else if (t->kind == TXK_TUNE) {
        double secs = fmax(0.5, fmin(10.0, a->tune_until - clk_now()));
        *n = (size_t)(secs * R);
        buf = malloc(sizeof(float) * *n);
        for (size_t i = 0; i < *n; ++i) buf[i] = 0.8f * sinf(2 * (float)M_PI * f0 * i / R);
    }
    return buf;
}

static void *tx_thread(void *arg)
{
    app_t *a = arg;
    while (!a->quit) {
        pthread_mutex_lock(&a->lock);
        int have = a->ntxq > 0 && a->tx_enabled;
        txitem_t t;
        if (have) t = a->txq[0];
        pthread_mutex_unlock(&a->lock);
        if (!have) { struct timespec ts = {0, 50000000}; nanosleep(&ts, NULL); continue; }

        double start = next_tx_start(a, &t, clk_now());
        /* wait, but bail out if the queue changes or TX is halted */
        while (!a->quit && clk_now() < start - 0.08) {
            struct timespec ts = {0, 20000000};
            nanosleep(&ts, NULL);
            pthread_mutex_lock(&a->lock);
            int same = a->ntxq > 0 && a->tx_enabled && !memcmp(&a->txq[0], &t, sizeof t);
            pthread_mutex_unlock(&a->lock);
            if (!same) goto next;
        }
        if (a->quit) break;
        size_t n = 0;
        float *audio = build_audio(a, &t, &n);
        if (!audio) {
            app_status(a, "cannot encode '%s'", t.text);
            pthread_mutex_lock(&a->lock);
            if (a->ntxq) memmove(a->txq, a->txq + 1, sizeof(txitem_t) * --a->ntxq);
            pthread_mutex_unlock(&a->lock);
            continue;
        }
        qlog("tx", "%s at %.6f MHz + %d Hz: \"%s\"", t.kind == TXK_TUNE ? "TUNE" : t.kind == TXK_WSPR ? "WSPR" : t.kind == TXK_JS8 ? "JS8" : mode_name(a->mode),
             a->dial_hz / 1e6, a->tx_off, t.text);
        if (g_hw && !a->rig_ok) qlog("tx", "radio not connected: no PTT");
        if (g_hw && a->rig_ok && rig_ptt(1)) { app_status(a, "PTT failed: %s", rig_error()); free(audio); goto pop; }
        tx_sleep_until(a, start);
        if (t.kind == TXK_WSPR) a->wspr_tx_slot = floor(start / 120.0) * 120.0;
        a->tx_active = 1;
        a->dirty = 1;
        {
            /* echo our own transmission into the log */
            rxline_t l = {.utc = start, .snr = 0, .freq = (float)a->tx_off, .mode = a->mode, .flags = LINE_TX};
            if (t.kind == TXK_JS8) {
                js8_rxframe_t r;
                js8v_interpret(t.text, t.bits, &r);
                snprintf(l.text, sizeof l.text, "%s", r.message);
            } else if (t.kind == TXK_TUNE) snprintf(l.text, sizeof l.text, "TUNE carrier");
            else if (t.kind == TXK_WSPR) snprintf(l.text, sizeof l.text, "WSPR %s %.4s %d", a->cfg.callsign, a->cfg.grid, wspr_valid_dbm(a->cfg.wspr_dbm));
            else snprintf(l.text, sizeof l.text, "%s", t.text);
            pthread_mutex_lock(&a->lock);
            app_add_line(a, &l);
            pthread_mutex_unlock(&a->lock);
        }
        if (g_hw && a->audio_out_dev[0]) {          /* plays even if the input failed to open */
            double t0 = clk_mono();
            int rc = audio_play(audio, n, a->cfg.tx_level);
            if (rc < 0) app_status(a, "TX audio: %s", audio_error());
            else qlog("tx", "audio %.1f s on %s at %.0f%% drive%s", clk_mono() - t0, a->audio_out_dev, a->cfg.tx_level * 100, rc == 1 ? " (stopped)" : "");
        } else {
            if (g_hw) qlog("tx", "no audio output device: nothing sent");
            /* no hardware: simulate the airtime */
            double end = clk_now() + n / (double)AUDIO_HW_RATE;
            while (!a->quit && a->tx_enabled && clk_now() < end) {
                a->tx_progress = 1.0f - (float)((end - clk_now()) / (n / (double)AUDIO_HW_RATE));
                struct timespec ts = {0, 50000000};
                nanosleep(&ts, NULL);
            }
        }
        if (g_hw && a->rig_ok) rig_ptt(0);
        a->tx_active = 0;
        qlog("tx", "done");
        a->tx_progress = 0;
        free(audio);
    pop:
        pthread_mutex_lock(&a->lock);
        if (a->ntxq && !memcmp(&a->txq[0], &t, sizeof t)) memmove(a->txq, a->txq + 1, sizeof(txitem_t) * --a->ntxq);
        if (t.kind == TXK_FT8) a->seq.tries++;
        pthread_mutex_unlock(&a->lock);
        a->dirty = 1;
    next:;
    }
    if (g_hw) rig_ptt(0);
    return NULL;
}

/* rig polling + WSPR beacon scheduling + GPS grid */
static void *misc_thread(void *arg)
{
    app_t *a = arg;
    double last_poll = 0, last_batt = -100, last_health = 0;
    long last_wspr_slot = -1;
    srand((unsigned)time(NULL));
    while (!a->quit) {
        struct timespec ts = {0, 250000000};
        nanosleep(&ts, NULL);
        double now = clk_now();
        if (g_hw && a->rig_ok && !a->tx_active && clk_mono() - last_poll > 2.0) {
            int64_t f = rig_get_freq();
            if (f > 0 && f != a->dial_hz) {
                a->dial_hz = f;
                a->dirty = 1;
                /* VFO turned onto another band: follow it, and (auto) jump to
                 * this mode's frequency there. Tuning within a band is left alone. */
                int b = band_from_freq(f);
                if (b >= 0 && b == a->band && in_ghostnet(a) && ghostnet_freq(b))
                    a->ghost_hz[b] = f;                  /* GHOSTNET: remember the net's moved frequency */
                if (b >= 0 && b != a->band && !a->tx_active) {
                    a->band_rejected &= ~(1u << b);      /* the radio is clearly on it */
                    a->band_mask |= 1u << b;
                    if (in_ghostnet(a)) {                /* GHOSTNET: follow the band, leave the VFO */
                        a->band = b;
                        if (ghostnet_freq(b)) a->ghost_hz[b] = f;
                        app_status(a, "radio moved to %s", k_bands[b].name);
                    } else if (a->cfg.auto_qsy) app_set_band(a, b);
                    else { a->band = b; app_status(a, "radio moved to %s", k_bands[b].name); }
                }
            }
            last_poll = clk_mono();
        }
        if (clk_mono() - last_batt > 30.0) { app_poll_battery(a); last_batt = clk_mono(); }
        if (g_hw && clk_mono() - last_health > 60.0) {          /* once a minute: how things are going */
            last_health = clk_mono();
            audio_rx_stats_t st;
            audio_rx_stats(&st);
            const batt_t *bt = &a->batt.now;
            qlog("health", "%s %.6f MHz  rig %s  input %s %.0f Hz peak %.0f%% errors %ld restarts %ld  clock %s%s  battery %d%%",
                 mode_name(a->mode), a->dial_hz / 1e6, a->rig_ok ? "online" : "OFFLINE",
                 a->audio_ok ? "open" : "CLOSED", st.rate, audio_rx_level() * 100, st.errors, st.reopens,
                 clk_source() == CLK_GPS ? "GPS" : "system", clk_locked() ? " locked" : "", bt->present ? bt->percent : -1);
        }
        if (g_hw && a->rig_ok && a->tx_active) {
            float v;
            if (!rig_get_level("SWR", &v)) a->swr = v;
            if (!rig_get_level("RFPOWER_METER", &v)) a->power = v;
        }
        if (a->gps_ok && a->cfg.auto_grid) {
            gps_state_t g;
            gps_get(&g);
            if (g.fix && strncmp(a->cfg.grid, g.grid, 6)) { snprintf(a->cfg.grid, sizeof a->cfg.grid, "%s", g.grid); a->dirty = 1; }
        }
        if (a->mode == MODE_WSPR && a->wspr_beacon) {
            long slot = (long)floor((now + 5) / 120.0);
            if (slot != last_wspr_slot && fmod(now, 120.0) > 100.0) {   /* decide ~20 s ahead */
                last_wspr_slot = slot;
                if (rand() % 100 < a->cfg.wspr_tx_pct) {
                    pthread_mutex_lock(&a->lock);
                    if (a->ntxq < MAX_TXQ) a->txq[a->ntxq++] = (txitem_t){.kind = TXK_WSPR, .slot_odd = -1};
                    pthread_mutex_unlock(&a->lock);
                    app_status(a, "WSPR beacon queued for next even minute");
                }
            }
        }
        static int sec;
        if ((int)now != sec) { sec = (int)now; a->dirty = 1; }   /* clock tick */
    }
    return NULL;
}

int app_start_threads(app_t *a)
{
    pthread_create(&g_dec_thr, NULL, decode_thread, a);
    pthread_create(&g_tx_thr, NULL, tx_thread, a);
    pthread_create(&g_misc_thr, NULL, misc_thread, a);
    return 0;
}

void app_stop_threads(app_t *a)
{
    a->quit = 1;
    audio_tx_abort();
    pthread_join(g_tx_thr, NULL);
    pthread_join(g_dec_thr, NULL);
    pthread_join(g_misc_thr, NULL);
}

/* ------------------------------------------------------------------ */
/* User actions                                                         */
/* ------------------------------------------------------------------ */

void app_halt_tx(app_t *a)
{
    pthread_mutex_lock(&a->lock);
    a->ntxq = 0;
    a->wspr_beacon = 0;
    seq_reset(a);
    pthread_mutex_unlock(&a->lock);
    audio_tx_abort();
    if (g_hw && a->rig_ok) rig_ptt(0);
    app_status(a, "TX HALTED");
}

void app_tune(app_t *a, double seconds)
{
    pthread_mutex_lock(&a->lock);
    a->tune_until = clk_now() + seconds;
    a->ntxq = 0;
    a->txq[a->ntxq++] = (txitem_t){.kind = TXK_TUNE, .slot_odd = -1};
    pthread_mutex_unlock(&a->lock);
    app_status(a, "TUNE %.0f s at %d Hz", seconds, a->tx_off);
}

void app_toggle_wspr(app_t *a)
{
    a->wspr_beacon = !a->wspr_beacon;
    char g4[5];
    snprintf(g4, sizeof g4, "%s", a->cfg.grid);
    uint8_t tmp[WSPR_SYMBOLS];
    if (a->wspr_beacon && wspr_encode(a->cfg.callsign, g4, a->cfg.wspr_dbm, tmp)) {
        a->wspr_beacon = 0;
        app_status(a, "WSPR can't encode %s (type-1 calls only)", a->cfg.callsign);
        return;
    }
    app_status(a, a->wspr_beacon ? "WSPR beacon ON (%d%%)" : "WSPR beacon OFF", a->cfg.wspr_tx_pct);
}

void app_call_cq(app_t *a)
{
    if (mode_is_ft(a->mode)) { pthread_mutex_lock(&a->lock); seq_start_cq(a); pthread_mutex_unlock(&a->lock); }
    else if (a->mode == MODE_JS8) {
        snprintf(a->input, sizeof a->input, "CQ CQ CQ %.4s", a->cfg.grid);
        app_submit_input(a);
    }
}

void app_send_heartbeat(app_t *a)
{
    if (a->mode != MODE_JS8) return;
    snprintf(a->input, sizeof a->input, "@HB HEARTBEAT %.4s", a->cfg.grid);
    app_submit_input(a);
}

static int queue_js8_frames(app_t *a, const char *text, const char *selcall)
{
    js8_txframe_t f[MAX_TXQ];
    int n = js8v_build_frames(a->cfg.callsign, a->cfg.grid, selcall, text, 0, 0,
                              (js8_submode_t)a->js8_sub, f, MAX_TXQ);
    for (int i = 0; i < n && a->ntxq < MAX_TXQ; ++i) {
        txitem_t t = {.kind = TXK_JS8, .bits = f[i].bits, .slot_odd = -1};
        memcpy(t.text, f[i].text, 13);
        a->txq[a->ntxq++] = t;
    }
    if (n > 0) snprintf(a->js8_last_tx, sizeof a->js8_last_tx, "%s", text);
    return n;
}

void app_queue_js8_locked(app_t *a, const char *text)
{
    queue_js8_frames(a, text, "");
}

static void queue_js8(app_t *a, const char *text, const char *selcall)
{
    pthread_mutex_lock(&a->lock);
    int n = queue_js8_frames(a, text, selcall);
    pthread_mutex_unlock(&a->lock);
    if (n <= 0) { app_status(a, "nothing to send"); return; }
    const js8_submode_info_t *in = js8_submode_info((js8_submode_t)a->js8_sub);
    app_status(a, "JS8: %d frame%s (%d s)", n, n > 1 ? "s" : "", n * in->period_s);
}

static int handle_command(app_t *a, char *cmd)
{
    char *arg = strchr(cmd, ' ');
    if (arg) *arg++ = 0;
    else arg = "";
    for (char *p = cmd; *p; ++p) *p = (char)tolower((unsigned char)*p);
    int nb;
    const band_t *bt = band_table(&nb);
    if (!strcmp(cmd, "/band") || !strcmp(cmd, "/b")) {
        for (int i = 0; i < nb; ++i)
            if (*arg && !strncasecmp(bt[i].name, arg, strlen(arg)) && (bt[i].name[strlen(arg)] == 'm' || !bt[i].name[strlen(arg)])) { app_set_band(a, i); return 1; }
        app_status(a, "bands: 160 80 60 40 30 20 17 15 12 10 6");
    } else if (!strcmp(cmd, "/mode") || !strcmp(cmd, "/m")) {
        if (!strncasecmp(arg, "ft8", 3)) app_set_mode(a, MODE_FT8);
        else if (!strncasecmp(arg, "ft4", 3)) app_set_mode(a, MODE_FT4);
        else if (!strncasecmp(arg, "js8", 3)) app_set_mode(a, MODE_JS8);
        else if (!strncasecmp(arg, "wspr", 4)) app_set_mode(a, MODE_WSPR);
    } else if (!strcmp(cmd, "/sub")) {
        static const char *s[] = {"normal", "fast", "turbo", "slow"};
        for (int i = 0; i < 4; ++i) if (!strncasecmp(arg, s[i], 3)) { a->js8_sub = i; app_status(a, "JS8 %s", js8_submode_info(i)->name); }
    } else if (!strcmp(cmd, "/freq") || !strcmp(cmd, "/f")) {
        double mhz = atof(arg);
        if (mhz > 0.1 && mhz < 60) { a->dial_hz = (int64_t)llround(mhz * 1e6); if (g_hw && a->rig_ok) rig_set_freq(a->dial_hz); }
    } else if (!strcmp(cmd, "/call")) {
        for (char *p = arg; *p; ++p) *p = (char)toupper((unsigned char)*p);
        snprintf(a->cfg.callsign, sizeof a->cfg.callsign, "%s", arg);
    } else if (!strcmp(cmd, "/grid")) {
        snprintf(a->cfg.grid, sizeof a->cfg.grid, "%s", arg);
        a->cfg.auto_grid = 0;
    } else if (!strcmp(cmd, "/pwr")) {
        a->cfg.wspr_dbm = wspr_valid_dbm(atoi(arg));
    } else if (!strcmp(cmd, "/level")) {
        a->cfg.tx_level = (float)fmax(0.01, fmin(1.0, atof(arg) / 100.0));
        app_status(a, "TX level %.0f%%", a->cfg.tx_level * 100);
    } else if (!strcmp(cmd, "/tune")) app_tune(a, *arg ? atof(arg) : 5);
    else if (!strcmp(cmd, "/cq")) app_call_cq(a);
    else if (!strcmp(cmd, "/hb")) app_send_heartbeat(a);
    else if (!strcmp(cmd, "/wspr")) app_toggle_wspr(a);
    else if (!strcmp(cmd, "/save")) app_status(a, config_save(&a->cfg, config_default_path()) ? "save failed" : "saved");
    else if (!strcmp(cmd, "/tx")) { a->tx_off = atoi(arg); }
    else if (!strcmp(cmd, "/rx")) { a->rx_off = atoi(arg); }
    else if (!strcmp(cmd, "/info")) { for (char *p = arg; *p; ++p) *p = (char)toupper((unsigned char)*p); snprintf(a->cfg.js8_info, sizeof a->cfg.js8_info, "%s", arg); app_status(a, "INFO: %s", arg); }
    else if (!strcmp(cmd, "/status")) { for (char *p = arg; *p; ++p) *p = (char)toupper((unsigned char)*p); snprintf(a->cfg.js8_status, sizeof a->cfg.js8_status, "%s", arg); app_status(a, "STATUS: %s", arg); }
    else if (!strcmp(cmd, "/auto")) { a->cfg.js8_autoreply = !strncasecmp(arg, "on", 2); app_status(a, "JS8 auto-reply %s", a->cfg.js8_autoreply ? "ON" : "off"); }
    else if (!strcmp(cmd, "/hback")) { a->cfg.js8_hb_ack = !strncasecmp(arg, "on", 2); app_status(a, "JS8 heartbeat ACK %s", a->cfg.js8_hb_ack ? "ON" : "off"); }
    else if (!strcmp(cmd, "/net")) {
        uplink_stats_t u;
        uplink_get(&u);
        uplink_check_now();
        app_status(a, "wsprnet: %s, sent %d (https %d) dropped %d - checking now", u.online ? "online" : "offline",
                   u.sent_https + u.sent_http, u.sent_https, u.dropped);
    }
    else if (!strcmp(cmd, "/port")) app_use_rig_device(a, strcmp(arg, "auto") ? arg : "");
    else if (!strcmp(cmd, "/ain")) app_use_audio(a, strcmp(arg, "auto") ? arg : "", NULL);
    else if (!strcmp(cmd, "/aout")) app_use_audio(a, NULL, strcmp(arg, "auto") ? arg : "");
    else if (!strcmp(cmd, "/cqmod")) app_set_cq_mod(a, arg);
    else if (!strcmp(cmd, "/cqonly") || !strcmp(cmd, "/filter")) {
        a->cfg.cq_only = !strcasecmp(arg, "on") ? 1 : !strcasecmp(arg, "off") ? 0 : !a->cfg.cq_only;
        app_status(a, a->cfg.cq_only ? "showing CQ calls only (/cqonly again for all)" : "showing all decodes");
        if (a->cfg.cq_only && !mode_is_ft(a->mode)) app_status(a, "CQ-only filter on: applies in FT8 and FT4");
    }
    else if (!strcmp(cmd, "/rescan")) app_rescan_audio(a);
    else if (!strcmp(cmd, "/inlevel") || !strcmp(cmd, "/outlevel")) {
        int cap = cmd[1] == 'i';
        mixer_level_t *m = cap ? &a->mix_in : &a->mix_out;
        app_audio_level(a, cap, atoi(arg) - (m->ok ? m->percent : 0));
    }
    else if (!strcmp(cmd, "/aprspos") || !strcmp(cmd, "/aprsgrid")) app_aprs_position(a);
    else if (!strcmp(cmd, "/aprs") || !strcmp(cmd, "/sms") || !strcmp(cmd, "/email")) {
        char first[64] = "";
        const char *rest = arg;
        int n = 0;
        while (*rest == ' ') ++rest;
        while (*rest && *rest != ' ' && n < (int)sizeof first - 1) first[n++] = *rest++;
        first[n] = 0;
        while (*rest == ' ') ++rest;
        if (!strcmp(cmd, "/aprs")) app_aprs_message(a, first, rest);
        else if (!strcmp(cmd, "/sms")) app_aprs_sms(a, first, rest);
        else app_aprs_email(a, first, rest);
    }
    else if (!strcmp(cmd, "/ghost") || !strcmp(cmd, "/ghostnet")) {
        int bn = atoi(arg);
        if (!strncasecmp(arg, "off", 3)) app_set_ghostnet(a, 0);
        else if (bn == 80) app_ghostnet_band(a, 1);
        else if (bn == 40) app_ghostnet_band(a, 3);
        else if (bn == 20) app_ghostnet_band(a, 5);
        else app_set_ghostnet(a, 1);
    }
    else if (!strcmp(cmd, "/qsy")) {
        a->cfg.auto_qsy = !strncasecmp(arg, "on", 2);
        app_status(a, "auto frequency %s", a->cfg.auto_qsy ? "ON" : "off");
        if (a->cfg.auto_qsy) app_set_band(a, a->band);
    }
    else if (!strcmp(cmd, "/quit")) a->quit = 1;
    else if (!strcmp(cmd, "/help")) a->screen = 3;
    else app_status(a, "unknown: %s (try /help)", cmd);
    return 1;
}

void app_submit_input(app_t *a)
{
    char text[160];
    snprintf(text, sizeof text, "%s", a->input);
    a->input[0] = 0;
    a->input_len = 0;
    a->dirty = 1;
    char *s = text;
    while (*s == ' ') ++s;
    if (!*s) { app_reply_selected(a); return; }
    if (*s == '/') { handle_command(a, s); return; }
    for (char *p = s; *p; ++p) *p = (char)toupper((unsigned char)*p);
    if (mode_is_ft(a->mode)) {
        uint8_t tones[105];
        if (ft8w_encode(a->mode == MODE_FT4 ? FT8W_FT4 : FT8W_FT8, s, tones) < 0) { app_status(a, "not a valid %s message", mode_name(a->mode)); return; }
        pthread_mutex_lock(&a->lock);
        if (seq_start_typed(a, s)) { pthread_mutex_unlock(&a->lock); return; }
        if (a->ntxq < MAX_TXQ) a->txq[a->ntxq++] = (txitem_t){.kind = TXK_FT8, .slot_odd = a->seq.st != SEQ_IDLE ? a->seq.slot_odd : 0};
        snprintf(a->txq[a->ntxq - 1].text, sizeof a->txq[0].text, "%s", s);
        pthread_mutex_unlock(&a->lock);
        app_status(a, "queued: %s", s);
    } else if (a->mode == MODE_JS8) {
        queue_js8(a, s, "");
    } else {
        app_status(a, "WSPR is beacon-only: /wspr toggles");
    }
}

void app_reply_selected(app_t *a)
{
    if (a->sel < 0 || a->sel >= a->nlines) return;
    rxline_t l = a->lines[a->sel];
    if (l.flags & (LINE_TX | LINE_SYS)) return;
    if (mode_is_ft(a->mode)) {
        pthread_mutex_lock(&a->lock);
        seq_start_reply(a, &l);
        pthread_mutex_unlock(&a->lock);
    } else if (a->mode == MODE_JS8) {
        /* prefill a directed message to the sender */
        char call[16] = "";
        sscanf(l.text, "%15[^:]", call);
        if (call[0]) { snprintf(a->input, sizeof a->input, "%s ", call); a->input_len = (int)strlen(a->input); }
        a->rx_off = a->tx_off = (int)l.freq;
    }
    a->dirty = 1;
}
