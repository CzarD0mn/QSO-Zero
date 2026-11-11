/* NeonQRP - FT8/FT4 wrapper around ft8_lib (MIT, Karlis Goba YL3JG). */
#ifndef NEONQRP_FT8W_H
#define NEONQRP_FT8W_H

#include <stdint.h>
#include <stddef.h>

#define FT8W_MAX_TEXT 40

typedef enum { FT8W_FT8 = 0, FT8W_FT4 = 1 } ft8w_proto_t;

typedef struct {
    int   snr;
    float dt;
    float freq;
    char  text[FT8W_MAX_TEXT];
} ft8w_decode_t;

typedef struct ft8w ft8w_t;

ft8w_t *ft8w_new(ft8w_proto_t proto, int sample_rate, float fmin, float fmax);
void    ft8w_free(ft8w_t *w);

/* Samples per slot the decoder consumes (15 s FT8 / 7.5 s FT4). */
int ft8w_slot_samples(const ft8w_t *w);

/* Decode one slot of mono int16 audio. Returns number of decodes. */
int ft8w_decode(ft8w_t *w, const int16_t *audio, int n, ft8w_decode_t *out, int max_out);

/* Encode standard message text ("CQ K1ABC FN42", "K1ABC W9XYZ -12", ...).
 * tones must hold 79 (FT8) or 105 (FT4). Returns number of tones or -1. */
int ft8w_encode(ft8w_proto_t proto, const char *text, uint8_t *tones);

/* GFSK synthesis exactly as WSJT-X shapes it (BT=2 FT8, BT=1 FT4). Returns
 * number of samples written; out must hold ntones * sample_rate * symbol_period. */
size_t ft8w_synth(ft8w_proto_t proto, const uint8_t *tones, int ntones, float f0,
                  int sample_rate, float amplitude, float *out);

#endif
