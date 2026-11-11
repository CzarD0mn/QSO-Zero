/*
 * QSO-Zero - JS8 modem (C99 port of the JS8Call JS8.cpp engine)
 *
 * Based on JS8Call (C) Jordan Sherer KN4CRD and Allan Bazinet W6BAZ, and on
 * WSJT-X by Joe Taylor K1JT et al. Licensed GPLv3, see LICENSE.
 *
 * This is the transport layer: a 12-character frame (64-char alphabet) plus a
 * 3-bit frame type, protected by CRC-12 and LDPC(174,87), sent as 79 8-FSK
 * symbols. Message-level packing (heartbeat, directed, data frames) lives in
 * js8_varicode.c.
 */
#ifndef QSOZERO_JS8_H
#define QSOZERO_JS8_H

#include <stdint.h>
#include <stddef.h>

#define JS8_SAMPLE_RATE   12000
#define JS8_NUM_SYMBOLS   79
#define JS8_FRAME_CHARS   12

typedef enum {
    JS8_NORMAL = 0,   /* JS8A: 15 s, 6.25 Hz spacing  */
    JS8_FAST   = 1,   /* JS8B: 10 s, 10 Hz spacing    */
    JS8_TURBO  = 2,   /* JS8C:  6 s, 20 Hz spacing    */
    JS8_SLOW   = 3,   /* JS8E: 30 s, 3.125 Hz spacing */
    JS8_ULTRA  = 4,   /* JS8I:  4 s (unused on air)   */
    JS8_NUM_SUBMODES
} js8_submode_t;

typedef struct {
    const char *name;
    int   symbol_samples;   /* samples per symbol at 12 kHz */
    int   start_delay_ms;   /* TX starts this long after the period boundary */
    int   period_s;         /* T/R period */
    int   costas_modified;  /* 0 = original arrays (Normal), 1 = modified */
    int   snr_threshold;
    float tone_spacing;     /* Hz */
    float bandwidth;        /* Hz, 8 tones */
} js8_submode_info_t;

const js8_submode_info_t *js8_submode_info(js8_submode_t m);

/* Returns 0 on success, -1 if the frame contains a character outside the
 * JS8 alphabet "0-9A-Za-z-+". frame must be exactly 12 chars. */
int js8_encode(js8_submode_t mode, int frame_type, const char frame[12],
               uint8_t tones[JS8_NUM_SYMBOLS]);

/* Synthesise a continuous-phase 8-FSK waveform for the tones. out must hold
 * 79 * symbol_samples(at sample_rate) floats. Returns samples written. A short
 * raised-cosine ramp is applied at the start and end to keep the PA clean. */
size_t js8_synth(js8_submode_t mode, const uint8_t tones[JS8_NUM_SYMBOLS],
                 float f0, int sample_rate, float amplitude, float *out);

typedef struct {
    int   snr;         /* dB in 2500 Hz */
    float dt;          /* seconds relative to expected start */
    float freq;        /* audio Hz of tone 0 */
    int   frame_type;  /* 3-bit i3 */
    float quality;     /* 0..1 */
    char  text[JS8_FRAME_CHARS + 1];
} js8_decode_t;

typedef struct js8_decoder js8_decoder_t;

/* One decoder instance per submode; allocates its FFT plans and buffers. */
js8_decoder_t *js8_decoder_new(js8_submode_t mode);
void           js8_decoder_free(js8_decoder_t *d);

/* Number of 12 kHz samples the decoder wants for one period. */
int js8_decoder_samples(const js8_decoder_t *d);

/* Decode one period of 12 kHz mono audio (int16). Signals are searched
 * between fmin and fmax Hz; candidates near fqso are tried first.
 * Returns number of unique decodes written to out (up to max_out). */
int js8_decode(js8_decoder_t *d, const int16_t *audio, int nsamples,
               int fmin, int fmax, int fqso, js8_decode_t *out, int max_out);

#endif
