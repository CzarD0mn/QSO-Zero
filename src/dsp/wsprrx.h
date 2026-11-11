/* NeonQRP - WSPR receive: 12 kHz audio -> 375 Hz baseband -> wsprd decoder. */
#ifndef NEONQRP_WSPRRX_H
#define NEONQRP_WSPRRX_H

#include <stdint.h>

#define WSPRRX_SAMPLES (114 * 12000)   /* audio needed, starting at the even minute */

typedef struct {
    int   snr;          /* dB in 2500 Hz */
    float dt;           /* s, 0 = started 1 s after the even minute */
    float freq;         /* audio Hz */
    int   drift;        /* Hz per transmission */
    char  call[13], grid[7];
    int   dbm;
    char  text[24];     /* "K1ABC FN42 37" */
} wsprrx_spot_t;

/* audio: 12 kHz mono from the start of an even UTC minute; n should be at
 * least WSPRRX_SAMPLES. Searches 1500 +/- 110 Hz (the WSPR sub-band).
 * Returns number of spots (<= max). Blocks for a few seconds on a Pi. */
int wsprrx_decode(const int16_t *audio, int n, wsprrx_spot_t *out, int max);

#endif
