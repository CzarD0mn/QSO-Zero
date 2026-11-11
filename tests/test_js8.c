/* JS8 loopback test: encode -> synthesize -> add AWGN -> decode. */
#include "dsp/js8.h"
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

/* SNR is defined in 2500 Hz bandwidth, as WSJT-X/JS8Call report it. */
static int run(js8_submode_t m, float snr_db, int nsig, int verbose)
{
    const js8_submode_info_t *in = js8_submode_info(m);
    js8_decoder_t *d = js8_decoder_new(m);
    int n = js8_decoder_samples(d);
    float *mix = calloc(n, sizeof(float));
    float *sig = calloc(JS8_NUM_SYMBOLS * in->symbol_samples, sizeof(float));
    const char *frames[] = {"N0CALLK1ABC+", "HELLOWORLD12", "abcXYZ-+0987", "CQCQCQDE4WAY"};
    float freqs[] = {1000.0f, 1500.0f, 620.0f, 2100.0f};
    /* noise sigma 1 per sample at 12 kHz => noise power in 2500 Hz = 2500/6000 */
    double pnoise2500 = 2500.0 / 6000.0;
    double amp = sqrt(2.0 * pnoise2500 * pow(10.0, snr_db / 10.0));
    for (int s = 0; s < nsig; ++s) {
        uint8_t tones[79];
        if (js8_encode(m, s & 7, frames[s], tones)) { printf("encode fail\n"); return 0; }
        size_t ns = js8_synth(m, tones, freqs[s], 12000, (float)amp, sig);
        size_t off = in->start_delay_ms * 12 + s * 37;  /* small DT offsets */
        for (size_t i = 0; i < ns && off + i < (size_t)n; ++i) mix[off + i] += sig[i];
    }
    int16_t *pcm = malloc(sizeof(int16_t) * n);
    double scale = 300.0;
    for (int i = 0; i < n; ++i) {
        double v = (mix[i] + gauss()) * scale;
        pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    js8_decode_t out[32];
    clock_t t0 = clock();
    int nd = js8_decode(d, pcm, n, 200, 2600, 1500, out, 32);
    double ms = 1000.0 * (clock() - t0) / CLOCKS_PER_SEC;
    int ok = 0;
    for (int k = 0; k < nd; ++k) {
        int match = 0;
        for (int s = 0; s < nsig; ++s)
            if (!strcmp(out[k].text, frames[s]) && out[k].frame_type == (s & 7)) match = 1;
        ok += match;
        if (verbose)
            printf("   %-6s %+3d dB  dt %+.2f  %7.1f Hz  i3=%d  %s %s\n", in->name, out[k].snr,
                   out[k].dt, out[k].freq, out[k].frame_type, out[k].text, match ? "" : "(SPURIOUS)");
    }
    if (verbose) printf("  %s @ %+.0f dB: %d/%d decoded, %d spurious, %.0f ms\n", in->name, snr_db, ok, nsig, nd - ok, ms);
    js8_decoder_free(d);
    free(mix); free(sig); free(pcm);
    return ok;
}

int main(int argc, char **argv)
{
    srand(12345);
    int fail = 0;
    printf("== Multi-signal loopback, -10 dB ==\n");
    for (int m = 0; m < 4; ++m) if (run((js8_submode_t)m, -10.0f, 4, 1) != 4) fail = 1;
    printf("== Sensitivity sweep (1 signal, 10 trials each) ==\n");
    for (int m = 0; m < 4; ++m) {
        printf("  %-6s", js8_submode_info(m)->name);
        for (int snr = -14; snr >= -28; snr -= 2) {
            int ok = 0;
            for (int t = 0; t < 10; ++t) ok += run((js8_submode_t)m, (float)snr, 1, 0);
            printf(" %d:%2d/10", snr, ok);
        }
        printf("\n");
    }
    (void)argc; (void)argv;
    printf(fail ? "FAIL\n" : "PASS\n");
    return fail;
}
