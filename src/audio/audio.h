/* NeonQRP - audio I/O. ALSA is loaded with dlopen so the binary has no
 * build-time dependency on libasound headers. */
#ifndef NEONQRP_AUDIO_H
#define NEONQRP_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#define AUDIO_HW_RATE 48000
#define AUDIO_DSP_RATE 12000

/* Called from the capture thread with 12 kHz samples and the corrected
 * UTC of the first sample. */
typedef void (*audio_rx_cb)(const int16_t *pcm, int n, double utc_first, void *user);

int  audio_open(const char *in_dev, const char *out_dev, audio_rx_cb cb, void *user);
void audio_close(void);
const char *audio_error(void);

/* Blocking playback of a float buffer at AUDIO_HW_RATE (-1..1). Returns 0
 * when done, -1 on error, 1 if aborted by audio_tx_abort(). */
int  audio_play(const float *buf, size_t n, float gain);
void audio_tx_abort(void);

/* Peak input level 0..1 over the last ~0.5 s (for the RX level meter). */
float audio_rx_level(void);

/* Find an ALSA card whose /proc/asound/cards line contains any of the
 * '|'-separated hints; writes "plughw:CARD=<id>" to out. */
int  audio_find_card(const char *hints, char *out, size_t outsz);

/* 4:1 decimator, exposed for tests */
typedef struct { float hist[2 * 96]; int pos, phase; } decim4_t;
void decim4_init(decim4_t *d);
int  decim4_run(decim4_t *d, const int16_t *in, int n, int16_t *out);

#endif
