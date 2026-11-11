/* WSPR receive loopback: our encoder -> 12 kHz audio + AWGN -> wsprd port. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsp/wspr.h"
#include "dsp/wsprrx.h"

static double gauss(void)
{
    double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

typedef struct { const char *call, *grid; int dbm; float f, snr, start; } sig_t;

static int run(const sig_t *sg, int ns, int verbose, double *ms)
{
    int n = 120 * 12000;
    float *mix = calloc(n, sizeof(float)), *w = malloc(sizeof(float) * 162 * 8192);
    for (int k = 0; k < ns; ++k) {
        uint8_t sym[162];
        if (wspr_encode(sg[k].call, sg[k].grid, sg[k].dbm, sym)) { printf("encode fail\n"); exit(1); }
        double amp = sqrt(2.0 * (2500.0 / 6000.0) * pow(10.0, sg[k].snr / 10.0));
        size_t m = wspr_synth(sym, sg[k].f, 12000, (float)amp, w);
        size_t off = (size_t)(sg[k].start * 12000);
        for (size_t i = 0; i < m && off + i < (size_t)n; ++i) mix[off + i] += w[i];
    }
    int16_t *pcm = malloc(sizeof(int16_t) * n);
    for (int i = 0; i < n; ++i) pcm[i] = (int16_t)fmax(-32768, fmin(32767, (mix[i] + gauss()) * 3000));
    wsprrx_spot_t sp[32];
    clock_t t0 = clock();
    int nd = wsprrx_decode(pcm, n, sp, 32);
    if (ms) *ms = 1000.0 * (clock() - t0) / CLOCKS_PER_SEC;
    int ok = 0;
    for (int i = 0; i < nd; ++i) {
        int m = 0;
        for (int k = 0; k < ns; ++k)
            if (!strcmp(sp[i].call, sg[k].call) && !strncmp(sp[i].grid, sg[k].grid, 4) && sp[i].dbm == wspr_valid_dbm(sg[k].dbm) &&
                fabs(sp[i].freq - sg[k].f) < 2) m = 1;
        ok += m;
        if (verbose) printf("   %+3d dB dt %+.1f %8.1f Hz drift %+d  %-16s%s\n", sp[i].snr, sp[i].dt, sp[i].freq, sp[i].drift,
                            sp[i].text, m ? "" : "  (WRONG)");
    }
    free(mix); free(w); free(pcm);
    return ok;
}

int main(void)
{
    srand(2026);
    int fail = 0;
    double ms;
    sig_t band[] = {
        {"K1ABC", "FN42", 37, 1430.0f, -18, 1.0f}, {"G4JNT", "IO90", 23, 1465.5f, -22, 1.0f},
        {"VE3XYZ", "FN03", 30, 1500.0f, -15, 1.3f}, {"JA1ABC", "PM95", 10, 1531.2f, -24, 0.8f},
        {"KN4CRD", "EM73", 37, 1575.0f, -20, 1.0f},
    };
    printf("== 5 stations across the WSPR window ==\n");
    int ok = run(band, 5, 1, &ms);
    printf("  %d/5 decoded correctly, %.0f ms\n", ok, ms);
    if (ok < 5) fail = 1;
    printf("== sensitivity (1 station, 5 trials) ==\n");
    for (int snr = -22; snr >= -30; snr -= 2) {
        int t = 0;
        for (int k = 0; k < 5; ++k) {
            sig_t s = {"W1AW", "FN31", 37, 1480.0f + k * 7, (float)snr, 1.0f};
            t += run(&s, 1, 0, NULL);
        }
        printf("  %+d dB: %d/5\n", snr, t);
        if (snr == -24 && t < 5) fail = 1;
    }
    printf(fail ? "FAIL\n" : "PASS\n");
    return fail;
}
