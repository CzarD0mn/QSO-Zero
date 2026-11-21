#include "audio.h"
#include "core/clock.h"
#include <ctype.h>
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- minimal ALSA ABI (values are stable across alsa-lib versions) ---- */
typedef struct snd_pcm snd_pcm_t;
#define SND_PCM_STREAM_PLAYBACK 0
#define SND_PCM_STREAM_CAPTURE 1
#define SND_PCM_FORMAT_S16_LE 2
#define SND_PCM_ACCESS_RW_INTERLEAVED 3

static struct {
    void *h;
    int (*open)(snd_pcm_t **, const char *, int, int);
    int (*set_params)(snd_pcm_t *, int, int, unsigned, unsigned, int, unsigned);
    long (*readi)(snd_pcm_t *, void *, unsigned long);
    long (*writei)(snd_pcm_t *, const void *, unsigned long);
    int (*recover)(snd_pcm_t *, int, int);
    int (*drain)(snd_pcm_t *);
    int (*drop)(snd_pcm_t *);
    int (*prepare)(snd_pcm_t *);
    int (*close)(snd_pcm_t *);
    const char *(*strerror)(int);
    long (*delay)(snd_pcm_t *, long *);
} A;

static snd_pcm_t *g_in, *g_out;
static char g_out_dev[96];
static pthread_t g_thr;
static volatile int g_run, g_abort;
static audio_rx_cb g_cb;
static void *g_user;
static char g_err[160];
static volatile float g_level;
static volatile long g_frames, g_errors;
static double g_rate_t0;
static long g_rate_f0;
static volatile double g_rate;
static char g_rx_err[80];

void audio_rx_stats(audio_rx_stats_t *o)
{
    o->rate = g_in ? g_rate : 0;
    o->errors = g_errors;
    snprintf(o->last_error, sizeof o->last_error, "%s", g_rx_err);
}

const char *audio_error(void) { return g_err; }
float audio_rx_level(void) { return g_level; }
void audio_tx_abort(void) { g_abort = 1; }

static int load_alsa(void)
{
    if (A.h) return 0;
    A.h = dlopen("libasound.so.2", RTLD_NOW);
    if (!A.h) { snprintf(g_err, sizeof g_err, "libasound.so.2 not found"); return -1; }
#define SYM(f, n) if (!(*(void **)&A.f = dlsym(A.h, n))) { snprintf(g_err, sizeof g_err, "missing %s", n); return -1; }
    SYM(open, "snd_pcm_open");
    SYM(set_params, "snd_pcm_set_params");
    SYM(readi, "snd_pcm_readi");
    SYM(writei, "snd_pcm_writei");
    SYM(recover, "snd_pcm_recover");
    SYM(drain, "snd_pcm_drain");
    SYM(drop, "snd_pcm_drop");
    SYM(prepare, "snd_pcm_prepare");
    SYM(close, "snd_pcm_close");
    SYM(strerror, "snd_strerror");
    SYM(delay, "snd_pcm_delay");
#undef SYM
    return 0;
}

/* ---- 4:1 polyphase decimator: 95-tap windowed-sinc, fc = 5.2 kHz @ 48 kHz.
 * Stopband > 70 dB above 6 kHz so nothing aliases into the 0-3 kHz passband. */
#define NTAPS 95
static float g_taps[NTAPS];
static int g_taps_ready;

static void make_taps(void)
{
    if (g_taps_ready) return;
    double fc = 5200.0 / 48000.0, sum = 0;
    for (int i = 0; i < NTAPS; ++i) {
        double m = i - (NTAPS - 1) / 2.0;
        double sinc = m == 0 ? 2 * fc : sin(2 * M_PI * fc * m) / (M_PI * m);
        /* Blackman-Harris window */
        double w = 0.35875 - 0.48829 * cos(2 * M_PI * i / (NTAPS - 1)) +
                   0.14128 * cos(4 * M_PI * i / (NTAPS - 1)) - 0.01168 * cos(6 * M_PI * i / (NTAPS - 1));
        g_taps[i] = (float)(sinc * w);
        sum += g_taps[i];
    }
    for (int i = 0; i < NTAPS; ++i) g_taps[i] /= (float)sum;
    g_taps_ready = 1;
}

void decim4_init(decim4_t *d)
{
    make_taps();
    memset(d, 0, sizeof *d);
}

int decim4_run(decim4_t *d, const int16_t *in, int n, int16_t *out)
{
    /* history is stored twice (pos and pos+NTAPS) so the dot product is one
     * contiguous, vectorisable loop with no modulo */
    int no = 0;
    for (int i = 0; i < n; ++i) {
        float x = in[i];
        d->hist[d->pos] = x;
        d->hist[d->pos + NTAPS] = x;
        if (++d->pos == NTAPS) d->pos = 0;
        if (++d->phase == 4) {
            d->phase = 0;
            const float *h = d->hist + d->pos;   /* oldest .. newest */
            float acc = 0;
            for (int t = 0; t < NTAPS; ++t) acc += g_taps[t] * h[t];
            if (acc > 32767) acc = 32767;
            if (acc < -32768) acc = -32768;
            out[no++] = (int16_t)lrintf(acc);
        }
    }
    return no;
}

/* ---- capture thread ---- */

