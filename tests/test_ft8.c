/* FT8 loopback: encode -> GFSK -> AWGN -> decode, several signals at once. */
#include "dsp/ft8w.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double gauss(void)
{
    double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

static int run(float snr, int verbose)
{
    const char *msgs[] = {"CQ K1ABC FN42", "W9XYZ K1ABC -12", "K1ABC W9XYZ R-08", "CQ POTA KN4CRD EM73"};
    float freqs[] = {700, 1200, 1650, 2200};
    int n = 15 * 12000;
    float *mix = calloc(n, sizeof(float)), *sig = calloc(n, sizeof(float));
    double amp = sqrt(2.0 * (2500.0 / 6000.0) * pow(10.0, snr / 10.0));
    for (int s = 0; s < 4; ++s) {
        uint8_t tones[105];
        int nt = ft8w_encode(FT8W_FT8, msgs[s], tones);
        if (nt < 0) { printf("encode failed: %s\n", msgs[s]); return 0; }
        size_t ns = ft8w_synth(FT8W_FT8, tones, nt, freqs[s], 12000, (float)amp, sig);
        for (size_t i = 0; i < ns; ++i) mix[6000 + i] += sig[i];
    }
    int16_t *pcm = malloc(sizeof(int16_t) * n);
    for (int i = 0; i < n; ++i) pcm[i] = (int16_t)fmax(-32768, fmin(32767, (mix[i] + gauss()) * 2000));
    ft8w_t *w = ft8w_new(FT8W_FT8, 12000, 200, 3000);
    ft8w_decode_t out[50];
    clock_t t0 = clock();
    int nd = ft8w_decode(w, pcm, n, out, 50);
    double ms = 1000.0 * (clock() - t0) / CLOCKS_PER_SEC;
    int ok = 0;
    for (int k = 0; k < nd; ++k) {
        int m = 0;
        for (int s = 0; s < 4; ++s) if (!strcmp(out[k].text, msgs[s])) m = 1;
        ok += m;
        if (verbose) printf("   %+3d dB %+.2f %6.1f Hz  %s%s\n", out[k].snr, out[k].dt, out[k].freq, out[k].text, m ? "" : "  (SPURIOUS)");
    }
    if (verbose) printf("  @ %+.0f dB: %d/4, %.0f ms\n", snr, ok, ms);
    ft8w_free(w);
    free(mix); free(sig); free(pcm);
    return ok;
}

int main(void)
{
    srand(99);
    printf("== FT8 loopback, 4 signals @ -12 dB ==\n");
    int ok = run(-12, 1);
    printf("== sweep (4 signals x 5 trials) ==\n");
    for (int snr = -14; snr >= -22; snr -= 2) {
        int t = 0;
        for (int k = 0; k < 5; ++k) t += run((float)snr, 0);
        printf("  %+d dB: %2d/20\n", snr, t);
    }
    printf(ok == 4 ? "PASS\n" : "FAIL\n");
    return ok != 4;
}
