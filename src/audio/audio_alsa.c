#include "audio.h"
#include "core/clock.h"
#include "core/log.h"
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
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
    int (*wait)(snd_pcm_t *, int);
    int (*start)(snd_pcm_t *);
} A;

static snd_pcm_t *g_in, *g_out;
static char g_out_dev[96];
static pthread_t g_thr;
static volatile int g_run, g_abort;
static audio_rx_cb g_cb;
static void *g_user;
static char g_err[160];
static volatile float g_level;
static volatile long g_frames, g_errors, g_reopens;
static char g_in_dev[96];
static int g_thread_on;
static volatile int g_rx_pause, g_rx_paused;   /* TX asks RX to let go of a half-duplex USB codec */
static double g_rate_t0;
static long g_rate_f0;
static volatile double g_rate;
static char g_rx_err[80];
static int g_in_ch = 1;            /* channels the input was opened with (2 = stereo card) */

void audio_rx_stats(audio_rx_stats_t *o)
{
    o->rate = g_in ? g_rate : 0;
    o->errors = g_errors;
    o->reopens = g_reopens;
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
    SYM(wait, "snd_pcm_wait");
    SYM(start, "snd_pcm_start");
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

/* ---- capture ---- */

static void sleep_ms(int ms) { struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&ts, NULL); }

/* Open + configure, retrying: some USB codecs (seen: the C-Media chip in the
 * Xiegu X6200 on the Cardputer Zero) intermittently fail to start a stream
 * with USB error -71 ("cannot set freq 48000"); a retry usually works. */
#define SND_PCM_NONBLOCK 1
/* Capture is opened in stereo when the card allows it, and one channel is
 * used (see rx_thread). Letting ALSA mix a stereo card down to mono averages
 * the channels, and some radios (the QRP Labs QMX) send the right channel as
 * an inverted copy of the left: averaged, that is exact silence. Mono cards
 * (C-Media in the X6100/X6200/Digirig) are opened in stereo through plughw too,
 * which just duplicates the channel. Playback stays mono. */
static int open_pcm(snd_pcm_t **pcm, const char *dev, int stream, int mode, unsigned latency_us, int tries)
{
    int e = -1;
    for (int t = 0; t < tries; ++t) {
        if (t) sleep_ms(300);
        const char *what = stream == SND_PCM_STREAM_CAPTURE ? "input" : "output";
        if ((e = A.open(pcm, dev, stream, mode)) < 0) {
            qlog("audio", "%s %s: open failed (try %d/%d): %s", what, dev, t + 1, tries, A.strerror(e));
            *pcm = NULL;
            continue;
        }
        int ch = stream == SND_PCM_STREAM_CAPTURE ? 2 : 1;
        if ((e = A.set_params(*pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, ch, AUDIO_HW_RATE, 1, latency_us)) < 0 && ch == 2) {
            A.close(*pcm);                                  /* no stereo on this input: mono */
            ch = 1;
            if ((e = A.open(pcm, dev, stream, mode)) >= 0)
                e = A.set_params(*pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, AUDIO_HW_RATE, 1, latency_us);
            else *pcm = NULL;
        }
        if (e < 0) {
            qlog("audio", "%s %s: 48 kHz 16-bit setup failed (try %d/%d): %s", what, dev, t + 1, tries, A.strerror(e));
            if (*pcm) A.close(*pcm);
            *pcm = NULL;
            continue;
        }
        if (stream == SND_PCM_STREAM_CAPTURE) g_in_ch = ch;
        /* snd_pcm_set_params() sets the start threshold to the whole buffer, so a
         * capture stream read in 20 ms pieces would never start: start it now. */
        if (stream == SND_PCM_STREAM_CAPTURE && (e = A.start(*pcm)) < 0) {
            qlog("audio", "%s %s: start failed (try %d/%d): %s", what, dev, t + 1, tries, A.strerror(e));
            A.close(*pcm); *pcm = NULL;
            continue;
        }
        if (t || stream == SND_PCM_STREAM_CAPTURE)
            qlog("audio", "%s %s open and running (48 kHz %s)%s", what, dev, stream == SND_PCM_STREAM_CAPTURE && ch == 2 ? "stereo, one channel used" : "mono",
                 t ? " after retrying" : "");
        return 0;
    }
    return e;
}

