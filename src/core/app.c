/* NeonQRP - engine: audio ring, waterfall, decode and transmit threads. */
#include "app.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "audio/audio.h"
#include "core/clock.h"
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

static const band_t k_bands[] = {
    {"160m", 1840000, 1842000, 1836600},   {"80m", 3573000, 3578000, 3568600},
    {"60m", 5357000, 5357000, 5287200},    {"40m", 7074000, 7078000, 7038600},
    {"30m", 10136000, 10130000, 10138700}, {"20m", 14074000, 14078000, 14095600},
    {"17m", 18100000, 18104000, 18104600}, {"15m", 21074000, 21078000, 21094600},
    {"12m", 24915000, 24922000, 24924600}, {"10m", 28074000, 28078000, 28124600},
    {"6m", 50313000, 50318000, 50293000},
};

const band_t *band_table(int *count)
{
    *count = (int)(sizeof k_bands / sizeof *k_bands);
    return k_bands;
}

const char *mode_name(op_mode_t m)
{
    static const char *n[] = {"FT8", "JS8", "WSPR"};
    return m < MODE_COUNT ? n[m] : "?";
}

int mode_period(const app_t *a)
{
    if (a->mode == MODE_FT8) return 15;
    if (a->mode == MODE_WSPR) return WSPR_PERIOD_S;
    return js8_submode_info((js8_submode_t)a->js8_sub)->period_s;
}

void app_status(app_t *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->status, sizeof a->status, fmt, ap);
    va_end(ap);
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

void app_set_band(app_t *a, int band)
{
    int nb;
    const band_t *bt = band_table(&nb);
    if (band < 0) band = nb - 1;
    if (band >= nb) band = 0;
    a->band = band;
    a->dial_hz = a->mode == MODE_FT8 ? bt[band].ft8 : a->mode == MODE_JS8 ? bt[band].js8 : bt[band].wspr;
    if (g_hw && a->rig_ok) {
        if (rig_set_freq(a->dial_hz)) app_status(a, "QSY failed: %s", rig_error());
        rig_set_mode(a->radio->data_mode, 0);
    }
    a->dirty = 1;
}

void app_set_mode(app_t *a, op_mode_t m)
{
    a->mode = m;
    if (m == MODE_WSPR) { a->tx_off = 1500; a->rx_off = 1500; }
    seq_reset(a);
    a->ntxq = 0;
    app_set_band(a, a->band);
    app_status(a, "%s %s", mode_name(m), m == MODE_JS8 ? js8_submode_info(a->js8_sub)->name : "");
}

int app_init(app_t *a, const char *cfgpath, int hw)
{
    memset(a, 0, sizeof *a);
    g_app = a;
    pthread_mutex_init(&a->lock, NULL);
    g_hw = hw;
    config_defaults(&a->cfg);
    config_load(&a->cfg, cfgpath);
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
    if (js8v_load_jsc(a->cfg.jsc_path) && js8v_load_jsc("data/js8.jsc") && js8v_load_jsc("/usr/share/neonqrp/js8.jsc"))
        app_status(a, "js8.jsc not found: JS8 free text limited to Huffman");

    if (hw) {
        rig_config_t rc = {.profile = a->radio, .baud = a->cfg.rig_baud, .spawn = a->cfg.spawn_rigctld, .port = 4532};
        snprintf(rc.serial, sizeof rc.serial, "%s", a->cfg.rig_device[0] ? a->cfg.rig_device : a->radio->serial_default);
        rc.ptt = a->radio->ptt;
        if (!strcmp(a->cfg.ptt, "cat")) rc.ptt = PTT_CAT;
        if (!strcmp(a->cfg.ptt, "rts")) rc.ptt = PTT_RTS;
        if (!strcmp(a->cfg.ptt, "dtr")) rc.ptt = PTT_DTR;
        snprintf(rc.ptt_device, sizeof rc.ptt_device, "%s", a->cfg.ptt_device);
        a->rig_ok = rig_open(&rc) == 0;
        rig_install_safety();
        if (!a->rig_ok) app_status(a, "RIG: %s", rig_error());

        char in[96], out[96];
        snprintf(in, sizeof in, "%s", a->cfg.audio_in);
        snprintf(out, sizeof out, "%s", a->cfg.audio_out);
        if (!in[0] && audio_find_card(a->radio->audio_hint, in, sizeof in)) snprintf(in, sizeof in, "default");
        if (!out[0]) snprintf(out, sizeof out, "%s", in);
        a->audio_ok = audio_open(in, out, app_audio_in, a) == 0;
        if (!a->audio_ok) app_status(a, "AUDIO: %s", audio_error());

        a->gps_ok = gps_start(a->cfg.gps_device, a->cfg.gps_baud, a->cfg.gps_latency_ms) == 0;
        if (a->cfg.wsprnet_upload) uplink_start(a->cfg.wsprnet_host, 443, 80, my_call, my_grid);
    }
    app_set_mode(a, MODE_FT8);
    return 0;
}

