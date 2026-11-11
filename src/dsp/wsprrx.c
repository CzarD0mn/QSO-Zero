/* NeonQRP - WSPR receive front end. Mirrors WSJT-X wsprd's readwavfile():
 * one 2^21-point real FFT of the 114 s recording, keep +/-187.5 Hz around
 * 1500 Hz, inverse 65536-point complex FFT -> 375 sample/s I/Q. */
#include "wsprrx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fft/kiss_fft.h"
#include "fft/kiss_fftr.h"
#include "wsprd/wsprd.h"

#define NFFT1 (2 * 1024 * 1024)
#define NFFT2 (NFFT1 / 32)
#define NOUT  (WSPRRX_SAMPLES / 32)    /* 42750 baseband samples */

int wsprrx_decode(const int16_t *audio, int n, wsprrx_spot_t *out, int max)
{
    static kiss_fftr_cfg c1;
    static kiss_fft_cfg c2;
    if (!c1) c1 = kiss_fftr_alloc(NFFT1, 0, NULL, NULL);
    if (!c2) c2 = kiss_fft_alloc(NFFT2, 1, NULL, NULL);
    float *in = calloc(NFFT1, sizeof(float));
    kiss_fft_cpx *spec = malloc(sizeof(kiss_fft_cpx) * (NFFT1 / 2 + 1));
    kiss_fft_cpx *bb = malloc(sizeof(kiss_fft_cpx) * NFFT2), *bt = malloc(sizeof(kiss_fft_cpx) * NFFT2);
    float *idat = calloc(NOUT, sizeof(float)), *qdat = calloc(NOUT, sizeof(float));
    int nres = 0;
    if (!in || !spec || !bb || !bt || !idat || !qdat || !c1 || !c2) goto done;

    int np = n < WSPRRX_SAMPLES ? n : WSPRRX_SAMPLES;
    for (int i = 0; i < np; ++i) in[i] = audio[i];
    kiss_fftr(c1, in, spec);

    const double df = 12000.0 / NFFT1;
    const int i0 = (int)(1500.0 / df + 0.5), nh2 = NFFT2 / 2;
    for (int i = 0; i < NFFT2; ++i) {
        int j = i0 + i;
        if (i > nh2) j -= NFFT2;
        bb[i] = spec[j];
    }
    kiss_fft(c2, bb, bt);
    for (int i = 0; i < NOUT; ++i) {
        idat[i] = bt[i].r / 1000.0f;
        qdat[i] = bt[i].i / 1000.0f;
    }

    struct decoder_options opt = {.freq = 0, .quickmode = 0, .usehashtable = 1, .npasses = 2, .subtraction = 1};
    struct decoder_results res[MAX_UNIQUES];
    memset(res, 0, sizeof res);
    wspr_decode(idat, qdat, NOUT, opt, res, &nres);
    if (nres > max) nres = max;
    for (int k = 0; k < nres; ++k) {
        wsprrx_spot_t *s = &out[k];
        memset(s, 0, sizeof *s);
        s->snr = (int)lroundf(res[k].snr);
        /* wsprd's DT is relative to 2 s into the file; WSPR starts at +1 s */
        s->dt = res[k].dt + 1.0f;
        s->freq = (float)(res[k].freq * 1e6);    /* dial 0 => audio offset 1500+f */
        s->drift = (int)lroundf(res[k].drift);
        snprintf(s->call, sizeof s->call, "%s", res[k].call);
        snprintf(s->grid, sizeof s->grid, "%s", res[k].loc);
        s->dbm = atoi(res[k].pwr);
        snprintf(s->text, sizeof s->text, "%s", res[k].message);
    }
done:
    free(in); free(spec); free(bb); free(bt); free(idat); free(qdat);
    return nres;
}
