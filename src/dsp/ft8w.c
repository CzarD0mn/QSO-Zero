/* NeonQRP - FT8/FT4 wrapper around ft8_lib (MIT). Adds a callsign hash
 * table, duplicate suppression, an SNR estimate and GFSK synthesis. */
#include "ft8w.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "common/monitor.h"
#include "ft8/constants.h"
#include "ft8/decode.h"
#include "ft8/encode.h"
#include "ft8/message.h"

#define MAX_CAND   140
#define MIN_SCORE  10
#define LDPC_ITERS 25
#define HASH_SIZE  256

struct ft8w {
    ft8w_proto_t proto;
    int sample_rate;
    monitor_t mon;
    float *fbuf;
};

/* ---- callsign hash table (shared by all instances) ---- */
static struct { char call[12]; uint32_t hash; } g_hash[HASH_SIZE];

static void hash_age(void)
{
    for (int i = 0; i < HASH_SIZE; ++i)
        if (g_hash[i].call[0]) {
            uint8_t age = g_hash[i].hash >> 24;
            if (age > 20) { g_hash[i].call[0] = 0; g_hash[i].hash = 0; }
            else g_hash[i].hash = ((uint32_t)(age + 1) << 24) | (g_hash[i].hash & 0x3FFFFF);
        }
}
static void hash_add(const char *call, uint32_t hash)
{
    int idx = (((hash >> 12) & 0x3FF) * 23) % HASH_SIZE;
    for (int n = 0; n < HASH_SIZE && g_hash[idx].call[0]; ++n) {
        if ((g_hash[idx].hash & 0x3FFFFF) == hash && !strcmp(g_hash[idx].call, call)) {
            g_hash[idx].hash &= 0x3FFFFF;
            return;
        }
        idx = (idx + 1) % HASH_SIZE;
    }
    strncpy(g_hash[idx].call, call, 11);
    g_hash[idx].call[11] = 0;
    g_hash[idx].hash = hash;
}
static bool hash_lookup(ftx_callsign_hash_type_t t, uint32_t hash, char *call)
{
    uint8_t sh = t == FTX_CALLSIGN_HASH_10_BITS ? 12 : t == FTX_CALLSIGN_HASH_12_BITS ? 10 : 0;
    int idx = (((hash >> (12 - sh)) & 0x3FF) * 23) % HASH_SIZE;
    for (int n = 0; n < HASH_SIZE && g_hash[idx].call[0]; ++n) {
        if (((g_hash[idx].hash & 0x3FFFFF) >> sh) == hash) { strcpy(call, g_hash[idx].call); return true; }
        idx = (idx + 1) % HASH_SIZE;
    }
    call[0] = 0;
    return false;
}
static ftx_callsign_hash_interface_t g_hash_if = {.lookup_hash = hash_lookup, .save_hash = hash_add};

/* ---- lifecycle ---- */

ft8w_t *ft8w_new(ft8w_proto_t proto, int sample_rate, float fmin, float fmax)
{
    ft8w_t *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->proto = proto;
    w->sample_rate = sample_rate;
    monitor_config_t cfg = {
        .f_min = fmin, .f_max = fmax, .sample_rate = sample_rate,
        .time_osr = 2, .freq_osr = 2,
        .protocol = proto == FT8W_FT4 ? FTX_PROTOCOL_FT4 : FTX_PROTOCOL_FT8,
    };
    monitor_init(&w->mon, &cfg);
    w->fbuf = malloc(sizeof(float) * w->mon.block_size);
    return w;
}

void ft8w_free(ft8w_t *w)
{
    if (!w) return;
    monitor_free(&w->mon);
    free(w->fbuf);
    free(w);
}

int ft8w_slot_samples(const ft8w_t *w)
{
    float slot = w->proto == FT8W_FT4 ? FT4_SLOT_TIME : FT8_SLOT_TIME;
    return (int)(slot * w->sample_rate);
}

/* SNR estimate in 2500 Hz: mean power on the active tone vs mean power on
 * the other seven tone bins, over the data symbols. */
static int estimate_snr(const ftx_waterfall_t *wf, const ftx_candidate_t *c, const uint8_t *tones, int ntones)
{
    double sig = 0, noise = 0;
    int ns = 0;
    for (int k = 0; k < ntones; ++k) {
        int block = c->time_offset + k;
        if (block < 0 || block >= wf->num_blocks) continue;
        const WF_ELEM_T *row = wf->mag + (size_t)block * wf->block_stride +
                               (c->time_sub * wf->freq_osr + c->freq_sub) * wf->num_bins + c->freq_offset;
        for (int t = 0; t < 8; ++t) {
            if (c->freq_offset + t >= wf->num_bins) break;
            double p = pow(10.0, WF_ELEM_MAG(row[t]) / 10.0);
            if (t == tones[k]) sig += p; else noise += p / 7.0;
        }
        ++ns;
    }
    if (!ns || noise <= 0) return -30;
    double r = sig / noise - 1.0;
    if (r < 1e-3) r = 1e-3;
    int snr = (int)lround(10.0 * log10(r) - 26.0);   /* 6.25 Hz bin -> 2500 Hz */
    return snr < -30 ? -30 : snr > 30 ? 30 : snr;
}