/* Use the louder channel of a stereo input; the left one unless the right is
 * clearly (3 dB) stronger, e.g. a card that only fills the right channel. */
int audio_pick_channel(double energy_left, double energy_right)
{
    return energy_right > 2.0 * energy_left ? 1 : 0;
}

static void *rx_thread(void *arg)
{
    (void)arg;
    enum { CHUNK = 960 };  /* 20 ms at 48 kHz */
    int16_t raw[CHUNK * 2], buf[CHUNK], dec[CHUNK / 4 + 4];
    /* which channel of a stereo input to use: measured over the first 3 s */
    double e_ch[2] = {0, 0}, e_mix = 0;
    float pk_ch[2] = {0, 0}, pk_mix = 0;
    long n_meas = 0;
    int use = 0, decided = 0;
    decim4_t d;
    decim4_init(&d);
    float peak = 0;
    int peak_n = 0, consec = 0;
    double last_ok = clk_mono();
    while (g_run) {
        if (g_rx_pause) {                                   /* transmitting on a half-duplex codec */
            if (g_in) { A.close(g_in); g_in = NULL; }
            g_rx_paused = 1;
            g_level = 0;
            sleep_ms(10);
            last_ok = clk_mono();
            continue;
        }
        g_rx_paused = 0;
        if (!g_in) {                                        /* reopening after a stall */
            int e = open_pcm(&g_in, g_in_dev, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK, 100000, 3);
            if (e < 0) { snprintf(g_rx_err, sizeof g_rx_err, "reopen: %s", A.strerror(e)); sleep_ms(1500); continue; }
            decim4_init(&d);
            last_ok = clk_mono();
            consec = 0;
        }
        int w = A.wait(g_in, 500);
        long r = w < 0 ? w : A.readi(g_in, raw, CHUNK);
        if (r > 0) {
            if (g_in_ch == 2) {
                if (!decided) {
                    for (long i = 0; i < r; ++i) {
                        float l = raw[2 * i] / 32768.0f, rr = raw[2 * i + 1] / 32768.0f, m = (l + rr) / 2;
                        e_ch[0] += l * l; e_ch[1] += rr * rr; e_mix += m * m;
                        if (fabsf(l) > pk_ch[0]) pk_ch[0] = fabsf(l);
                        if (fabsf(rr) > pk_ch[1]) pk_ch[1] = fabsf(rr);
                        if (fabsf(m) > pk_mix) pk_mix = fabsf(m);
                    }
                    if ((n_meas += r) >= 3L * AUDIO_HW_RATE) {
                        use = audio_pick_channel(e_ch[0], e_ch[1]);
                        decided = 1;
                        qlog("audio", "input is stereo: left peak %.2f%%, right peak %.2f%%, both mixed %.2f%%%s - using the %s channel",
                             pk_ch[0] * 100, pk_ch[1] * 100, pk_mix * 100,
                             e_mix < 0.01 * (e_ch[0] + e_ch[1]) / 2 && e_ch[0] + e_ch[1] > 0 ? " (the channels cancel when mixed)" : "",
                             use ? "right" : "left");
                    }
                }
                for (long i = 0; i < r; ++i) buf[i] = raw[2 * i + use];
            } else memcpy(buf, raw, sizeof(int16_t) * (size_t)r);
        }
        double t_end = clk_now(), tm = clk_mono();
        if (r == -EAGAIN || r == 0) r = 0;
        else if (r < 0) {
            ++g_errors; ++consec;
            snprintf(g_rx_err, sizeof g_rx_err, "%s", A.strerror((int)r));
            if (g_errors <= 10 || g_errors % 500 == 0) qlog("audio", "input read error #%ld: %s", g_errors, g_rx_err);
            if (A.recover(g_in, (int)r, 1) < 0) sleep_ms(50);
            else A.start(g_in);                             /* recover re-prepares; capture must be started again */
        } else {
            consec = 0;
            last_ok = tm;
            g_frames += r;
        }
        if (tm - g_rate_t0 >= 2.0) {                       /* samples/s over ~2 s */
            g_rate = g_rate_t0 > 0 ? (g_frames - g_rate_f0) / (tm - g_rate_t0) : 0;
            g_rate_t0 = tm; g_rate_f0 = g_frames;
        }
        /* watchdog: nothing for 3 s, or errors in a row -> close and reopen */
        if (consec >= 5 || tm - last_ok > 3.0) {
            qlog("audio", "input %s: %s - restarting it (restart #%ld)", g_in_dev,
                 consec >= 5 ? "errors in a row" : "no samples for 3 s", g_reopens + 1);
            A.close(g_in);
            g_in = NULL;
            ++g_reopens;
            g_level = 0;
            continue;
        }
        if (r <= 0) continue;
        for (long i = 0; i < r; ++i) {
            float a = fabsf(buf[i] / 32768.0f);
            if (a > peak) peak = a;
        }
        if ((peak_n += (int)r) >= 9600) { g_level = peak;   /* every 200 ms: quick enough for the Setup meter */ peak = 0; peak_n = 0; }
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
    snprintf(g_in_dev, sizeof g_in_dev, "%s", in_dev);
    int e = open_pcm(&g_in, in_dev, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK, 100000, 5);
    if (e < 0) {
        snprintf(g_err, sizeof g_err, "capture %s: %s", in_dev, A.strerror(e));
        return -1;
    }
    g_cb = cb;
    g_user = user;
    g_frames = g_errors = g_reopens = 0; g_rate = 0; g_rate_t0 = 0; g_rate_f0 = 0; g_rx_err[0] = 0;
    g_run = 1;
    g_thread_on = pthread_create(&g_thr, NULL, rx_thread, NULL) == 0;
    return 0;
}

void audio_close(void)
{
    g_run = 0;
    if (g_thread_on) { pthread_join(g_thr, NULL); g_thread_on = 0; }
    if (g_in) { A.close(g_in); g_in = NULL; }
}

void audio_set_output(const char *out_dev) { snprintf(g_out_dev, sizeof g_out_dev, "%s", out_dev); }

int audio_play(const float *buf, size_t n, float gain)
{
    if (load_alsa()) return -1;
    int e;
    /* same 100 ms buffer as capture: USB codecs that tie the two directions
     * together refuse a second stream with different period sizes */
    int paused = 0;
    e = open_pcm(&g_out, g_out_dev, SND_PCM_STREAM_PLAYBACK, 0, 100000, 2);
    if (e == -EBUSY && g_thread_on) {
        /* half-duplex codec (seen on the Xiegu X6200): let go of the input while transmitting */
        qlog("audio", "output %s busy while the input is open: pausing the input for this transmission", g_out_dev);
        g_rx_pause = 1;
        paused = 1;
        for (int i = 0; i < 100 && !g_rx_paused; ++i) sleep_ms(5);
        e = open_pcm(&g_out, g_out_dev, SND_PCM_STREAM_PLAYBACK, 0, 100000, 4);
    }
    if (e < 0) {
        snprintf(g_err, sizeof g_err, "playback %s: %s", g_out_dev, A.strerror(e));
        g_rx_pause = 0;
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
        if (w < 0) {
            qlog("audio", "output write error: %s", A.strerror((int)w));
            if (A.recover(g_out, (int)w, 1) < 0) { rc = -1; break; }
            continue;
        }
        pos += (size_t)w;
    }
    if (rc == 1) A.drop(g_out); else A.drain(g_out);
    A.close(g_out);
    g_out = NULL;
    if (paused) g_rx_pause = 0;                    /* RX reopens the input */
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
