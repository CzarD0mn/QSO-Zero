/*
 * NeonQRP - JS8 modem, C99 port of JS8Call's JS8.cpp (GPLv3).
 *
 * Faithful to the reference decoder's pipeline:
 *   syncjs8   - Nuttall-windowed 1/4-symbol spectra, polynomial noise baseline,
 *               Costas sync metric per bin, 40th-percentile normalisation,
 *               candidate extraction with near-duplicate suppression
 *   js8dec    - baseband downsample, coarse DT / fine DF search, symbol FFTs,
 *               hard sync check, two LLR metrics, 4-pass BP decode, CRC-12,
 *               re-encode, signal subtraction, SNR estimate
 *   3 passes with subtraction between them.
 *
 * Differences from the reference, all deliberate:
 *   - kissfft instead of FFTW, no Eigen/Boost (6x6 solve done directly).
 *   - Baseline polynomial is fitted on a [0,1] scaled axis (same curve,
 *     much better numerical conditioning in double).
 *   - The subtraction low-pass filter is placed circularly (negative lags at
 *     the end of the buffer) as the original Fortran cshift() did; the C++
 *     std::rotate placed them at +701..+1400, which smears the estimate.
 */
#include "js8.h"
#include <complex.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "fft/kiss_fft.h"
#include "fft/kiss_fftr.h"

#include "js8_tables.h"

#define N_BITS   174
#define K_BITS   87
#define M_BITS   87
#define ND       58
#define NN       79
#define ASYNCMIN 1.5f
#define NFSRCH   5
#define NMAXCAND 300
#define NFILT    1400
#define NROWS    8
#define NFOS     2
#define NSSY     4
#define NP       3200
#define NP2_G    2812
#define TAU      6.28318530717958647692f

#define BASELINE_DEGREE 5
#define BASELINE_NODES  (BASELINE_DEGREE + 1)
#define BASELINE_SAMPLE 10
#define BASELINE_MIN    500
#define BASELINE_MAX    2500

typedef float complex cfloat;

/* ------------------------------------------------------------------ */
/* Submode tables                                                       */
/* ------------------------------------------------------------------ */

static const js8_submode_info_t k_info[JS8_NUM_SUBMODES] = {
    {"NORMAL", 1920, 500, 15, 0, -24, 12000.0f / 1920, 8 * 12000.0f / 1920},
    {"FAST",   1200, 200, 10, 1, -22, 12000.0f / 1200, 8 * 12000.0f / 1200},
    {"TURBO",   600, 100,  6, 1, -20, 12000.0f /  600, 8 * 12000.0f /  600},
    {"SLOW",   3840, 500, 30, 1, -28, 12000.0f / 3840, 8 * 12000.0f / 3840},
    {"ULTRA",   384, 100,  4, 1, -18, 12000.0f /  384, 8 * 12000.0f /  384},
};

/* Decoder parameters per submode (NDOWNSPS, NDD, JZ, ASTART, BASESUB, AZ factor) */
typedef struct {
    int nsps, ntxdur, ndownsps, ndd, jz;
    float astart, basesub, azf;
} mode_params_t;

static const mode_params_t k_par[JS8_NUM_SUBMODES] = {
    {1920, 15, 32, 100,  62, 0.5f, 40.0f, 0.64f},
    {1200, 10, 20, 100, 144, 0.2f, 39.0f, 0.80f},
    { 600,  6, 12, 120, 172, 0.1f, 38.0f, 0.60f},
    {3840, 30, 32,  94,  32, 0.5f, 42.0f, 0.64f},
    { 384,  4, 12, 125, 250, 0.1f, 36.0f, 0.64f},
};

static const int k_costas[2][3][7] = {
    {{4, 2, 5, 6, 1, 3, 0}, {4, 2, 5, 6, 1, 3, 0}, {4, 2, 5, 6, 1, 3, 0}},
    {{0, 6, 2, 3, 5, 4, 1}, {1, 5, 0, 2, 3, 6, 4}, {2, 5, 0, 6, 4, 1, 3}},
};

const js8_submode_info_t *js8_submode_info(js8_submode_t m)
{
    return (m >= 0 && m < JS8_NUM_SUBMODES) ? &k_info[m] : NULL;
}

/* ------------------------------------------------------------------ */
/* Alphabet, CRC, parity                                                */
/* ------------------------------------------------------------------ */