int ft8w_decode(ft8w_t *w, const int16_t *audio, int n, ft8w_decode_t *out, int max_out)
{
    monitor_reset(&w->mon);
    for (int pos = 0; pos + w->mon.block_size <= n; pos += w->mon.block_size) {
        for (int i = 0; i < w->mon.block_size; ++i) w->fbuf[i] = audio[pos + i] / 32768.0f;
        monitor_process(&w->mon, w->fbuf);
    }
    const ftx_waterfall_t *wf = &w->mon.wf;
    ftx_candidate_t cand[MAX_CAND];
    int nc = ftx_find_candidates(wf, MAX_CAND, cand, MIN_SCORE);
    ftx_message_t seen[64];
    int nseen = 0, nout = 0;
    for (int i = 0; i < nc && nout < max_out; ++i) {
        ftx_message_t msg;
        ftx_decode_status_t st;
        if (!ftx_decode_candidate(wf, &cand[i], LDPC_ITERS, &msg, &st)) continue;
        int dup = 0;
        for (int k = 0; k < nseen; ++k)
            if (!memcmp(seen[k].payload, msg.payload, sizeof msg.payload)) { dup = 1; break; }
        if (dup) continue;
        if (nseen < 64) seen[nseen++] = msg;
        ft8w_decode_t *o = &out[nout];
        ftx_message_offsets_t offs;
        if (ftx_message_decode(&msg, &g_hash_if, o->text, &offs) != FTX_MESSAGE_RC_OK) continue;
        o->freq = (w->mon.min_bin + cand[i].freq_offset + (float)cand[i].freq_sub / wf->freq_osr) / w->mon.symbol_period;
        /* ft8_lib's STFT frames lag the signal by 0.14 s (measured: an on-time
         * signal at +0.5 s reads 0.64 s); report DT the way WSJT-X does */
        o->dt = (cand[i].time_offset + (float)cand[i].time_sub / wf->time_osr) * w->mon.symbol_period - 0.64f;
        uint8_t tones[FT4_NN > FT8_NN ? FT4_NN : FT8_NN];
        int nt;
        if (w->proto == FT8W_FT4) { ft4_encode(msg.payload, tones); nt = FT4_NN; }
        else { ft8_encode(msg.payload, tones); nt = FT8_NN; }
        o->snr = estimate_snr(wf, &cand[i], tones, nt);
        ++nout;
    }
    hash_age();
    return nout;
}

int ft8w_encode(ft8w_proto_t proto, const char *text, uint8_t *tones)
{
    ftx_message_t msg;
    if (ftx_message_encode(&msg, &g_hash_if, text) != FTX_MESSAGE_RC_OK) return -1;
    if (proto == FT8W_FT4) { ft4_encode(msg.payload, tones); return FT4_NN; }
    ft8_encode(msg.payload, tones);
    return FT8_NN;
}

#define GFSK_K 5.336446f /* pi * sqrt(2 / ln 2) */

size_t ft8w_synth(ft8w_proto_t proto, const uint8_t *sym, int nsym, float f0,
                  int rate, float amp, float *out)
{
    float period = proto == FT8W_FT4 ? FT4_SYMBOL_PERIOD : FT8_SYMBOL_PERIOD;
    float bt = proto == FT8W_FT4 ? 1.0f : 2.0f;
    int nsps = (int)(0.5f + rate * period);
    int nwave = nsym * nsps;
    float *dphi = malloc(sizeof(float) * (nwave + 2 * nsps));
    float *pulse = malloc(sizeof(float) * 3 * nsps);
    if (!dphi || !pulse) { free(dphi); free(pulse); return 0; }
    for (int i = 0; i < 3 * nsps; ++i) {
        float t = i / (float)nsps - 1.5f;
        pulse[i] = (erff(GFSK_K * bt * (t + 0.5f)) - erff(GFSK_K * bt * (t - 0.5f))) / 2;
    }
    float peak = 2 * (float)M_PI / nsps;
    for (int i = 0; i < nwave + 2 * nsps; ++i) dphi[i] = 2 * (float)M_PI * f0 / rate;
    for (int i = 0; i < nsym; ++i)
        for (int j = 0; j < 3 * nsps; ++j) dphi[j + i * nsps] += peak * sym[i] * pulse[j];
    for (int j = 0; j < 2 * nsps; ++j) {
        dphi[j] += peak * pulse[j + nsps] * sym[0];
        dphi[j + nsym * nsps] += peak * pulse[j] * sym[nsym - 1];
    }
    float phi = 0;
    for (int k = 0; k < nwave; ++k) {
        out[k] = amp * sinf(phi);
        phi = fmodf(phi + dphi[k + nsps], 2 * (float)M_PI);
    }
    int ramp = nsps / 8;
    for (int i = 0; i < ramp; ++i) {
        float env = (1 - cosf(2 * (float)M_PI * i / (2 * ramp))) / 2;
        out[i] *= env;
        out[nwave - 1 - i] *= env;
    }
    free(dphi);
    free(pulse);
    return (size_t)nwave;
}