void app_shutdown(app_t *a)
{
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

static void sleep_until(app_t *a, double utc)
{
    for (;;) {
        double d = utc - clk_now();
        if (d <= 0 || a->quit) return;
        struct timespec ts = {0, (long)(fmin(d, 0.1) * 1e9)};
        nanosleep(&ts, NULL);
    }
}

static void *decode_thread(void *arg)
{
    app_t *a = arg;
    ft8w_t *ft8 = ft8w_new(FT8W_FT8, 12000, 100, 3000);
    js8_decoder_t *js8 = NULL;
    int js8_sub = -1;
    int16_t *buf = malloc(sizeof(int16_t) * WSPRRX_SAMPLES);   /* largest window: WSPR 114 s */
    while (!a->quit) {
        op_mode_t mode = a->mode;
        int P = mode_period(a);
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
        else {
            const js8_submode_info_t *in = js8_submode_info((js8_submode_t)a->js8_sub);
            ready = in->start_delay_ms / 1000.0 + 79.0 * in->symbol_samples / 12000.0 + 0.35;
        }
        if (clk_now() > start + ready) start += P;      /* already past it: next period */
        double trigger = start + ready;
        sleep_until(a, trigger);
        if (a->quit || a->mode != mode) continue;
        a->decoding = 1;
        a->dirty = 1;
        double t0 = clk_mono();
        if (mode == MODE_FT8) {
            int n = (int)((ready - 0.1) * 12000);
            if (ring_extract(a, start, n, buf) == 0) {
                ft8w_decode_t d[60];
                int nd = ft8w_decode(ft8, buf, n, d, 60);
                int odd = ((long long)(start / 15)) & 1;
                pthread_mutex_lock(&a->lock);
                for (int i = 0; i < nd; ++i) {
                    rxline_t l = {.utc = start, .snr = d[i].snr, .dt = d[i].dt, .freq = d[i].freq, .mode = MODE_FT8, .slot_odd = odd};
                    snprintf(l.text, sizeof l.text, "%s", d[i].text);
                    if (!strncmp(l.text, "CQ ", 3)) l.flags |= LINE_CQ;
                    size_t cl = strlen(a->cfg.callsign);
                    if (!strncmp(l.text, a->cfg.callsign, cl) && l.text[cl] == ' ') l.flags |= LINE_TOME;
                    app_add_line(a, &l);
                    seq_on_decode(a, &l);
                }
                seq_after_cycle(a, odd);
                pthread_mutex_unlock(&a->lock);
            }
        } else if (mode == MODE_JS8) {
            if (js8_sub != a->js8_sub) {
                js8_decoder_free(js8);
                js8_sub = a->js8_sub;
                js8 = js8_decoder_new((js8_submode_t)js8_sub);
            }
            int n = (int)((ready - 0.1) * 12000);
            if (js8 && ring_extract(a, start, n, buf) == 0) {
                js8_decode_t d[40];
                int nd = js8_decode(js8, buf, n, 200, 2900, a->rx_off, d, 40);
                pthread_mutex_lock(&a->lock);
                for (int i = 0; i < nd; ++i)
                    js8_assemble(a, d[i].text, d[i].frame_type, d[i].snr, d[i].freq, d[i].dt, start);
                pthread_mutex_unlock(&a->lock);
            }
        }
        a->last_decode_ms = (clk_mono() - t0) * 1000.0;
        a->decoding = 0;
        a->dirty = 1;
    }
    ft8w_free(ft8);
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
    if (t->kind == TXK_FT8) {
        double s = floor(now / 15) * 15;
        int idx = (int)((long long)(s / 15) & 1);
        double pos = now - s;
        /* late start is fine up to ~2 s (decoders search DT +/-2.5 s) */
        if ((t->slot_odd < 0 || idx == t->slot_odd) && pos < 2.0) return pos < 0.5 ? s + 0.5 : now + 0.05;
        for (int k = 1; k <= 2; ++k) {
            double c = s + 15 * k;
            int ci = (int)((long long)(c / 15) & 1);
            if (t->slot_odd < 0 || ci == t->slot_odd) return c + 0.5;
        }
        return s + 15.5;
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
        int nt = ft8w_encode(FT8W_FT8, t->text, tones);
        if (nt < 0) return NULL;
        buf = malloc(sizeof(float) * (size_t)(nt * 0.16 * R + R));
        *n = ft8w_synth(FT8W_FT8, tones, nt, f0, R, 1.0f, buf);
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
        if (g_hw && a->rig_ok && rig_ptt(1)) { app_status(a, "PTT failed: %s", rig_error()); free(audio); goto pop; }
        sleep_until(a, start);
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
        if (g_hw && a->audio_ok) {
            int rc = audio_play(audio, n, a->cfg.tx_level);
            if (rc < 0) app_status(a, "TX audio: %s", audio_error());
        } else {
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
    double last_poll = 0;
    long last_wspr_slot = -1;
    srand((unsigned)time(NULL));
    while (!a->quit) {
        struct timespec ts = {0, 250000000};
        nanosleep(&ts, NULL);
        double now = clk_now();
        if (g_hw && a->rig_ok && !a->tx_active && clk_mono() - last_poll > 2.0) {
            int64_t f = rig_get_freq();
            if (f > 0 && f != a->dial_hz) { a->dial_hz = f; a->dirty = 1; }
            last_poll = clk_mono();
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
    if (a->mode == MODE_FT8) { pthread_mutex_lock(&a->lock); seq_start_cq(a); pthread_mutex_unlock(&a->lock); }
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
            if (!strncasecmp(bt[i].name, arg, strlen(arg)) && *arg) { app_set_band(a, i); return 1; }
        app_status(a, "bands: 160 80 60 40 30 20 17 15 12 10 6");
    } else if (!strcmp(cmd, "/mode") || !strcmp(cmd, "/m")) {
        if (!strncasecmp(arg, "ft8", 3)) app_set_mode(a, MODE_FT8);
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
    if (a->mode == MODE_FT8) {
        uint8_t tones[105];
        if (ft8w_encode(FT8W_FT8, s, tones) < 0) { app_status(a, "not a valid FT8 message"); return; }
        pthread_mutex_lock(&a->lock);
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
    if (a->mode == MODE_FT8) {
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
