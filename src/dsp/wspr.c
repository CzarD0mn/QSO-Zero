/* NeonQRP - WSPR type-1 encoder, written from the protocol description
 * (K1JT / G4JNT "Non-normative specification of WSPR protocol"). Verified
 * against Etherkit JTEncode in tests/test_wspr.c. */
#include "wspr.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t k_sync[WSPR_SYMBOLS] = {
    1,1,0,0,0,0,0,0,1,0,0,0,1,1,1,0,0,0,1,0,0,1,0,1,1,1,1,0,0,0,0,0,0,0,1,0,0,1,0,1,
    0,0,0,0,0,0,1,0,1,1,0,0,1,1,0,1,0,0,0,1,1,0,1,0,0,0,0,1,1,0,1,0,1,0,1,0,1,0,0,1,
    0,0,1,0,1,1,0,0,0,1,1,0,1,0,1,0,0,0,1,0,0,0,0,0,1,0,0,1,0,0,1,1,1,0,1,1,0,0,1,1,
    0,1,0,0,0,1,1,1,0,0,0,0,0,1,0,1,0,0,1,1,0,0,0,0,0,0,0,1,1,0,1,0,1,1,0,0,0,1,1,0,
    0,0};

int wspr_valid_dbm(int dbm)
{
    static const int ok[] = {0,3,7,10,13,17,20,23,27,30,33,37,40,43,47,50,53,57,60};
    int best = ok[0];
    for (size_t i = 0; i < sizeof ok / sizeof *ok; ++i)
        if (abs(ok[i] - dbm) < abs(best - dbm)) best = ok[i];
    return best;
}

static int cv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    if (c == ' ') return 36;
    return -1;
}

static int parity32(uint32_t v)
{
    return __builtin_parity(v);
}

int wspr_encode(const char *call_in, const char *grid_in, int dbm, uint8_t sym[WSPR_SYMBOLS])
{
    char call[7] = "      ", c[8];
    size_t n = 0;
    for (const char *p = call_in; *p && n < 7; ++p)
        if (!isspace((unsigned char)*p)) c[n++] = (char)toupper((unsigned char)*p);
    c[n] = 0;
    if (n < 2 || n > 6) return -1;
    /* third character must be a digit: right-align a 1-letter prefix */
    if (isdigit((unsigned char)c[2])) memcpy(call, c, n);
    else if (isdigit((unsigned char)c[1]) && n <= 5) memcpy(call + 1, c, n);
    else return -1;

    if (strlen(grid_in) < 4) return -1;
    char g[4];
    for (int i = 0; i < 4; ++i) g[i] = (char)toupper((unsigned char)grid_in[i]);
    if (g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R' || !isdigit((unsigned char)g[2]) || !isdigit((unsigned char)g[3]))
        return -1;
    dbm = wspr_valid_dbm(dbm);

    uint32_t N = cv(call[0]);
    N = N * 36 + cv(call[1]);
    N = N * 10 + cv(call[2]);
    for (int i = 3; i < 6; ++i) {
        int v = cv(call[i]);
        if (v < 10) return -1;          /* suffix must be letters or space */
        N = N * 27 + (v - 10);
    }
    uint32_t M = (uint32_t)((179 - 10 * (g[0] - 'A') - (g[2] - '0')) * 180 + 10 * (g[1] - 'A') + (g[3] - '0'));
    M = M * 128 + dbm + 64;

    uint8_t b[11] = {0};
    b[0] = N >> 20; b[1] = N >> 12; b[2] = N >> 4;
    b[3] = (uint8_t)(((N & 0x0F) << 4) | ((M >> 18) & 0x0F));
    b[4] = M >> 10; b[5] = M >> 2; b[6] = (uint8_t)((M & 0x03) << 6);

    uint8_t conv[WSPR_SYMBOLS];
    uint32_t reg = 0;
    int k = 0;
    for (int i = 0; i < 81; ++i) {
        int bit = (b[i >> 3] >> (7 - (i & 7))) & 1;
        reg = (reg << 1) | bit;
        conv[k++] = (uint8_t)parity32(reg & 0xF2D05351u);
        conv[k++] = (uint8_t)parity32(reg & 0xE4613C47u);
    }
    uint8_t il[WSPR_SYMBOLS];
    int p = 0;
    for (int i = 0; i < 256 && p < WSPR_SYMBOLS; ++i) {
        int j = 0;
        for (int bt = 0; bt < 8; ++bt) if (i & (1 << bt)) j |= 1 << (7 - bt);
        if (j < WSPR_SYMBOLS) il[j] = conv[p++];
    }
    for (int i = 0; i < WSPR_SYMBOLS; ++i) sym[i] = (uint8_t)(k_sync[i] + 2 * il[i]);
    return 0;
}

size_t wspr_synth(const uint8_t sym[WSPR_SYMBOLS], float f0, int rate, float amp, float *out)
{
    size_t nsps = (size_t)llround((double)WSPR_SYMBOL_SAMPS * rate / 12000.0);
    double phi = 0;
    size_t n = 0, ramp = nsps / 16;
    size_t total = nsps * WSPR_SYMBOLS;
    for (int s = 0; s < WSPR_SYMBOLS; ++s) {
        /* f0 is the centre of the 4 tones, as in WSJT-X and on wsprnet */
        double dphi = 2 * M_PI * (f0 + (sym[s] - 1.5) * WSPR_TONE_SPACING) / rate;
        for (size_t i = 0; i < nsps; ++i, ++n) {
            float a = amp;
            if (n < ramp) a *= 0.5f * (1 - cosf((float)M_PI * n / ramp));
            else if (n >= total - ramp) a *= 0.5f * (1 - cosf((float)M_PI * (total - 1 - n) / ramp));
            out[n] = a * (float)sin(phi);
            phi += dphi;
            if (phi > 2 * M_PI) phi -= 2 * M_PI;
        }
    }
    return n;
}