static void *rx_thread(void *arg)
{
    (void)arg;
    enum { CHUNK = 960 };  /* 20 ms at 48 kHz */
    int16_t buf[CHUNK], dec[CHUNK / 4 + 4];
    decim4_t d;
    decim4_init(&d);
    float peak = 0;
    int peak_n = 0;
    while (g_run) {
        long r = A.readi(g_in, buf, CHUNK);
        double t_end = clk_now();
        if (r < 0) {
            ++g_errors;
            snprintf(g_rx_err, sizeof g_rx_err, "%s", A.strerror((int)r));
            if (A.recover(g_in, (int)r, 1) < 0) { struct timespec ts = {0, 50000000}; nanosleep(&ts, NULL); }
            continue;
        }
        g_frames += r;
        double tm = clk_mono();
        if (tm - g_rate_t0 >= 2.0) {                       /* samples/s over ~2 s */
            g_rate = g_rate_t0 > 0 ? (g_frames - g_rate_f0) / (tm - g_rate_t0) : 0;
            g_rate_t0 = tm; g_rate_f0 = g_frames;
        }
        for (long i = 0; i < r; ++i) {
            float a = fabsf(buf[i] / 32768.0f);
            if (a > peak) peak = a;
        }
        if ((peak_n += (int)r) >= 24000) { g_level = peak; peak = 0; peak_n = 0; }
        int nd = decim4_run(&d, buf, (int)r, dec);
        /* time of first output sample: end time minus the chunk, minus the
         * FIR group delay ((NTAPS-1)/2 input samples). */
        double t0 = t_end - (double)r / AUDIO_HW_RATE - ((NTAPS - 1) / 2.0) / AUDIO_HW_RATE;
        if (nd && g_cb) g_cb(dec, nd, t0, g_user);
    }
    return NULL;
}

int audio_open(const char *in_dev, const char *out_dev, audio_rx_cb cb, void *user)
{
    snprintf(g_out_dev, sizeof g_out_dev, "%s", out_dev);
    if (load_alsa()) return -1;
    int e;
    if ((e = A.open(&g_in, in_dev, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        snprintf(g_err, sizeof g_err, "capture %s: %s", in_dev, A.strerror(e));
        return -1;
    }
    if ((e = A.set_params(g_in, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, AUDIO_HW_RATE, 1, 100000)) < 0) {
        snprintf(g_err, sizeof g_err, "capture params: %s", A.strerror(e));
        A.close(g_in); g_in = NULL;
        return -1;
    }
    g_cb = cb;
    g_user = user;
    g_frames = g_errors = 0; g_rate = 0; g_rate_t0 = 0; g_rate_f0 = 0; g_rx_err[0] = 0;
    g_run = 1;
    pthread_create(&g_thr, NULL, rx_thread, NULL);
    return 0;
}

void audio_close(void)
{
    if (!g_in) return;
    g_run = 0;
    pthread_join(g_thr, NULL);
    A.close(g_in);
    g_in = NULL;
}

void audio_set_output(const char *out_dev) { snprintf(g_out_dev, sizeof g_out_dev, "%s", out_dev); }

int audio_play(const float *buf, size_t n, float gain)
{
    if (load_alsa()) return -1;
    int e;
    if ((e = A.open(&g_out, g_out_dev, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        snprintf(g_err, sizeof g_err, "playback %s: %s", g_out_dev, A.strerror(e));
        return -1;
    }
    if ((e = A.set_params(g_out, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, AUDIO_HW_RATE, 1, 60000)) < 0) {
        snprintf(g_err, sizeof g_err, "playback params: %s", A.strerror(e));
        A.close(g_out); g_out = NULL;
        return -1;
    }
    g_abort = 0;
    enum { CH = 1200 };
    int16_t pcm[CH];
    size_t pos = 0;
    int rc = 0;
    while (pos < n) {
        if (g_abort) { rc = 1; break; }
        size_t m = n - pos < CH ? n - pos : CH;
        for (size_t i = 0; i < m; ++i) {
            float v = buf[pos + i] * gain;
            if (v > 0.999f) v = 0.999f;
            if (v < -0.999f) v = -0.999f;
            pcm[i] = (int16_t)lrintf(v * 32767.0f);
        }
        long w = A.writei(g_out, pcm, m);
        if (w < 0) { if (A.recover(g_out, (int)w, 1) < 0) { rc = -1; break; } continue; }
        pos += (size_t)w;
    }
    if (rc == 1) A.drop(g_out); else A.drain(g_out);
    A.close(g_out);
    g_out = NULL;
    return rc;
}

int audio_find_card(const char *hints, char *out, size_t outsz)
{
    FILE *f = fopen("/proc/asound/cards", "r");
    if (!f) return -1;
    char line[256], h[128];
    int found = -1;
    while (fgets(line, sizeof line, f)) {
        /* " 1 [QMX            ]: USB-Audio - QMX" */
        char *lb = strchr(line, '['), *rb = lb ? strchr(lb, ']') : NULL;
        if (!lb || !rb) continue;
        snprintf(h, sizeof h, "%s", hints);
        for (char *tok = strtok(h, "|"); tok; tok = strtok(NULL, "|")) {
            if (strstr(line, tok)) {
                char id[32];
                size_t L = (size_t)(rb - lb - 1);
                if (L >= sizeof id) L = sizeof id - 1;
                memcpy(id, lb + 1, L);
                id[L] = 0;
                while (L && id[L - 1] == ' ') id[--L] = 0;
                snprintf(out, outsz, "plughw:CARD=%s", id);
                found = 0;
                break;
            }
        }
        if (!found) break;
    }
    fclose(f);
    return found;
}
