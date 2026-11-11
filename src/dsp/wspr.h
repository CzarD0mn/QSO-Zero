/* QSO-Zero - WSPR type-1 beacon encoder (transmit). */
#ifndef QSOZERO_WSPR_H
#define QSOZERO_WSPR_H

#include <stdint.h>
#include <stddef.h>

#define WSPR_SYMBOLS      162
#define WSPR_SYMBOL_SAMPS 8192            /* at 12 kHz: 0.682667 s per symbol */
#define WSPR_TONE_SPACING (12000.0f / 8192.0f)   /* 1.4648 Hz */
#define WSPR_PERIOD_S     120

/* Encode "K1ABC", "FN42", 23 dBm into 162 4-FSK channel symbols (0..3).
 * Power is rounded to the nearest legal WSPR value (ends in 0, 3 or 7).
 * Returns 0 on success, -1 if callsign/grid are not type-1 encodable. */
int wspr_encode(const char *call, const char *grid4, int dbm, uint8_t sym[WSPR_SYMBOLS]);

/* Continuous phase 4-FSK synthesis; f0 = centre of the 4 tones (audio Hz,
 * 1400-1600), matching WSJT-X and wsprnet frequency reporting. out must hold 162 * 8192 * rate / 12000 floats. */
size_t wspr_synth(const uint8_t sym[WSPR_SYMBOLS], float f0, int rate, float amp, float *out);

/* Snap a requested power to the nearest valid WSPR dBm value. */
int wspr_valid_dbm(int dbm);

#endif