static const char k_alphabet[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+";

static int alpha_word(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    if (c >= 'a' && c <= 'z') return c - 'a' + 36;
    if (c == '-') return 62;
    if (c == '+') return 63;
    return -1;
}

/* boost::augmented_crc<12, 0xc06> over 11 bytes, then ^ 42 (JS8 vs FT8). */
static uint16_t crc12(const uint8_t *bytes, int n)
{
    uint16_t rem = 0;
    for (int i = 0; i < n; ++i) {
        for (int b = 7; b >= 0; --b) {
            int top = (rem >> 11) & 1;
            rem = (uint16_t)(((rem << 1) | ((bytes[i] >> b) & 1)) & 0xFFF);
            if (top) rem ^= 0xC06;
        }
    }
    return rem ^ 42;
}

static uint8_t g_parity[87][11];   /* row-wise packed generator, MSB first */
static int     g_parity_ready;

static void parity_init(void)
{
    if (g_parity_ready) return;
    for (int r = 0; r < 87; ++r) {
        const char *h = js8_parity_hex[r];
        for (int k = 0; k < 11; ++k) {
            int hi = h[2 * k], lo = h[2 * k + 1];
            hi = hi <= '9' ? hi - '0' : hi - 'a' + 10;
            lo = lo <= '9' ? lo - '0' : lo - 'a' + 10;
            g_parity[r][k] = (uint8_t)((hi << 4) | lo);
        }
        g_parity[r][10] &= 0xFE; /* only 87 columns */
    }
    g_parity_ready = 1;
}

static int popcount8(uint8_t v)
{
    return __builtin_popcount(v);
}

int js8_encode(js8_submode_t mode, int type, const char frame[12],
               uint8_t tones[NN])
{
    parity_init();
    uint8_t bytes[11] = {0};
    for (int i = 0, j = 0; i < 12; i += 4, j += 3) {
        int w[4];
        for (int k = 0; k < 4; ++k)
            if ((w[k] = alpha_word(frame[i + k])) < 0) return -1;
        uint32_t words = ((uint32_t)w[0] << 18) | (w[1] << 12) | (w[2] << 6) | w[3];
        bytes[j] = (uint8_t)(words >> 16);
        bytes[j + 1] = (uint8_t)(words >> 8);
        bytes[j + 2] = (uint8_t)words;
    }
    bytes[9] = (uint8_t)((type & 7) << 5);
    uint16_t crc = crc12(bytes, 11);
    bytes[9] |= (crc >> 7) & 0x1F;
    bytes[10] = (uint8_t)((crc & 0x7F) << 1);

    const int (*cos)[7] = k_costas[k_info[mode].costas_modified];
    for (int a = 0; a < 3; ++a)
        for (int k = 0; k < 7; ++k) tones[a * 36 + k] = (uint8_t)cos[a][k];

    /* parity bit i = popcount(row_i & message) mod 2 */
    for (int s = 0; s < 29; ++s) {
        int pw = 0, ow = 0;
        for (int b = 0; b < 3; ++b) {
            int i = s * 3 + b, acc = 0;
            for (int k = 0; k < 11; ++k) acc += popcount8(g_parity[i][k] & bytes[k]);
            pw = (pw << 1) | (acc & 1);
            ow = (ow << 1) | ((bytes[i >> 3] >> (7 - (i & 7))) & 1);
        }
        tones[7 + s] = (uint8_t)pw;
        tones[43 + s] = (uint8_t)ow;
    }
    return 0;
}

size_t js8_synth(js8_submode_t mode, const uint8_t tones[NN], float f0,
                 int sample_rate, float amplitude, float *out)
{
    const js8_submode_info_t *in = &k_info[mode];
    double sym_s = (double)in->symbol_samples / JS8_SAMPLE_RATE;
    size_t nsps = (size_t)lround(sym_s * sample_rate);
    size_t n = nsps * NN;
    double phi = 0.0;
    size_t ramp = nsps / 8;
    for (size_t k = 0; k < NN; ++k) {
        double dphi = 2.0 * M_PI * (f0 + tones[k] * in->tone_spacing) / sample_rate;
        for (size_t i = 0; i < nsps; ++i) {
            size_t idx = k * nsps + i;
            float a = amplitude;
            if (idx < ramp) a *= 0.5f * (1.0f - cosf((float)M_PI * idx / ramp));
            else if (idx >= n - ramp) a *= 0.5f * (1.0f - cosf((float)M_PI * (n - 1 - idx) / ramp));
            out[idx] = a * (float)sin(phi);
            phi += dphi;
            if (phi > 2.0 * M_PI) phi -= 2.0 * M_PI;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Belief propagation                                                   */
/* ------------------------------------------------------------------ */

static int bpdecode174(const float llr[N_BITS], int8_t decoded[K_BITS],
                       int8_t cw[N_BITS])
{
    float tov[N_BITS][3];
    float toc[M_BITS][7];
    float tanhtoc[M_BITS][7];
    float zn[N_BITS];
    int ncnt = 0, nclast = 0;

    memset(tov, 0, sizeof tov);
    memset(toc, 0, sizeof toc);
    for (int i = 0; i < M_BITS; ++i)
        for (int j = 0; j < js8_Nm_count[i]; ++j) toc[i][j] = llr[js8_Nm[i][j]];

    for (int iter = 0; iter <= 30; ++iter) {
        for (int i = 0; i < N_BITS; ++i) {
            zn[i] = llr[i] + tov[i][0] + tov[i][1] + tov[i][2];
            cw[i] = zn[i] > 0 ? 1 : 0;
        }
        int ncheck = 0;
        for (int i = 0; i < M_BITS; ++i) {
            int s = 0;
            for (int j = 0; j < js8_Nm_count[i]; ++j) s += cw[js8_Nm[i][j]];
            ncheck += s & 1;
        }
        if (ncheck == 0) {
            memcpy(decoded, cw + M_BITS, K_BITS);
            int nerr = 0;
            for (int i = 0; i < N_BITS; ++i)
                if ((2 * cw[i] - 1) * llr[i] < 0.0f) ++nerr;
            return nerr;
        }
        if (iter > 0) {
            int nd = ncheck - nclast;
            ncnt = nd < 0 ? 0 : ncnt + 1;
            if (ncnt >= 5 && iter >= 10 && ncheck > 15) return -1;
        }
        nclast = ncheck;

        for (int i = 0; i < M_BITS; ++i) {
            for (int j = 0; j < js8_Nm_count[i]; ++j) {
                int ibj = js8_Nm[i][j];
                float v = zn[ibj];
                for (int k = 0; k < 3; ++k)
                    if (js8_Mn[ibj][k] == i) v -= tov[ibj][k];
                toc[i][j] = v;
            }
            for (int j = 0; j < js8_Nm_count[i]; ++j) tanhtoc[i][j] = tanhf(-toc[i][j] / 2.0f);
        }
        for (int i = 0; i < N_BITS; ++i) {
            for (int j = 0; j < 3; ++j) {
                int ichk = js8_Mn[i][j];
                float t = 1.0f;
                for (int k = 0; k < js8_Nm_count[ichk]; ++k)
                    if (js8_Nm[ichk][k] != i) t *= tanhtoc[ichk][k];
                /* clamp to keep atanh finite, as gfortran's platt() effectively did */
                if (t > 0.999999f) t = 0.999999f;
                if (t < -0.999999f) t = -0.999999f;
                tov[i][j] = 2.0f * atanhf(-t);
            }
        }
    }
    return -1;
}

static int check_crc(const int8_t d[K_BITS])
{
    uint8_t bits[11] = {0};
    for (int i = 0; i < K_BITS; ++i)
        if (d[i]) bits[i / 8] |= (uint8_t)(1 << (7 - (i % 8)));
    uint16_t crc = (uint16_t)(((bits[9] & 0x1F) << 7) | (bits[10] >> 1));
    bits[9] &= 0xE0;
    bits[10] = 0;
    return crc == crc12(bits, 11);
}

/* ------------------------------------------------------------------ */
/* Decoder state                                                        */
/* ------------------------------------------------------------------ */

typedef struct { float freq, step, sync; int alive; } sync_t;

struct js8_decoder {
    js8_submode_t mode;
    const int (*costas)[7];
    /* params */
    int nsps, ndownsps, ndd, jz, nmax, nfft1, nstep, nhsym, ndown, nqsymbol;
    int ndfft1, ndfft2, np2, jstrt;
    float astart, basesub, az, tstep, df;

    float  *nuttal;     /* nfft1 */
    cfloat  csyncs[3][7][32];
    cfloat *csymb;      /* ndownsps */
    cfloat *filter;     /* nmax */
    cfloat *cfilt;      /* nmax */
    cfloat *ds_cx;      /* ndfft1/2+1 */
    float  *ds_in;      /* ndfft1 */
    cfloat *sd;         /* nfft1/2+1 */
    float  *sd_in;      /* nfft1 */
    cfloat  cd0[NP];
    cfloat  tmp[NP];
    float  *dd;         /* nmax */
    float  *s;          /* nsps x nhsym, s[i*nhsym + j] */
    float  *savg;       /* nsps */
    sync_t *sync;       /* up to nsps entries */
    int     nsync;
    float  *taper[2];   /* ndd+1 */
    double  coef[BASELINE_NODES];
    double  base_scale;

    kiss_fft_cfg  p_ds, p_cf, p_cb, p_cs;
    kiss_fftr_cfg p_bb, p_sd;
    cfloat *cs_out, *cf_tmp;
};

#define S(d, i, j) ((d)->s[(size_t)(i) * (d)->nhsym + (j)])

js8_decoder_t *js8_decoder_new(js8_submode_t mode)
{
    if (mode < 0 || mode >= JS8_NUM_SUBMODES) return NULL;
    parity_init();
    js8_decoder_t *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    const mode_params_t *p = &k_par[mode];
    d->mode = mode;
    d->costas = k_costas[k_info[mode].costas_modified];
    d->nsps = p->nsps;
    d->ndownsps = p->ndownsps;
    d->ndd = p->ndd;
    d->jz = p->jz;
    d->astart = p->astart;
    d->basesub = p->basesub;
    d->az = (12000.0f / p->nsps) * p->azf;
    d->nmax = p->ntxdur * JS8_SAMPLE_RATE;
    d->nfft1 = p->nsps * NFOS;
    d->nstep = p->nsps / NSSY;
    d->nhsym = d->nmax / d->nstep - 3;
    d->ndown = p->nsps / p->ndownsps;
    d->nqsymbol = p->ndownsps / 4;
    d->ndfft1 = p->nsps * p->ndd;
    d->ndfft2 = d->ndfft1 / d->ndown;
    d->np2 = NN * p->ndownsps;
    d->tstep = d->nstep / 12000.0f;
    d->jstrt = (int)(d->astart / d->tstep);
    d->df = 12000.0f / d->nfft1;

    d->nuttal = malloc(sizeof(float) * d->nfft1);
    d->csymb = calloc(d->ndownsps, sizeof(cfloat));
    d->cs_out = calloc(d->ndownsps, sizeof(cfloat));
    d->filter = calloc(d->nmax, sizeof(cfloat));
    d->cfilt = calloc(d->nmax, sizeof(cfloat));
    d->cf_tmp = calloc(d->nmax, sizeof(cfloat));
    d->ds_cx = calloc(d->ndfft1 / 2 + 1, sizeof(cfloat));
    d->ds_in = calloc(d->ndfft1, sizeof(float));
    d->sd = calloc(d->nfft1 / 2 + 1, sizeof(cfloat));
    d->sd_in = calloc(d->nfft1, sizeof(float));
    d->dd = calloc(d->nmax, sizeof(float));
    d->s = calloc((size_t)d->nsps * d->nhsym, sizeof(float));
    d->savg = calloc(d->nsps, sizeof(float));
    d->sync = calloc(d->nsps, sizeof(sync_t));
    d->taper[0] = malloc(sizeof(float) * (d->ndd + 1));
    d->taper[1] = malloc(sizeof(float) * (d->ndd + 1));

    d->p_ds = kiss_fft_alloc(d->ndfft2, 1, NULL, NULL);
    d->p_cf = kiss_fft_alloc(d->nmax, 0, NULL, NULL);
    d->p_cb = kiss_fft_alloc(d->nmax, 1, NULL, NULL);
    d->p_cs = kiss_fft_alloc(d->ndownsps, 0, NULL, NULL);
    d->p_bb = kiss_fftr_alloc(d->ndfft1, 0, NULL, NULL);
    d->p_sd = kiss_fftr_alloc(d->nfft1, 0, NULL, NULL);

    if (!d->nuttal || !d->csymb || !d->filter || !d->cfilt || !d->cf_tmp ||
        !d->ds_cx || !d->ds_in || !d->sd || !d->sd_in || !d->dd || !d->s ||
        !d->savg || !d->sync || !d->taper[0] || !d->taper[1] || !d->p_ds ||
        !d->p_cf || !d->p_cb || !d->p_cs || !d->p_bb || !d->p_sd) {
        js8_decoder_free(d);
        return NULL;
    }

    /* Nuttall window, normalised as in the reference */
    {
        const float a0 = 0.3635819f, a1 = -0.4891775f, a2 = 0.1365995f, a3 = -0.0106411f;
        const float pi = 4.0f * atanf(1.0f);
        float sum = 0.0f;
        for (int i = 0; i < d->nfft1; ++i) {
            double v = (double)a0 + a1 * cos(2 * pi * i / d->nfft1) +
                       a2 * cos(4 * pi * i / d->nfft1) + a3 * cos(6 * pi * i / d->nfft1);
            d->nuttal[i] = (float)v;
            sum += (float)v;
        }
        for (int i = 0; i < d->nfft1; ++i)
            d->nuttal[i] = d->nuttal[i] / sum * d->nfft1 / 300.0f;
    }
    /* Costas reference waveforms */
    for (int a = 0; a < 3; ++a)
        for (int i = 0; i < 7; ++i) {
            float dphi = TAU * d->costas[a][i] / d->ndownsps, phi = 0.0f;
            for (int j = 0; j < d->ndownsps; ++j) {
                d->csyncs[a][i][j] = cexpf(I * phi);
                phi = fmodf(phi + dphi, TAU);
            }
        }
    /* Tapers */
    for (int i = 0; i <= d->ndd; ++i) {
        float v = 0.5f * (1.0f + cosf(i * (float)M_PI / d->ndd));
        d->taper[1][i] = v;
        d->taper[0][d->ndd - i] = v;
    }
    /* Subtraction LPF, circularly centred at 0 */
    {
        float sum = 0.0f;
        const float pi = 4.0f * atanf(1.0f);
        for (int j = -NFILT / 2; j <= NFILT / 2; ++j) sum += powf(cosf(pi * j / NFILT), 2);
        for (int j = -NFILT / 2; j <= NFILT / 2; ++j) {
            int idx = j >= 0 ? j : d->nmax + j;
            d->filter[idx] = powf(cosf(pi * j / NFILT), 2) / sum;
        }
        kiss_fft(d->p_cf, (kiss_fft_cpx *)d->filter, (kiss_fft_cpx *)d->cf_tmp);
        for (int i = 0; i < d->nmax; ++i) d->filter[i] = d->cf_tmp[i] / d->nmax;
    }
    return d;
}

void js8_decoder_free(js8_decoder_t *d)
{
    if (!d) return;
    free(d->nuttal); free(d->csymb); free(d->cs_out); free(d->filter);
    free(d->cfilt); free(d->cf_tmp); free(d->ds_cx); free(d->ds_in);
    free(d->sd); free(d->sd_in); free(d->dd); free(d->s); free(d->savg);
    free(d->sync); free(d->taper[0]); free(d->taper[1]);
    kiss_fft_free(d->p_ds); kiss_fft_free(d->p_cf); kiss_fft_free(d->p_cb);
    kiss_fft_free(d->p_cs); kiss_fftr_free(d->p_bb); kiss_fftr_free(d->p_sd);
    free(d);
}

int js8_decoder_samples(const js8_decoder_t *d) { return d->nmax; }

/* ------------------------------------------------------------------ */
/* Baseline                                                             */
/* ------------------------------------------------------------------ */

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

/* Solve the square system V c = y (n <= 8) by Gaussian elimination with
 * partial pivoting. */
static void solve_small(int n, double A[][BASELINE_NODES], double *y, double *x)
{
    for (int c = 0; c < n; ++c) {
        int piv = c;
        for (int r = c + 1; r < n; ++r)
            if (fabs(A[r][c]) > fabs(A[piv][c])) piv = r;
        if (piv != c) {
            for (int k = 0; k < n; ++k) { double t = A[c][k]; A[c][k] = A[piv][k]; A[piv][k] = t; }
            double t = y[c]; y[c] = y[piv]; y[piv] = t;
        }
        double dv = A[c][c];
        if (fabs(dv) < 1e-300) dv = 1e-300;
        for (int r = c + 1; r < n; ++r) {
            double f = A[r][c] / dv;
            for (int k = c; k < n; ++k) A[r][k] -= f * A[c][k];
            y[r] -= f * y[c];
        }
    }
    for (int r = n - 1; r >= 0; --r) {
        double acc = y[r];
        for (int k = r + 1; k < n; ++k) acc -= A[r][k] * x[k];
        x[r] = acc / A[r][r];
    }
}

static void baseline(js8_decoder_t *d, int ia, int ib)
{
    const int bmin = (int)lroundf(BASELINE_MIN / d->df);
    const int bmax = (int)lroundf(BASELINE_MAX / d->df);
    const int size = bmax - bmin + 1;
    const int arm = size / (2 * BASELINE_NODES);
    float *data = d->savg + bmin;
    float span[2048];
    double A[BASELINE_NODES][BASELINE_NODES], yv[BASELINE_NODES];

    for (int i = 0; i < size; ++i) data[i] = 10.0f * log10f(data[i]);

    for (int i = 0; i < BASELINE_NODES; ++i) {
        double node01 = 0.5 * (1.0 - cos(M_PI / (2.0 * BASELINE_NODES) * (2.0 * i + 1)));
        double node = size * node01;
        int base = (int)lround(node);
        int lo = base - arm, hi = base + arm;
        if (lo < 0) lo = 0;
        if (hi > size) hi = size;
        int cnt = hi - lo;
        if (cnt > (int)(sizeof span / sizeof span[0])) cnt = sizeof span / sizeof span[0];
        memcpy(span, data + lo, sizeof(float) * cnt);
        qsort(span, cnt, sizeof(float), cmp_float);
        int n = cnt * BASELINE_SAMPLE / 100;
        double u = node / (size - 1);       /* scaled axis */
        double pw = 1.0;
        for (int k = 0; k < BASELINE_NODES; ++k) { A[i][k] = pw; pw *= u; }
        yv[i] = cnt ? span[n] : 0.0;
    }
    solve_small(BASELINE_NODES, A, yv, d->coef);

    memset(d->savg, 0, sizeof(float) * d->nsps);
    for (int i = ia; i <= ib && i < d->nsps; ++i) {
        double u = (double)(i - ia) / (double)(ib - ia);   /* maps to x/(size-1) */
        double v = 0.0;
        for (int k = BASELINE_NODES - 1; k >= 0; --k) v = v * u + d->coef[k];
        d->savg[i] = (float)v + 0.65f;
    }
}

/* ------------------------------------------------------------------ */
/* Sync / candidates                                                    */
/* ------------------------------------------------------------------ */

static int cmp_sync_desc(const void *a, const void *b)
{
    const sync_t *x = *(const sync_t *const *)a, *y = *(const sync_t *const *)b;
    return (x->sync < y->sync) - (x->sync > y->sync);
}

static int syncjs8(js8_decoder_t *d, int nfa, int nfb, sync_t *cand, int maxcand)
{
    memset(d->savg, 0, sizeof(float) * d->nsps);
    for (int j = 0; j < d->nhsym; ++j) {
        int ia = j * d->nstep, ib = ia + d->nfft1;
        if (ib > d->nmax) break;
        for (int k = 0; k < d->nfft1; ++k) d->sd_in[k] = d->dd[ia + k] * d->nuttal[k];
        kiss_fftr(d->p_sd, d->sd_in, (kiss_fft_cpx *)d->sd);
        for (int i = 0; i < d->nsps; ++i) {
            float pw = crealf(d->sd[i]) * crealf(d->sd[i]) + cimagf(d->sd[i]) * cimagf(d->sd[i]);
            S(d, i, j) = pw;
            d->savg[i] += pw;
        }
    }

    int nwin = nfb - nfa;
    if (nfa < 100) { nfa = 100; if (nwin < 100) nfb = nfa + nwin; }
    if (nfb > 4910) { nfb = 4910; if (nwin < 100) nfa = nfb - nwin; }
    int ia = (int)lroundf(nfa / d->df); if (ia < 0) ia = 0;
    int ib = (int)lroundf(nfb / d->df);
    if (ib > d->nsps - 1 - NFOS * 7) ib = d->nsps - 1 - NFOS * 7;

    baseline(d, ia, ib);

    d->nsync = 0;
    for (int i = ia; i <= ib; ++i) {
        float maxv = -INFINITY;
        int maxj = -d->jz;
        for (int j = -d->jz; j <= d->jz; ++j) {
            float t[2][3] = {{0}};
            for (int p = 0; p < 3; ++p)
                for (int n = 0; n < 7; ++n) {
                    int off = j + d->jstrt + NSSY * n + p * 36 * NSSY;
                    if (off >= 0 && off < d->nhsym) {
                        t[0][p] += S(d, i + NFOS * d->costas[p][n], off);
                        for (int f = 0; f < 7; ++f) t[1][p] += S(d, i + NFOS * f, off);
                    }
                }
#define CSYNC(a, b) ({ float tx = 0, t0 = 0; for (int q = a; q <= b; ++q) { tx += t[0][q]; t0 += t[1][q]; } tx / ((t0 - tx) / 6.0f); })
            float s0 = CSYNC(0, 2), s1 = CSYNC(0, 1), s2 = CSYNC(1, 2);
#undef CSYNC
            float sv = s0;
            if (s1 > sv) sv = s1;
            if (s2 > sv) sv = s2;
            if (sv > maxv) { maxv = sv; maxj = j; }
        }
        sync_t *e = &d->sync[d->nsync++];
        e->freq = d->df * i;
        e->step = d->tstep * (maxj + 0.5f);
        e->sync = maxv;
        e->alive = 1;
    }
    if (!d->nsync) return 0;

    /* 40th percentile normalisation */
    float *vals = malloc(sizeof(float) * d->nsync);
    sync_t **order = malloc(sizeof(sync_t *) * d->nsync);
    for (int i = 0; i < d->nsync; ++i) { vals[i] = d->sync[i].sync; order[i] = &d->sync[i]; }
    qsort(vals, d->nsync, sizeof(float), cmp_float);
    float norm = vals[d->nsync * 4 / 10];
    for (int i = 0; i < d->nsync; ++i) d->sync[i].sync /= norm;
    free(vals);

    qsort(order, d->nsync, sizeof(sync_t *), cmp_sync_desc);
    int nc = 0;
    for (int k = 0; k < d->nsync && nc < maxcand; ++k) {
        sync_t *e = order[k];
        if (!e->alive) continue;
        if (isnan(e->sync)) continue;
        if (e->sync < ASYNCMIN) break;
        cand[nc++] = *e;
        float lo = e->freq - d->az, hi = e->freq + d->az;
        /* entries are ordered by frequency */
        int idx = (int)(e - d->sync);
        for (int q = idx; q >= 0 && d->sync[q].freq >= lo; --q) d->sync[q].alive = 0;
        for (int q = idx; q < d->nsync && d->sync[q].freq <= hi; ++q) d->sync[q].alive = 0;
    }
    free(order);
    return nc;
}

/* ------------------------------------------------------------------ */
/* Downsample / fine sync                                               */
/* ------------------------------------------------------------------ */

static void baseband_fft(js8_decoder_t *d)
{
    memcpy(d->ds_in, d->dd, sizeof(float) * d->nmax);
    memset(d->ds_in + d->nmax, 0, sizeof(float) * (d->ndfft1 - d->nmax));
    kiss_fftr(d->p_bb, d->ds_in, (kiss_fft_cpx *)d->ds_cx);
}

static void downsample(js8_decoder_t *d, float f0)
{
    const float DF = 12000.0f / d->ndfft1;
    const float BAUD = 12000.0f / d->nsps;
    float ft = f0 + 8.5f * BAUD, fb = f0 - 1.5f * BAUD;
    int i0 = (int)lroundf(f0 / DF);
    int it = (int)lroundf(ft / DF); if (it > d->ndfft1 / 2) it = d->ndfft1 / 2;
    int ib = (int)lroundf(fb / DF); if (ib < 0) ib = 0;
    int range = it - ib + 1;
    int nddsz = d->ndd + 1;

    memset(d->cd0, 0, sizeof(cfloat) * d->ndfft2);
    memcpy(d->cd0, d->ds_cx + ib, sizeof(cfloat) * range);
    for (int k = 0; k < nddsz && k < range; ++k) d->cd0[k] *= d->taper[0][k];
    for (int k = 0; k < nddsz && k < range; ++k) d->cd0[range - nddsz + k] *= d->taper[1][k];

    /* rotate left by (i0 - ib) within [0, ndfft2) */
    int sh = i0 - ib;
    sh %= d->ndfft2; if (sh < 0) sh += d->ndfft2;
    memcpy(d->tmp, d->cd0, sizeof(cfloat) * d->ndfft2);
    for (int k = 0; k < d->ndfft2; ++k) d->cd0[k] = d->tmp[(k + sh) % d->ndfft2];

    kiss_fft(d->p_ds, (kiss_fft_cpx *)d->cd0, (kiss_fft_cpx *)d->tmp);
    float factor = 1.0f / sqrtf((float)d->ndfft1 * d->ndfft2);
    for (int k = 0; k < d->ndfft2; ++k) d->cd0[k] = d->tmp[k] * factor;
    for (int k = d->ndfft2; k < NP; ++k) d->cd0[k] = 0;
}

static float syncjs8d(js8_decoder_t *d, int i0, float delf)
{
    cfloat adj[32];
    const float base = TAU * (1.0f / (12000.0f / d->ndown));
    if (delf != 0.0f) {
        float dphi = base * delf, phi = 0.0f;
        for (int i = 0; i < d->ndownsps; ++i) {
            adj[i] = cexpf(I * phi);
            phi = fmodf(phi + dphi, TAU);
            if (phi < 0) phi += TAU;
        }
    } else {
        for (int i = 0; i < d->ndownsps; ++i) adj[i] = 1.0f;
    }
    float sync = 0.0f;
    for (int a = 0; a < 3; ++a)
        for (int j = 0; j < 7; ++j) {
            int off = 36 * a * d->ndownsps + i0 + j * d->ndownsps;
            if (off >= 0 && off + d->ndownsps <= d->np2) {
                cfloat z = 0;
                for (int k = 0; k < d->ndownsps; ++k)
                    z += d->cd0[off + k] * conjf(adj[k] * d->csyncs[a][j][k]);
                sync += crealf(z) * crealf(z) + cimagf(z) * cimagf(z);
            }
        }
    return sync;
}

static void subtract(js8_decoder_t *d, const uint8_t tones[NN], float f0, float dt)
{
    const int nref = NN * d->nsps;
    const float bfpi = TAU * f0 / 12000.0f;
    int nstart = (int)(dt * 12000.0f);
    int cs = nstart < 0 ? -nstart : 0;
    int ds = nstart > 0 ? nstart : 0;
    int size = nref - cs;
    if (d->nmax - ds < size) size = d->nmax - ds;
    if (size <= 0) return;

    /* generate cref on the fly (phase accumulator) into cf_tmp as reference */
    cfloat *cref = d->cf_tmp;
    float phi = 0.0f;
    for (int k = 0, n = 0; k < NN; ++k) {
        float dphi = bfpi + TAU * tones[k] / d->nsps;
        for (int i = 0; i < d->nsps; ++i, ++n) {
            if (n >= cs && n - cs < size) cref[n - cs] = cexpf(I * phi);
            phi = fmodf(phi + dphi, TAU);
        }
    }
    for (int i = 0; i < size; ++i) d->cfilt[i] = d->dd[ds + i] * conjf(cref[i]);
    memset(d->cfilt + size, 0, sizeof(cfloat) * (d->nmax - size));

    /* FFT -> filter -> IFFT. cref must survive, so use dd-sized scratch:
     * we reuse cfilt in place via a second buffer = ds_in is too small; do it
     * with the forward/back plans through 'tmpbuf'. */
    cfloat *tmpbuf = malloc(sizeof(cfloat) * d->nmax);
    if (!tmpbuf) return;
    kiss_fft(d->p_cf, (kiss_fft_cpx *)d->cfilt, (kiss_fft_cpx *)tmpbuf);
    for (int i = 0; i < d->nmax; ++i) tmpbuf[i] *= d->filter[i];
    kiss_fft(d->p_cb, (kiss_fft_cpx *)tmpbuf, (kiss_fft_cpx *)d->cfilt);
    free(tmpbuf);

    for (int i = 0; i < size; ++i) d->dd[ds + i] -= 2.0f * crealf(d->cfilt[i] * cref[i]);
}

/* ------------------------------------------------------------------ */
/* Single candidate decode                                              */
/* ------------------------------------------------------------------ */

static float max4(float a, float b, float c, float e)
{
    float m = a > b ? a : b;
    m = m > c ? m : c;
    return m > e ? m : e;
}

static void llr_metrics(float ps[8], float *o)
{
    o[0] = max4(ps[4], ps[5], ps[6], ps[7]) - max4(ps[0], ps[1], ps[2], ps[3]);
    o[1] = max4(ps[2], ps[3], ps[6], ps[7]) - max4(ps[0], ps[1], ps[4], ps[5]);
    o[2] = max4(ps[1], ps[3], ps[5], ps[7]) - max4(ps[0], ps[2], ps[4], ps[6]);
}

static void normalize_llr(float *llr, int n)
{
    float sum = 0, sq = 0;
    for (int i = 0; i < n; ++i) { sum += llr[i]; sq += llr[i] * llr[i]; }
    float av = sum / n, av2 = sq / n, var = av2 - av * av;
    float sig = sqrtf(var > 0 ? var : av2);
    if (sig <= 0) sig = 1;
    for (int i = 0; i < n; ++i) llr[i] = llr[i] / sig * 2.83f;
}

static int js8dec(js8_decoder_t *d, int lsubtract, float *f1, float *xdt,
                  float *xsnr, int *nhard, int *type, char text[13])
{
    const float FR = 12000.0f / d->nfft1;
    const float FS2 = 12000.0f / d->ndown;
    const float DT2 = 1.0f / FS2;
    int index = (int)lroundf(*f1 / FR);
    if (index < 0) index = 0;
    if (index >= d->nsps) index = d->nsps - 1;
    float xbase = powf(10.0f, 0.1f * (d->savg[index] - d->basesub));

    downsample(d, *f1);

    int i0 = (int)lroundf((*xdt + d->astart) * FS2), ibest = 0;
    float smax = 0;
    for (int idt = i0 - d->nqsymbol; idt <= i0 + d->nqsymbol; ++idt) {
        float s = syncjs8d(d, idt, 0.0f);
        if (s > smax) { smax = s; ibest = idt; }
    }
    float xdt2 = ibest * DT2;
    i0 = (int)lroundf(xdt2 * FS2);
    smax = 0;
    float delfbest = 0;
    for (int ifr = -NFSRCH; ifr <= NFSRCH; ++ifr) {
        float delf = ifr * 0.5f, s = syncjs8d(d, i0, delf);
        if (s > smax) { smax = s; delfbest = delf; }
    }
    {
        cfloat wstep = cexpf(I * (-delfbest * (TAU / FS2))), w = 1.0f;
        for (int i = 0; i < NP2_G; ++i) { w *= wstep; d->cd0[i] *= w; }
    }
    *xdt = xdt2;
    *f1 += delfbest;
    float sync = syncjs8d(d, i0, 0.0f);

    float s2[NROWS][NN];
    for (int k = 0; k < NN; ++k) {
        int i1 = ibest + k * d->ndownsps;
        if (i1 >= 0 && i1 + d->ndownsps <= NP2_G)
            memcpy(d->csymb, d->cd0 + i1, sizeof(cfloat) * d->ndownsps);
        else
            memset(d->csymb, 0, sizeof(cfloat) * d->ndownsps);
        kiss_fft(d->p_cs, (kiss_fft_cpx *)d->csymb, (kiss_fft_cpx *)d->cs_out);
        for (int i = 0; i < NROWS; ++i) s2[i][k] = cabsf(d->cs_out[i]) / 1000.0f;
    }

    int nsync = 0;
    for (int a = 0; a < 3; ++a)
        for (int c = 0; c < 7; ++c) {
            int col = a * 36 + c, best = 0;
            for (int r = 1; r < NROWS; ++r)
                if (s2[r][col] > s2[best][col]) best = r;
            if (d->costas[a][c] == best) ++nsync;
        }
    if (nsync <= 6) return 0;

    float llr0[3 * ND], llr1[3 * ND];
    for (int j = 0; j < ND; ++j) {
        int col = j < 29 ? 7 + j : 43 + (j - 29);
        float ps[8];
        for (int i = 0; i < 8; ++i) ps[i] = s2[i][col];
        llr_metrics(ps, &llr0[3 * j]);
        for (int i = 0; i < 8; ++i) ps[i] = logf(ps[i] + 1e-32f);
        llr_metrics(ps, &llr1[3 * j]);
    }
    normalize_llr(llr0, 3 * ND);
    normalize_llr(llr1, 3 * ND);

    int8_t decoded[K_BITS], cw[N_BITS];
    for (int ipass = 1; ipass <= 4; ++ipass) {
        const float *llr = ipass == 2 ? llr1 : llr0;
        if (ipass == 3) memset(llr0, 0, sizeof(float) * 24);
        else if (ipass == 4) memset(llr0 + 24, 0, sizeof(float) * 24);
        int nh = bpdecode174(llr, decoded, cw);
        *xsnr = -99.0f;
        int allzero = 1;
        for (int i = 0; i < N_BITS; ++i) if (cw[i]) { allzero = 0; break; }
        if (allzero) continue;
        if (nh >= 0 && nh < 60 && !(sync < 2.0f && nh > 35) &&
            !(ipass > 2 && nh > 39) && !(ipass == 4 && nh > 30)) {
            if (!check_crc(decoded)) continue;
            for (int i = 0; i < 12; ++i) {
                int w = 0;
                for (int b = 0; b < 6; ++b) w = (w << 1) | decoded[i * 6 + b];
                text[i] = k_alphabet[w];
            }
            text[12] = 0;
            *type = (decoded[72] << 2) | (decoded[73] << 1) | decoded[74];
            uint8_t itone[NN];
            js8_encode(d->mode, *type, text, itone);
            if (lsubtract) subtract(d, itone, *f1, xdt2);
            float xsig = 0;
            for (int i = 0; i < NN; ++i) xsig += s2[itone[i]][i] * s2[itone[i]][i];
            float r = xsig / xbase - 1.0f;
            if (r < 1.259e-10f) r = 1.259e-10f;
            *xsnr = 10.0f * log10f(r) - 32.0f;
            if (*xsnr < -60.0f) *xsnr = -60.0f;
            *nhard = nh;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public decode                                                        */
/* ------------------------------------------------------------------ */

static int g_fqso;
static int cmp_cand(const void *a, const void *b)
{
    const sync_t *x = a, *y = b;
    float ad = fabsf(x->freq - g_fqso), bd = fabsf(y->freq - g_fqso);
    if (ad < 10.0f && bd >= 10.0f) return -1;
    if (bd < 10.0f && ad >= 10.0f) return 1;
    if (ad != bd) return ad < bd ? -1 : 1;
    return (x->freq > y->freq) - (x->freq < y->freq);
}

int js8_decode(js8_decoder_t *d, const int16_t *audio, int nsamples,
               int fmin, int fmax, int fqso, js8_decode_t *out, int max_out)
{
    static sync_t cand[NMAXCAND];
    int nout = 0;
    memset(d->dd, 0, sizeof(float) * d->nmax);
    if (nsamples > d->nmax) nsamples = d->nmax;
    for (int i = 0; i < nsamples; ++i) d->dd[i] = audio[i];

    for (int ipass = 1; ipass <= 3; ++ipass) {
        int nc = syncjs8(d, fmin, fmax, cand, NMAXCAND);
        if (!nc) break;
        g_fqso = fqso;
        qsort(cand, nc, sizeof(sync_t), cmp_cand);
        baseband_fft(d);
        int improved = 0;
        for (int c = 0; c < nc; ++c) {
            float f1 = cand[c].freq, xdt = cand[c].step, xsnr = 0;
            int nh = -1, type = 0;
            char text[13];
            if (!js8dec(d, ipass < 3, &f1, &xdt, &xsnr, &nh, &type, text)) continue;
            int snr = (int)lroundf(xsnr);
            int found = -1;
            for (int k = 0; k < nout; ++k)
                if (out[k].frame_type == type && !strcmp(out[k].text, text)) { found = k; break; }
            if (found >= 0) {
                if (out[found].snr < snr) { out[found].snr = snr; improved = 1; }
                continue;
            }
            if (nout >= max_out) continue;
            js8_decode_t *o = &out[nout++];
            o->snr = snr;
            o->dt = xdt - d->astart;
            o->freq = f1;
            o->frame_type = type;
            o->quality = 1.0f - nh / 60.0f;
            memcpy(o->text, text, 13);
            improved = 1;
        }
        if (!improved) break;
    }
    return nout;
}
