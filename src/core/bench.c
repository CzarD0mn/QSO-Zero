/* QSO-Zero - decode speed check: `qso-zero --bench`.
 *
 * Builds a busy band of synthetic signals for each mode, runs the same
 * decoder calls the app makes (same audio window), times them, and compares
 * the slowest of 3 runs with the time available before the next
 * transmission could start. Run it on the target (the Cardputer Zero's
 * CM0) to see whether replies will go out on time. */
#include "core/bench.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "dsp/ft8w.h"
#include "dsp/js8.h"
#include "dsp/wspr.h"
#include "dsp/wsprrx.h"

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static unsigned g_seed = 12345;
static double urand(void) { g_seed = g_seed * 1103515245u + 12345u; return ((g_seed >> 8) & 0xFFFFFF) / 16777216.0; }
static double gauss(void)
{
    double u = urand() + 1e-12, v = urand();
    return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

/* amplitude for a signal at snr dB (2500 Hz) over unit-sigma noise at 12 kHz */
static float amp_for(double snr) { return (float)sqrt(2.0 * (2500.0 / 6000.0) * pow(10.0, snr / 10.0)); }

static int16_t *to_pcm(const float *mix, int n)
{
    int16_t *p = malloc(sizeof(int16_t) * n);
    for (int i = 0; i < n; ++i) {
        double v = (mix[i] + gauss()) * 800.0;
        p[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    return p;
}

static const char *k_calls[] = {"W1AW", "K1ABC", "N0XYZ", "VE3ABC", "G4JNT", "DL1ABC", "JA1XYZ", "VK2ABC", "F5ABC", "EA4XYZ",
                                "I2ABC", "ON4ABC", "PA3XYZ", "SM5ABC", "OH2ABC", "LA9ABC", "OK1ABC", "SP5XYZ", "HA5ABC", "YO3ABC"};
static const char *k_grids[] = {"FN31", "FN42", "EN34", "FN03", "IO90", "JO62", "PM95", "QF56", "JN18", "IN80",
                                "JN45", "JO20", "JO22", "JO89", "KP20", "JP50", "JO70", "KO02", "JN97", "KN34"};

typedef struct { const char *mode; double worst_ms, med_ms, budget_ms; int found, sent; } result_t;

static void verdict(const result_t *r)
{
    const char *v = r->worst_ms <= r->budget_ms * 0.66 ? "OK" : r->worst_ms <= r->budget_ms ? "TIGHT" : "TOO SLOW";
    printf("  %-11s %6.0f ms (worst of 3, median %5.0f)  budget %5.0f ms  decoded %2d/%-2d  %s\n",
           r->mode, r->worst_ms, r->med_ms, r->budget_ms, r->found, r->sent, v);
}

static void sort3(double *t)
{
    for (int a = 0; a < 3; ++a) for (int b = a + 1; b < 3; ++b) if (t[b] < t[a]) { double x = t[a]; t[a] = t[b]; t[b] = x; }
}

/* FT8 / FT4: same window and decoder settings as the app */
static result_t bench_ft(ft8w_proto_t pr, int nsig)
{
    int ft4 = pr == FT8W_FT4;
    double ready = ft4 ? 5.9 : 14.0, P = ft4 ? 7.5 : 15.0;
    int n = (int)((ready - 0.1) * 12000);
    float *mix = calloc(n, sizeof(float)), *w = malloc(sizeof(float) * 16 * 12000);
    char msgs[32][40];
    for (int s = 0; s < nsig; ++s) {
        int k = s % 20;
        if (s % 3 == 0) snprintf(msgs[s], 40, "CQ %s %s", k_calls[k], k_grids[k]);
        else if (s % 3 == 1) snprintf(msgs[s], 40, "KM4FLG %s %s", k_calls[k], k_grids[k]);
        else snprintf(msgs[s], 40, "%s KM4FLG -%02d", k_calls[k], 5 + s % 15);
        uint8_t tones[105];
        int nt = ft8w_encode(pr, msgs[s], tones);
        double snr = -18 + (s * 23 % 24);                       /* -18 .. +5 dB */
        float f0 = 300 + s * (ft4 ? 150.0f : 115.0f);
        size_t m = ft8w_synth(pr, tones, nt, f0, 12000, amp_for(snr), w);
        int off = (int)((0.5 + 0.02 * (s % 10)) * 12000);       /* DT 0 .. +0.18 s */
        for (size_t i = 0; i < m && off + (int)i < n; ++i) mix[off + i] += w[i];
    }
    int16_t *pcm = to_pcm(mix, n);
    ft8w_t *dec = ft8w_new(pr, 12000, 100, 3000);
    ft8w_decode_t d[60];
    double t[3];
    int found = 0;
    for (int r = 0; r < 3; ++r) {
        double t0 = now_s();
        int nd = ft8w_decode(dec, pcm, n, d, 60);
        t[r] = (now_s() - t0) * 1000;
        if (r == 0)
            for (int k = 0; k < nd; ++k)
                for (int s = 0; s < nsig; ++s) if (!strcmp(d[k].text, msgs[s])) { ++found; break; }
    }
    sort3(t);
    ft8w_free(dec);
    free(mix); free(w); free(pcm);
    /* a reply goes out at the next period + 0.5 s; decoding starts at `ready` */
    result_t res = {ft4 ? "FT4" : "FT8", t[2], t[1], (P + 0.5 - ready) * 1000, found, nsig};
    return res;
}

static result_t bench_js8(js8_submode_t m, int nsig)
{
    const js8_submode_info_t *in = js8_submode_info(m);
    double ready = in->start_delay_ms / 1000.0 + 79.0 * in->symbol_samples / 12000.0 + 0.35;
    int n = (int)((ready - 0.1) * 12000);
    float *mix = calloc(n, sizeof(float)), *w = malloc(sizeof(float) * JS8_NUM_SYMBOLS * in->symbol_samples);
    static const char *frames[] = {"HELLOWORLD12", "N0CALLK1ABC+", "CQCQCQDE4WAY", "abcXYZ-+0987",
                                   "KM4FLGTEST01", "QRPFIELDOPS1", "GHOSTNET7107", "JS8CALLTEST2"};
    for (int s = 0; s < nsig; ++s) {
        uint8_t tones[JS8_NUM_SYMBOLS];
        js8_encode(m, 0, frames[s % 8], tones);
        float f0 = 400 + s * in->bandwidth * 1.6f;
        size_t k = js8_synth(m, tones, f0, 12000, amp_for(-14 + (s * 7) % 16), w);
        size_t off = in->start_delay_ms * 12 + s * 40;
        for (size_t i = 0; i < k && off + i < (size_t)n; ++i) mix[off + i] += w[i];
    }
    int16_t *pcm = to_pcm(mix, n);
    js8_decoder_t *dec = js8_decoder_new(m);
    js8_decode_t d[40];
    double t[3];
    int found = 0;
    for (int r = 0; r < 3; ++r) {
        double t0 = now_s();
        int nd = js8_decode(dec, pcm, n, 200, 2900, 1500, d, 40);
        t[r] = (now_s() - t0) * 1000;
        if (r == 0)
            for (int k = 0; k < nd; ++k)
                for (int s = 0; s < nsig; ++s) if (!strcmp(d[k].text, frames[s % 8])) { ++found; break; }
    }
    sort3(t);
    js8_decoder_free(dec);
    free(mix); free(w); free(pcm);
    static char name[4][16];
    snprintf(name[m], sizeof name[m], "JS8 %s", in->name);
    result_t res = {name[m], t[2], t[1], (in->period_s + in->start_delay_ms / 1000.0 - ready) * 1000, found, nsig};
    return res;
}

static result_t bench_wspr(int nsig)
{
    int n = WSPRRX_SAMPLES;
    float *mix = calloc(n, sizeof(float)), *w = malloc(sizeof(float) * 162 * 8192);
    for (int s = 0; s < nsig; ++s) {
        uint8_t sym[162];
        wspr_encode(k_calls[s], k_grids[s], 37, sym);
        size_t m = wspr_synth(sym, 1420 + s * 22.0f, 12000, amp_for(-24 + s % 8), w);
        for (size_t i = 0; i < m && 12000 + i < (size_t)n; ++i) mix[12000 + i] += w[i];
    }
    int16_t *pcm = to_pcm(mix, n);
    wsprrx_spot_t sp[40];
    double t[3];
    int found = 0;
    for (int r = 0; r < 3; ++r) {
        double t0 = now_s();
        int nd = wsprrx_decode(pcm, n, sp, 40);
        t[r] = (now_s() - t0) * 1000;
        if (r == 0) found = nd;
    }
    sort3(t);
    free(mix); free(w); free(pcm);
    /* decoded at +115 s; the next cycle's beacon would start at +121 s */
    result_t res = {"WSPR", t[2], t[1], 6000, found > nsig ? nsig : found, nsig};
    return res;
}

static void cpu_info(void)
{
    char model[128] = "?", line[256];
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "Model", 5) || !strncmp(line, "model name", 10)) {
                char *c = strchr(line, ':');
                if (c) { snprintf(model, sizeof model, "%s", c + 2); model[strcspn(model, "\n")] = 0; }
            }
        fclose(f);
    }
    long khz = 0;
    if ((f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", "r"))) { if (fscanf(f, "%ld", &khz) != 1) khz = 0; fclose(f); }
    double temp = 0;
    if ((f = fopen("/sys/class/thermal/thermal_zone0/temp", "r"))) { long t; if (fscanf(f, "%ld", &t) == 1) temp = t / 1000.0; fclose(f); }
    printf("  CPU: %s, %ld cores", model, sysconf(_SC_NPROCESSORS_ONLN));
    if (khz) printf(", max %ld MHz", khz / 1000);
    if (temp > 0) printf(", %.0f C", temp);
    printf("\n");
}

int bench_run(void)
{
    printf("QSO-Zero decode speed check (single core, like the app's decode thread)\n");
    cpu_info();
    printf("  budget = time from decode start to the earliest reply transmission\n\n");
    result_t r[8];
    int nr = 0;
    printf("  running FT8 (20 signals)...\n"); fflush(stdout);
    r[nr++] = bench_ft(FT8W_FT8, 20);
    printf("  running FT4 (15 signals)...\n"); fflush(stdout);
    r[nr++] = bench_ft(FT8W_FT4, 15);
    for (int m = JS8_NORMAL; m <= JS8_SLOW; ++m) {
        printf("  running JS8 %s (8 signals)...\n", js8_submode_info((js8_submode_t)m)->name); fflush(stdout);
        r[nr++] = bench_js8((js8_submode_t)m, 8);
    }
    printf("  running WSPR (8 signals, 2-minute cycle)...\n\n"); fflush(stdout);
    r[nr++] = bench_wspr(8);
    int bad = 0;
    for (int i = 0; i < nr; ++i) {
        verdict(&r[i]);
        if (r[i].worst_ms > r[i].budget_ms) ++bad;
    }
    printf("\n  OK = under 2/3 of the budget, TIGHT = fits but with little margin, TOO SLOW = replies would be late\n");
    return bad ? 1 : 0;
}
