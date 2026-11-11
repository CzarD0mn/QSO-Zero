/*
 * NeonQRP - JS8 message layer. C port of JS8Call varicode.cpp, jsc.cpp and
 * decodedtext.cpp (C) 2018 Jordan Sherer KN4CRD, GPLv3.
 *
 * The Qt regular expressions are replaced by small hand-written matchers that
 * follow the same alternation order and backtracking behaviour, so frames are
 * bit-identical to what JS8Call produces.
 */
#include "js8_varicode.h"
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define LINE_MAX_CH 1024

/* ------------------------------------------------------------------ */
/* Tables                                                               */
/* ------------------------------------------------------------------ */

static const char ALPHA72[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+/?.";
static const char ALPHA41[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+-./?";
static const char ALNUM[]   = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ /@";

#define NBASECALL (37u * 36u * 10u * 27u * 27u * 27u)
#define NBASEGRID (180 * 180)
#define NUSERGRID (NBASEGRID + 10)
#define NMAXGRID  ((1 << 15) - 1)

typedef struct { const char *key; int val; } kv_t;

static const kv_t k_cmds[] = {
    {" HEARTBEAT", -1}, {" HB", -1}, {" CQ", -1}, {" SNR?", 0}, {"?", 0},
    {" DIT DIT", 1}, {" HEARING?", 3}, {" GRID?", 4}, {">", 5}, {" STATUS?", 6},
    {" STATUS", 7}, {" HEARING", 8}, {" MSG", 9}, {" MSG TO:", 10}, {" QUERY", 11},
    {" QUERY MSGS", 12}, {" QUERY MSGS?", 12}, {" QUERY CALL", 13}, {" GRID", 15},
    {" INFO?", 16}, {" INFO", 17}, {" FB", 18}, {" HW CPY?", 19}, {" SK", 20},
    {" RR", 21}, {" QSL?", 22}, {" QSL", 23}, {" CMD", 24}, {" SNR", 25}, {" NO", 26},
    {" YES", 27}, {" 73", 28}, {" NACK", 2}, {" ACK", 14}, {" HEARTBEAT SNR", 29},
    {" AGN?", 30}, {"  ", 31}, {" ", 31},
};
#define NCMDS ((int)(sizeof k_cmds / sizeof k_cmds[0]))

static const int k_buffered[] = {5, 9, 10, 11, 12, 13, 15, 24};
static const struct { int cmd, bits; } k_checksum[] = {
    {5, 16}, {9, 16}, {10, 16}, {11, 16}, {12, 16}, {13, 16}, {15, 0}, {24, 16}};

static const char *const k_basecalls[] = {
    NULL, "<....>", "@ALLCALL", "@JS8NET", "@DX/NA", "@DX/SA", "@DX/EU", "@DX/AS",
    "@DX/AF", "@DX/OC", "@DX/AN", "@REGION/1", "@REGION/2", "@REGION/3", "@GROUP/0",
    "@GROUP/1", "@GROUP/2", "@GROUP/3", "@GROUP/4", "@GROUP/5", "@GROUP/6", "@GROUP/7",
    "@GROUP/8", "@GROUP/9", "@COMMAND", "@CONTROL", "@NET", "@NTS", "@RESERVE/0",
    "@RESERVE/1", "@RESERVE/2", "@RESERVE/3", "@RESERVE/4", "@APRSIS", "@RAGCHEW",
    "@JS8", "@EMCOMM", "@ARES", "@MARS", "@AMRRON", "@RACES", "@RAYNET", "@RADAR",
    "@SKYWARN", "@CQ", "@HB", "@QSO", "@QSOPARTY", "@CONTEST", "@FIELDDAY", "@SOTA",
    "@IOTA", "@POTA", "@QRP", "@QRO",
};
#define NBASECALLS ((int)(sizeof k_basecalls / sizeof k_basecalls[0]))

static const char *const k_cqs[8] = {"CQ CQ CQ", "CQ DX", "CQ QRP", "CQ CONTEST",
                                     "CQ FIELD", "CQ FD", "CQ CQ", "CQ"};

static const struct { char ch; const char *code; } k_huff[] = {
    {' ', "01"}, {'E', "100"}, {'T', "1101"}, {'A', "0011"}, {'O', "11111"},
    {'I', "11100"}, {'N', "10111"}, {'S', "10100"}, {'H', "00011"}, {'R', "00000"},
    {'D', "111011"}, {'L', "110011"}, {'C', "110001"}, {'U', "101101"}, {'M', "101011"},
    {'W', "001011"}, {'F', "001001"}, {'G', "000101"}, {'Y', "000011"}, {'P', "1111011"},
    {'B', "1111001"}, {'.', "1110100"}, {'V', "1100101"}, {'K', "1100100"}, {'-', "1100001"},
    {'+', "1100000"}, {'?', "1011001"}, {'!', "1011000"}, {'"', "1010101"}, {'X', "1010100"},
    {'0', "0010101"}, {'J', "0010100"}, {'1', "0010001"}, {'Q', "0010000"}, {'2', "0001001"},
    {'Z', "0001000"}, {'3', "0000101"}, {'5', "0000100"}, {'4', "11110101"}, {'9', "11110100"},
    {'8', "11110001"}, {'6', "11110000"}, {'7', "11101011"}, {'/', "11101010"},
};
#define NHUFF ((int)(sizeof k_huff / sizeof k_huff[0]))

/* ------------------------------------------------------------------ */
/* Small string helpers                                                 */
/* ------------------------------------------------------------------ */

static int idx_of(const char *alpha, char c)
{
    const char *p = c ? strchr(alpha, c) : NULL;
    return p ? (int)(p - alpha) : -1;
}
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
static int is_word(char c) { return isalnum((unsigned char)c) || c == '_'; }
static int starts_with(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
static const char *lstrip(const char *s) { while (*s && is_space(*s)) ++s; return s; }

static int cmd_lookup(const char *key)
{
    for (int i = 0; i < NCMDS; ++i)
        if (!strcmp(k_cmds[i].key, key)) return i;
    return -1;
}
/* QMap::key(value): first key in sorted order with that value */
static const char *cmd_key(int val)
{
    const char *best = NULL;
    for (int i = 0; i < NCMDS; ++i)
        if (k_cmds[i].val == val && (!best || strcmp(k_cmds[i].key, best) < 0)) best = k_cmds[i].key;
    return best ? best : "";
}
static int cmd_allowed(const char *cmd) { return cmd_lookup(cmd) >= 0; }
static int cmd_is_snr(const char *cmd)
{
    int i = cmd_lookup(cmd);
    return i >= 0 && (k_cmds[i].val == 25 || k_cmds[i].val == 29);
}
static int cmd_buffered(const char *cmd)
{
    int i = cmd_lookup(cmd);
    if (i < 0) return 0;
    if (strchr(cmd, ' ')) return 1;
    for (size_t k = 0; k < sizeof k_buffered / sizeof k_buffered[0]; ++k)
        if (k_buffered[k] == k_cmds[i].val) return 1;
    return 0;
}
static int cmd_checksum(const char *cmd)
{
    int i = cmd_lookup(cmd);
    if (i < 0) return -1;
    for (size_t k = 0; k < sizeof k_checksum / sizeof k_checksum[0]; ++k)
        if (k_checksum[k].cmd == k_cmds[i].val) return k_checksum[k].bits;
    return -1;
}
static int basecall_index(const char *s)
{
    for (int i = 1; i < NBASECALLS; ++i)
        if (!strcmp(k_basecalls[i], s)) return i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* 72-bit frame helpers                                                 */
/* ------------------------------------------------------------------ */

typedef struct { uint8_t b[96]; int n; } bitvec_t;

static void bv_push(bitvec_t *v, uint64_t val, int width)
{
    for (int i = width - 1; i >= 0; --i) v->b[v->n++] = (val >> i) & 1;
}
static uint64_t bv_get(const uint8_t *b, int off, int width)
{
    uint64_t v = 0;
    for (int i = 0; i < width; ++i) v = (v << 1) | b[off + i];
    return v;
}
static void bits72_to_frame(const uint8_t *b, char out[13])
{
    for (int i = 0; i < 12; ++i) out[i] = ALPHA72[bv_get(b, i * 6, 6)];
    out[12] = 0;
}
static int frame_to_bits72(const char *f, uint8_t *b)
{
    for (int i = 0; i < 12; ++i) {
        int v = idx_of(ALPHA72, f[i]);
        if (v < 0) return -1;
        for (int k = 0; k < 6; ++k) b[i * 6 + k] = (v >> (5 - k)) & 1;
    }
    return 0;
}
static void finish_frame(bitvec_t *v, char out[13])
{
    /* pad: first a 0, then 1s, to 72 bits */
    int pad = 72 - v->n;
    for (int i = 0; i < pad; ++i) v->b[v->n++] = i == 0 ? 0 : 1;
    bits72_to_frame(v->b, out);
}

/* ------------------------------------------------------------------ */
/* CRC-16/KERMIT and checksum16                                         */
/* ------------------------------------------------------------------ */

uint16_t js8v_crc16_kermit(const char *s, size_t n)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc ^= (uint8_t)s[i];
        for (int k = 0; k < 8; ++k) crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
    }
    return crc;
}

static void pack16bits(uint16_t v, char out[4])
{
    unsigned a = v / (41 * 41), b = (v - a * 41 * 41) / 41, c = v % 41;
    out[0] = ALPHA41[a % 41]; out[1] = ALPHA41[b]; out[2] = ALPHA41[c]; out[3] = 0;
}

void js8v_checksum16(const char *s, char out[4])
{
    pack16bits(js8v_crc16_kermit(s, strlen(s)), out);
}

/* ------------------------------------------------------------------ */
/* Alphanumeric / callsign / grid packing                               */
/* ------------------------------------------------------------------ */

static uint64_t pack_an50(const char *value)
{
    char w[32];
    int n = 0;
    for (const char *p = value; *p && n < 24; ++p)
        if ((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == ' ' || *p == '/' || *p == '@')
            w[n++] = *p;
    w[n] = 0;
    if (n > 3 && w[3] != '/') { memmove(w + 4, w + 3, n - 3 + 1); w[3] = ' '; ++n; }
    if (n > 7 && w[7] != '/') { memmove(w + 8, w + 7, n - 7 + 1); w[7] = ' '; ++n; }
    while (n < 11) w[n++] = ' ';
    w[n] = 0;
#define AI(c) ((uint64_t)idx_of(ALNUM, (c)))
    uint64_t v = AI(w[0]);
    v = v * 38 + AI(w[1]);
    v = v * 38 + AI(w[2]);
    v = v * 2 + (w[3] == '/');
    v = v * 38 + AI(w[4]);
    v = v * 38 + AI(w[5]);
    v = v * 38 + AI(w[6]);
    v = v * 2 + (w[7] == '/');
    v = v * 38 + AI(w[8]);
    v = v * 38 + AI(w[9]);
    v = v * 38 + AI(w[10]);
#undef AI
    return v;
}

static void unpack_an50(uint64_t v, char *out)
{
    char w[12];
    w[10] = ALNUM[v % 38]; v /= 38;
    w[9] = ALNUM[v % 38]; v /= 38;
    w[8] = ALNUM[v % 38]; v /= 38;
    w[7] = (v % 2) ? '/' : ' '; v /= 2;
    w[6] = ALNUM[v % 38]; v /= 38;
    w[5] = ALNUM[v % 38]; v /= 38;
    w[4] = ALNUM[v % 38]; v /= 38;
    w[3] = (v % 2) ? '/' : ' '; v /= 2;
    w[2] = ALNUM[v % 38]; v /= 38;
    w[1] = ALNUM[v % 38]; v /= 38;
    w[0] = ALNUM[v % 39];
    int n = 0;
    for (int i = 0; i < 11; ++i) if (w[i] != ' ') out[n++] = w[i];
    out[n] = 0;
}

/* ([0-9A-Z ])([0-9A-Z])([0-9])([A-Z ])([A-Z ])([A-Z ]) on a 6-char string */
static int callsign6_ok(const char *s)
{
    if (strlen(s) != 6) return 0;
    if (!(isdigit((unsigned char)s[0]) || isupper((unsigned char)s[0]) || s[0] == ' ')) return 0;
    if (!(isdigit((unsigned char)s[1]) || isupper((unsigned char)s[1]))) return 0;
    if (!isdigit((unsigned char)s[2])) return 0;
    for (int i = 3; i < 6; ++i) if (!(isupper((unsigned char)s[i]) || s[i] == ' ')) return 0;
    return 1;
}

uint32_t js8v_pack_callsign(const char *value, int *portable)
{
    char c[32];
    size_t n = 0;
    const char *p = lstrip(value);
    while (*p && n < sizeof c - 1) c[n++] = (char)toupper((unsigned char)*p++);
    while (n && is_space(c[n - 1])) --n;
    c[n] = 0;
    int bi = basecall_index(c);
    if (bi > 0) return NBASECALL + bi;
    if (n >= 2 && !strcmp(c + n - 2, "/P")) { c[n -= 2] = 0; if (portable) *portable = 1; }
    if (starts_with(c, "3DA0")) { char t[32]; snprintf(t, sizeof t, "3D0%s", c + 4); strcpy(c, t); }
    if (starts_with(c, "3X") && c[2] >= 'A' && c[2] <= 'Z') { char t[32]; snprintf(t, sizeof t, "Q%s", c + 2); strcpy(c, t); }
    int slen = (int)strlen(c);
    if (slen < 2 || slen > 6) return 0;
    char perms[3][8];
    int np = 0;
    strcpy(perms[np++], c);
    if (slen == 2) { snprintf(perms[np++], 8, " %s   ", c); }
    if (slen == 3) { snprintf(perms[np++], 8, " %s  ", c); snprintf(perms[np++], 8, "%s   ", c); }
    if (slen == 4) { snprintf(perms[np++], 8, " %s ", c); snprintf(perms[np++], 8, "%s  ", c); }
    if (slen == 5) { snprintf(perms[np++], 8, " %s", c); snprintf(perms[np++], 8, "%s ", c); }
    const char *m = NULL;
    for (int i = 0; i < np; ++i) if (callsign6_ok(perms[i])) m = perms[i];
    if (!m) return 0;
    uint32_t v = idx_of(ALNUM, m[0]);
    v = 36 * v + idx_of(ALNUM, m[1]);
    v = 10 * v + idx_of(ALNUM, m[2]);
    v = 27 * v + idx_of(ALNUM, m[3]) - 10;
    v = 27 * v + idx_of(ALNUM, m[4]) - 10;
    v = 27 * v + idx_of(ALNUM, m[5]) - 10;
    return v;
}

void js8v_unpack_callsign(uint32_t v, int portable, char *out)
{
    if (v > NBASECALL && v - NBASECALL < (uint32_t)NBASECALLS) { strcpy(out, k_basecalls[v - NBASECALL]); return; }
    char w[16];
    w[5] = ALNUM[v % 27 + 10]; v /= 27;
    w[4] = ALNUM[v % 27 + 10]; v /= 27;
    w[3] = ALNUM[v % 27 + 10]; v /= 27;
    w[2] = ALNUM[v % 10]; v /= 10;
    w[1] = ALNUM[v % 36]; v /= 36;
    w[0] = ALNUM[v % 39];
    w[6] = 0;
    char t[24];
    if (starts_with(w, "3D0")) snprintf(t, sizeof t, "3DA0%s", w + 3);
    else if (w[0] == 'Q' && w[1] >= 'A' && w[1] <= 'Z') snprintf(t, sizeof t, "3X%s", w + 1);
    else strcpy(t, w);
    /* trim */
    const char *s = lstrip(t);
    size_t n = strlen(s);
    while (n && is_space(s[n - 1])) --n;
    memcpy(out, s, n);
    out[n] = 0;
    if (portable) strcat(out, "/P");
}

static void deg2grid(float dlong, float dlat, char *out)
{
    if (dlong < -180) dlong += 360;
    if (dlong > 180) dlong -= 360;
    int nlong = (int)(60.0 * (180.0 - dlong) / 5);
    int n1 = nlong / 240, n2 = (nlong - 240 * n1) / 24, n3 = nlong - 240 * n1 - 24 * n2;
    out[0] = 'A' + n1; out[2] = '0' + n2; out[4] = 'a' + n3;
    int nlat = (int)(60.0 * (dlat + 90) / 2.5);
    n1 = nlat / 240; n2 = (nlat - 240 * n1) / 24; n3 = nlat - 240 * n1 - 24 * n2;
    out[1] = 'A' + n1; out[3] = '0' + n2; out[5] = 'a' + n3;
    out[6] = 0;
}

static void grid2deg(const char *grid, float *dlong, float *dlat)
{
    char g[7];
    memcpy(g, grid, 4);
    g[4] = 'm'; g[5] = 'm'; g[6] = 0;
    for (int i = 0; i < 4; ++i) g[i] = (char)toupper((unsigned char)g[i]);
    int nlong = 180 - 20 * (g[0] - 'A');
    int n20d = 2 * (g[2] - '0');
    float xminlong = 5 * (g[4] - 'a' + 0.5f);
    *dlong = nlong - n20d - xminlong / 60.0f;
    int nlat = -90 + 10 * (g[1] - 'A') + g[3] - '0';
    float xminlat = 2.5f * (g[5] - 'a' + 0.5f);
    *dlat = nlat + xminlat / 60.0f;
}

uint16_t js8v_pack_grid(const char *value)
{
    const char *s = lstrip(value);
    if (strlen(s) < 4) return NMAXGRID;
    float lo, la;
    grid2deg(s, &lo, &la);
    int ilong = (int)lo, ilat = (int)(la + 90);
    return (uint16_t)(((ilong + 180) / 2) * 180 + ilat);
}

void js8v_unpack_grid(uint16_t v, char *out)
{
    if (v > NBASEGRID) { out[0] = 0; return; }
    float dlat = v % 180 - 90;
    float dlong = v / 180 * 2 - 180 + 2;
    char g[8];
    deg2grid(dlong, dlat, g);
    memcpy(out, g, 4);
    out[4] = 0;
}

static int pack_num(const char *num, int *ok)
{
    if (!num || !*num) { if (ok) *ok = 0; return 0; }
    char *end;
    long v = strtol(num, &end, 10);
    int good = end != num && *end == 0;
    if (ok) *ok = good;
    if (!good) v = 0;
    if (v > 31) v = 31;
    if (v < -30) v = -30;
    return (int)v + 30 + 1;
}

static int pack_cmd(int cmd, int num)
{
    const char *k = cmd_key(cmd);
    if (cmd_is_snr(k)) return ((((1 << 1) | (strcmp(k, " HEARTBEAT SNR") == 0)) << 6) + (num & 63)) & 0xFF;
    return cmd & 0x7F;
}
static int unpack_cmd(int value, int *num)
{
    if (value & 0x80) {
        *num = value & 63;
        return (value & 0x40) ? 29 : 25;
    }
    *num = 0;
    return value & 0x7F;
}

static void format_snr(int snr, char *out)
{
    if (snr < -60 || snr > 60) { out[0] = 0; return; }
    if (snr >= 0) sprintf(out, "+%02d", snr);
    else sprintf(out, "-%02d", -snr);
}

/* ------------------------------------------------------------------ */
/* Callsign validation                                                  */
/* ------------------------------------------------------------------ */

static int has_digit_letter_pair(const char *s)
{
    for (; s[0] && s[1]; ++s)
        if ((isdigit((unsigned char)s[0]) && isupper((unsigned char)s[1])) ||
            (isupper((unsigned char)s[0]) && isdigit((unsigned char)s[1]))) return 1;
    return 0;
}

/* full match of ([0-9A-Z])?([0-9A-Z])([0-9])([A-Z]){0,3}(/P)? */
static int base_call_full(const char *c)
{
    char t[32];
    snprintf(t, sizeof t, "%s", c);
    size_t n = strlen(t);
    if (n >= 2 && !strcmp(t + n - 2, "/P")) t[n -= 2] = 0;
    for (int d = 1; d <= 2; ++d) {
        if ((int)n <= d) continue;
        int ok = isdigit((unsigned char)t[d]);
        for (int i = 0; i < d && ok; ++i) ok = isdigit((unsigned char)t[i]) || isupper((unsigned char)t[i]);
        int suf = (int)n - d - 1;
        if (suf > 3) ok = 0;
        for (int i = d + 1; i < (int)n && ok; ++i) ok = isupper((unsigned char)t[i]) != 0;
        if (ok) return 1;
    }
    return 0;
}

static int compound_pattern_full(const char *c)
{
    const char *p = c;
    if (*p == '@') ++p;
    size_t n = strlen(p);
    if (n < 1 || n > 11) return 0;
    for (size_t i = 0; i < n; ++i) {
        char ch = p[i];
        if (!(isupper((unsigned char)ch) || isdigit((unsigned char)ch) || ch == '/' || (i == 0 && ch == '@'))) return 0;
    }
    return is_word(p[n - 1]);
}

static int valid_compound(const char *c)
{
    int slashes = 0;
    for (const char *p = c; *p; ++p) slashes += *p == '/';
    if ((int)strlen(c) - slashes > 9) return 0;
    const char *sl = strchr(c, '/');
    if (sl) {
        char pre[32];
        size_t k = (size_t)(sl - c);
        if (k >= sizeof pre) k = sizeof pre - 1;
        memcpy(pre, c, k);
        pre[k] = 0;
        return basecall_index(pre) < 0;
    }
    if (c[0] == '@') return 1;
    return strlen(c) > 2 && has_digit_letter_pair(c);
}

int js8v_is_valid_callsign(const char *c, int *is_compound)
{
    if (is_compound) *is_compound = 0;
    if (basecall_index(c) > 0) return 1;
    if (base_call_full(c)) return strlen(c) > 2 && has_digit_letter_pair(c);
    if (compound_pattern_full(c)) {
        int v = valid_compound(c);
        if (is_compound) *is_compound = v;
        return v;
    }
    return 0;
}

static int is_compound_call(const char *c)
{
    if (basecall_index(c) > 0 && c[0] != '@') return 0;
    if (base_call_full(c)) return 0;
    if (!compound_pattern_full(c)) return 0;
    return valid_compound(c);
}

/* ------------------------------------------------------------------ */
/* JSC dictionary                                                       */
/* ------------------------------------------------------------------ */

typedef struct { uint8_t ch, pad; uint16_t pad2; uint32_t size, index; } jsc_prefix_t;

static struct {
    void *mem; size_t len;
    uint32_t nmap, nlist, nprefix;
    const uint32_t *off; const uint8_t *size; const uint32_t *list;
    const jsc_prefix_t *prefix; const char *blob;
} J;

int js8v_load_jsc(const char *path)
{
    if (J.mem) return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return -1; }
    void *m = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return -1;
    const uint8_t *p = m;
    if (memcmp(p, "NQJSC01", 8)) { munmap(m, st.st_size); return -1; }
    memcpy(&J.nmap, p + 8, 4); memcpy(&J.nlist, p + 12, 4); memcpy(&J.nprefix, p + 16, 4);
    size_t o = 20;
    J.off = (const uint32_t *)(p + o); o += 4 * (J.nmap + 1);
    J.size = p + o; o += J.nmap;
    o = (o + 3) & ~(size_t)3; /* list is u32; file layout keeps it unaligned -> copy below */
    J.mem = m; J.len = st.st_size;
    /* the generator packs without alignment; recompute exact offsets */
    o = 20 + 4 * (J.nmap + 1) + J.nmap;
    static uint32_t *list_copy;
    list_copy = malloc(4 * J.nlist);
    memcpy(list_copy, p + o, 4 * J.nlist);
    J.list = list_copy;
    o += 4 * J.nlist;
    static jsc_prefix_t *prefix_copy;
    prefix_copy = malloc(sizeof(jsc_prefix_t) * J.nprefix);
    memcpy(prefix_copy, p + o, sizeof(jsc_prefix_t) * J.nprefix);
    J.prefix = prefix_copy;
    o += sizeof(jsc_prefix_t) * J.nprefix;
    J.blob = (const char *)(p + o);
    /* offsets table starts at 20, which is 4-aligned */
    return 0;
}

void js8v_unload_jsc(void)
{
    if (!J.mem) return;
    munmap(J.mem, J.len);
    free((void *)J.list);
    free((void *)J.prefix);
    memset(&J, 0, sizeof J);
}

static int jsc_lookup(const char *b, uint32_t *index)
{
    for (uint32_t i = 0; i < J.nprefix; ++i) {
        if ((uint8_t)b[0] != J.prefix[i].ch) continue;
        if (J.prefix[i].size == 1) { *index = J.list[J.prefix[i].index]; return 1; }
        uint32_t s = J.prefix[i].index, e = s + J.prefix[i].size;
        for (uint32_t k = s; k < e; ++k) {
            uint32_t mi = J.list[k];
            uint32_t len = J.size[mi];
            if (strncmp(b, J.blob + J.off[mi], len) == 0) { *index = mi; return 1; }
        }
        return 0;
    }
    return 0;
}

static void jsc_codeword(uint32_t index, int separate, bitvec_t *out)
{
    const uint32_t s = 7, c = 9;
    uint32_t parts[16];
    int np = 0;
    uint32_t x = index / s;
    while (x > 0) { x -= 1; parts[np++] = (x % c) + s; x /= c; }
    out->n = 0;
    for (int i = np - 1; i >= 0; --i) bv_push(out, parts[i], 4);
    bv_push(out, ((index % s) << 1) + (uint32_t)separate, 5);
}

/* Emits (codeword, nchars) pairs for text; returns count. */
typedef struct { bitvec_t bits; int chars; } cwpair_t;

static int jsc_compress(const char *text, cwpair_t *out, int max)
{
    int n = 0;
    const char *p = text;
    for (;;) {
        const char *sp = strchr(p, ' ');
        size_t wl = sp ? (size_t)(sp - p) : strlen(p);
        int is_last_word = sp == NULL;
        char w[LINE_MAX_CH];
        int is_space_char = 0;
        if (wl == 0 && !is_last_word) { strcpy(w, " "); is_space_char = 1; }
        else { memcpy(w, p, wl); w[wl] = 0; }
        char *cur = w;
        while (*cur && n < max) {
            uint32_t idx;
            if (!jsc_lookup(cur, &idx)) break;
            size_t tsz = J.size[idx];
            size_t rem = strlen(cur);
            cur += tsz < rem ? tsz : rem;
            int is_last = *cur == 0;
            int app = is_last && !is_space_char && !is_last_word;
            jsc_codeword(idx, app, &out[n].bits);
            out[n].chars = (int)tsz + app;
            ++n;
        }
        if (is_last_word) break;
        p = sp + 1;
    }
    return n;
}

static void jsc_decompress(const uint8_t *bits, int count, char *out, size_t outsz)
{
    const uint32_t s = 7, c = 9;
    uint32_t base[8];
    base[0] = 0;
    base[1] = s;
    for (int k = 2; k < 8; ++k) base[k] = base[k - 1] + s * (uint32_t)pow(c, k - 1);
    uint32_t bytes[64];
    int nb = 0, seps[64], ns = 0, si = 0;
    int i = 0;
    while (i < count && nb < 64) {
        if (count - i < 4) break;
        uint32_t byte = (uint32_t)bv_get(bits, i, 4);
        bytes[nb++] = byte;
        i += 4;
        if (byte < s) {
            if (count - i > 0 && bits[i]) seps[ns++] = nb - 1;
            i += 1;
        }
    }
    size_t o = 0;
    out[0] = 0;
    uint32_t start = 0;
    while (start < (uint32_t)nb) {
        uint32_t k = 0, j = 0;
        while (start + k < (uint32_t)nb && bytes[start + k] >= s) { j = j * c + (bytes[start + k] - s); ++k; }
        if (j >= J.nmap) break;
        if (start + k >= (uint32_t)nb) break;
        j = j * s + bytes[start + k] + base[k];
        if (j >= J.nmap) break;
        uint32_t len = J.off[j + 1] - J.off[j];
        if (o + len + 2 < outsz) { memcpy(out + o, J.blob + J.off[j], len); o += len; }
        if (si < ns && seps[si] == (int)(start + k)) { if (o + 2 < outsz) out[o++] = ' '; ++si; }
        out[o] = 0;
        start += k + 1;
    }
}

/* ------------------------------------------------------------------ */
/* Huffman                                                              */
/* ------------------------------------------------------------------ */

static int huff_index(char c)
{
    for (int i = 0; i < NHUFF; ++i) if (k_huff[i].ch == c) return i;
    return -1;
}

static void huff_decode(const uint8_t *bits, int n, char *out, size_t outsz)
{
    size_t o = 0;
    int pos = 0;
    for (;;) {
        int found = 0;
        for (int i = 0; i < NHUFF; ++i) {
            const char *code = k_huff[i].code;
            int L = (int)strlen(code), ok = pos + L <= n;
            for (int k = 0; ok && k < L; ++k) ok = bits[pos + k] == (code[k] == '1');
            if (ok) {
                if (o + 1 < outsz) out[o++] = k_huff[i].ch;
                pos += L;
                found = 1;
                break;
            }
        }
        if (!found) break;
    }
    out[o] = 0;
}

/* ------------------------------------------------------------------ */
/* Frame packers                                                        */
/* ------------------------------------------------------------------ */

static int pack_compound_frame(const char *call, int type, uint16_t num, int bits3, char out[13])
{
    if (type == JS8_FRAME_DATA || type == JS8_FRAME_DIRECTED) return 0;
    uint64_t pc = pack_an50(call);
    if (pc == 0) return 0;
    bitvec_t v = {.n = 0};
    bv_push(&v, type, 3);
    bv_push(&v, pc, 50);
    bv_push(&v, (num >> 5) & 0x7FF, 11);
    bv_push(&v, (uint64_t)(((num & 31) << 3) | (bits3 & 7)), 8);
    bits72_to_frame(v.b, out);
    return 1;
}

/* heartbeat_re */
static int parse_heartbeat(const char *t, int *cq_index, int *is_alt, char *grid)
{
    static const char *types[] = {"CQ CQ CQ", "CQ DX", "CQ QRP", "CQ CONTEST", "CQ FIELD",
                                  "CQ FD", "CQ CQ", "CQ", "HB", "HEARTBEAT"};
    const char *p = lstrip(t);
    const char *q = NULL;
    if (starts_with(p, "@ALLCALL")) q = p + 8;
    else if (starts_with(p, "@HB")) q = p + 3;
    if (q && is_space(*q)) { while (is_space(*q)) ++q; p = q; }
    for (int k = 0; k < 10; ++k) {
        size_t L = strlen(types[k]);
        if (strncmp(p, types[k], L)) continue;
        const char *e = p + L;
        if (k == 9) {
            const char *s = e;
            if (is_space(*s)) { while (is_space(*s)) ++s; if (starts_with(s, "SNR")) continue; }
        }
        grid[0] = 0;
        if (is_space(e[0]) && e[1] >= 'A' && e[1] <= 'R' && e[2] >= 'A' && e[2] <= 'R' &&
            isdigit((unsigned char)e[3]) && isdigit((unsigned char)e[4]) && !is_word(e[5])) {
            memcpy(grid, e + 1, 4); grid[4] = 0;
            e += 5;
        } else if (is_word(*e)) continue;
        *is_alt = types[k][0] == 'C';
        *cq_index = 0;
        if (*is_alt) for (int c = 0; c < 8; ++c) if (!strcmp(k_cqs[c], types[k])) { *cq_index = c; break; }
        return (int)(e - t);
    }
    return 0;
}

static int pack_heartbeat(const char *text, const char *mycall, char out[13])
{
    int cq = 0, alt = 0;
    char grid[8];
    int n = parse_heartbeat(text, &cq, &alt, grid);
    if (!n || !mycall || !*mycall) return 0;
    uint16_t extra = NMAXGRID;
    if (strlen(grid) == 4) extra = js8v_pack_grid(grid);
    if (alt) extra |= 1u << 15;
    if (!pack_compound_frame(mycall, JS8_FRAME_HEARTBEAT, extra, alt ? cq : 0, out)) return 0;
    return n;
}

/* optional_cmd_pattern at p; returns length matched (0 = none) */
static int match_cmd(const char *p)
{
    static const char *lit[] = {"AGN?", "QSL?", "HW CPY?", "MSG TO:", "SNR?", "INFO?",
                                "GRID?", "STATUS?", "QUERY MSGS?", "HEARING?"};
    static const char *words[] = {"STATUS", "HEARING", "QUERY CALL", "QUERY MSGS", "QUERY", "CMD",
                                  "MSG", "NACK", "ACK", "73", "YES", "NO", "HEARTBEAT SNR", "SNR",
                                  "QSL", "RR", "SK", "FB", "INFO", "GRID", "DIT DIT"};
    for (int sp = 1; sp >= 0; --sp) {
        const char *q = p;
        if (sp) { if (!is_space(*q)) continue; ++q; }
        for (size_t i = 0; i < sizeof lit / sizeof *lit; ++i)
            if (starts_with(q, lit[i])) return (int)(q - p + strlen(lit[i]));
        for (size_t i = 0; i < sizeof words / sizeof *words; ++i) {
            size_t L = strlen(words[i]);
            if (!strncmp(q, words[i], L) && (q[L] == ' ' || q[L] == 0)) return (int)(q - p + L);
        }
        if (*q == '?' || *q == '>' || *q == ' ') return (int)(q - p + 1);
    }
    return 0;
}

/* optional_num_pattern at p (lookbehind SNR checked against base) */
static int match_num(const char *base, const char *p)
{
    if (p - base < 3 || strncmp(p - 3, "SNR", 3)) return 0;
    const char *q = p;
    if (is_space(*q)) ++q;
    if (*q == '-' || *q == '+') ++q;
    if (q[0] == '3' && (q[1] == '0' || q[1] == '1')) q += 2;
    else if (q[0] >= '0' && q[0] <= '2' && isdigit((unsigned char)q[1])) q += 2;
    else if (isdigit((unsigned char)q[0])) q += 1;
    else return 0;
    return (int)(q - p);
}

static int match_callsign(const char *p)
{
    const char *q = p;
    if (*q == '@') ++q;
    const char *s = q;
    while (isupper((unsigned char)*q) || isdigit((unsigned char)*q) || *q == '/') ++q;
    return q == s ? 0 : (int)(q - p);
}

static void copy_n(char *dst, size_t dsz, const char *src, size_t n)
{
    if (n >= dsz) n = dsz - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void trim_copy(char *dst, size_t dsz, const char *src)
{
    const char *s = lstrip(src);
    size_t n = strlen(s);
    while (n && is_space(s[n - 1])) --n;
    copy_n(dst, dsz, s, n);
}

static int pack_compound(const char *text, char out[13])
{
    const char *p = lstrip(text);
    if (*p != '`') return 0;
    ++p;
    int cl = match_callsign(p);
    if (!cl) return 0;
    char call[32], grid[8] = "", cmd[24] = "", num[8] = "";
    copy_n(call, sizeof call, p, cl);
    const char *q = p + cl;
    for (int sp = 1; sp >= 0; --sp) {
        const char *g = q;
        if (sp) { if (!is_space(*g)) continue; ++g; }
        if (g[0] >= 'A' && g[0] <= 'R' && g[1] >= 'A' && g[1] <= 'R' && isdigit((unsigned char)g[2]) && isdigit((unsigned char)g[3])) {
            copy_n(grid, sizeof grid, g, 4);
            q = g + 4;
            break;
        }
    }
    int ml = match_cmd(q);
    if (ml) { copy_n(cmd, sizeof cmd, q, ml); q += ml; }
    int nl = match_num(text, q);
    if (nl) { char tmp[8]; copy_n(tmp, sizeof tmp, q, nl); trim_copy(num, sizeof num, tmp); q += nl; }

    int type = JS8_FRAME_COMPOUND;
    uint16_t extra = NMAXGRID;
    if (*cmd && cmd_allowed(cmd)) {
        int inum = pack_num(num, NULL);
        extra = NUSERGRID + pack_cmd(k_cmds[cmd_lookup(cmd)].val, inum);
        type = JS8_FRAME_COMPOUND_DIRECTED;
    } else if (*grid) {
        extra = js8v_pack_grid(grid);
    }
    if (!pack_compound_frame(call, type, extra, 0, out)) return 0;
    return (int)(q - text);
}

static int pack_directed(const char *text, const char *mycall, char *to_out, int *to_compound,
                         char *cmd_out, char *num_out, char out[13])
{
    const char *p = text;
    int cl = match_callsign(p);
    if (!cl) return 0;
    char to[32], cmd[24] = "", num[8] = "";
    copy_n(to, sizeof to, p, cl);
    const char *q = p + cl;
    int ml = match_cmd(q);
    if (ml) { copy_n(cmd, sizeof cmd, q, ml); q += ml; }
    int nl = match_num(text, q);
    if (nl) { copy_n(num, sizeof num, q, nl); q += nl; }
    if (!*cmd) return 0;

    const char *from = mycall;
    if (is_compound_call(mycall)) from = "<....>";
    int isc = 0;
    if (!strcmp(to, mycall) || !js8v_is_valid_callsign(to, &isc)) return 0;
    strcpy(to_out, to);
    *to_compound = isc;
    char to_pack[32];
    strcpy(to_pack, isc ? "<....>" : to);

    char tcmd[24];
    trim_copy(tcmd, sizeof tcmd, cmd);
    if (!cmd_allowed(cmd) && !cmd_allowed(tcmd)) return 0;
    char tnum[8];
    trim_copy(tnum, sizeof tnum, num);
    int ok = 0, inum = pack_num(tnum, &ok);
    if (ok) strcpy(num_out, num);

    int pf = 0, pt = 0;
    uint32_t packed_from = js8v_pack_callsign(from, &pf);
    uint32_t packed_to = js8v_pack_callsign(to_pack, &pt);
    if (!packed_from || !packed_to) return 0;
    const char *cmdout = "";
    int packed_cmd = 0;
    if (cmd_lookup(cmd) >= 0) { cmdout = k_cmds[cmd_lookup(cmd)].key; packed_cmd = k_cmds[cmd_lookup(cmd)].val; }
    if (cmd_lookup(tcmd) >= 0) { cmdout = k_cmds[cmd_lookup(tcmd)].key; packed_cmd = k_cmds[cmd_lookup(tcmd)].val; }
    strcpy(cmd_out, cmdout);

    bitvec_t v = {.n = 0};
    bv_push(&v, JS8_FRAME_DIRECTED, 3);
    bv_push(&v, packed_from, 28);
    bv_push(&v, packed_to, 28);
    bv_push(&v, (uint64_t)(packed_cmd % 32), 5);
    bv_push(&v, (uint64_t)(((pf << 7) + (pt << 6) + inum) & 0xFF), 8);
    bits72_to_frame(v.b, out);
    return (int)(q - text);
}

static int pack_huff(const char *input, int prefix_bits, int nprefix, char out[13])
{
    for (const char *p = input; *p; ++p)
        if (huff_index((char)toupper((unsigned char)*p)) < 0) return 0;
    bitvec_t v = {.n = 0};
    if (nprefix) bv_push(&v, prefix_bits, nprefix);
    int used = 0;
    for (const char *p = input; *p; ++p) {
        int h = huff_index(*p);
        if (h < 0) continue;           /* matches JS8Call: unmapped chars skipped */
        int L = (int)strlen(k_huff[h].code);
        if (v.n + L >= 72) break;
        for (int k = 0; k < L; ++k) v.b[v.n++] = k_huff[h].code[k] == '1';
        ++used;
    }
    finish_frame(&v, out);
    return used;
}

static int pack_jsc(const char *input, int prefix_bits, int nprefix, char out[13])
{
    if (!J.mem) return 0;
    static cwpair_t pairs[256];
    int np = jsc_compress(input, pairs, 256);
    bitvec_t v = {.n = 0};
    if (nprefix) bv_push(&v, prefix_bits, nprefix);
    int used = 0;
    for (int i = 0; i < np; ++i) {
        if (v.n + pairs[i].bits.n >= 72) break;
        memcpy(v.b + v.n, pairs[i].bits.b, pairs[i].bits.n);
        v.n += pairs[i].bits.n;
        used += pairs[i].chars;
    }
    finish_frame(&v, out);
    return used;
}

static int pack_data(const char *input, char out[13])
{
    char hf[13] = "", cf[13] = "";
    int hn = pack_huff(input, 2, 2, hf);       /* {1,0} */
    int cn = pack_jsc(input, 3, 2, cf);        /* {1,1} */
    if (hn > cn) { memcpy(out, hf, 13); return hn; }
    memcpy(out, cf, 13);
    return cn;
}

static int pack_fast_data(const char *input, char out[13])
{
    return pack_jsc(input, 0, 0, out);
}

/* ------------------------------------------------------------------ */
/* buildMessageFrames                                                   */
/* ------------------------------------------------------------------ */

static int first_callsign(const char *line, char *out, size_t outsz)
{
    const char *p = line;
    while (*p) {
        while (*p && !(isupper((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '/' || *p == '@')) ++p;
        const char *s = p;
        while (*p && (isupper((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '/' || *p == '@')) ++p;
        if (p == s) break;
        char tok[32];
        copy_n(tok, sizeof tok, s, (size_t)(p - s));
        int isgrid = strlen(tok) >= 4 && tok[0] >= 'A' && tok[0] <= 'X' && tok[1] >= 'A' && tok[1] <= 'X' &&
                     isdigit((unsigned char)tok[2]) && isdigit((unsigned char)tok[3]);
        if (!isgrid && js8v_is_valid_callsign(tok, NULL)) { copy_n(out, outsz, tok, strlen(tok)); return 1; }
    }
    return 0;
}

int js8v_build_frames(const char *mycall, const char *mygrid, const char *selcall,
                      const char *text_in, int force_identify, int force_data,
                      js8_submode_t submode, js8_txframe_t *out, int max)
{
    char line[LINE_MAX_CH], tmp[LINE_MAX_CH];
    int nout = 0, has_directed = 0, has_data = 0;
    if (!selcall) selcall = "";
    if (!mygrid) mygrid = "";
    size_t L = 0;
    for (const char *p = text_in; *p && L < sizeof line - 1; ++p) line[L++] = (char)toupper((unsigned char)*p);
    line[L] = 0;
    int mycall_compound = is_compound_call(mycall);

    if (force_data) { force_identify = 0; has_data = 1; }
    size_t ml = strlen(mycall);
    if (!strncmp(line, mycall, ml) && (line[ml] == ':' || line[ml] == ' ')) {
        strcpy(tmp, lstrip(line + ml + 1));
        strcpy(line, tmp);
    }
    if (*selcall && !starts_with(line, selcall) && line[0] != '`' && !force_data) {
        int base = starts_with(line, "@ALLCALL") || starts_with(line, "CQ") || starts_with(line, "HB");
        char c0[32];
        int std = first_callsign(line, c0, sizeof c0) && starts_with(line, c0) && strlen(c0) > 3;
        if (!base && !std) {
            snprintf(tmp, sizeof tmp, "%s%s%s", selcall, line[0] == ' ' ? "" : " ", line);
            strcpy(line, tmp);
        }
    }

    while (line[0] && nout < max) {
        char bcn[13], cmp[13], dir[13], dat[13];
        int l = pack_heartbeat(line, mycall, bcn);
        int o = pack_compound(line, cmp);
        char dir_to[32] = "", dir_cmd[24] = "", dir_num[8] = "";
        int dir_to_compound = 0;
        int n = pack_directed(line, mycall, dir_to, &dir_to_compound, dir_cmd, dir_num, dir);

        int likely_data = nout == 0 && !*selcall && !*dir_to && l == 0 && o == 0;
        if (force_identify && likely_data && !strstr(line, mycall)) {
            snprintf(tmp, sizeof tmp, "%s: %s", mycall, line);
            strcpy(line, tmp);
        }
        int fast = submode != JS8_NORMAL;
        int m = fast ? pack_fast_data(line, dat) : pack_data(line, dat);

        if (!has_directed && !has_data && l > 0) {
            memcpy(out[nout].text, bcn, 13); out[nout++].bits = 0;
            memmove(line, line + l, strlen(line + l) + 1);
        } else if (!has_directed && !has_data && o > 0) {
            memcpy(out[nout].text, cmp, 13); out[nout++].bits = 0;
            memmove(line, line + o, strlen(line + o) + 1);
        } else if (!has_directed && !has_data && n > 0) {
            has_directed = 1;
            if (mycall_compound || dir_to_compound) {
                char msg[96], fr[13];
                snprintf(msg, sizeof msg, "`%s %s", mycall, mygrid);
                if (pack_compound(msg, fr) && nout < max) { memcpy(out[nout].text, fr, 13); out[nout++].bits = 0; }
                snprintf(msg, sizeof msg, "`%s%s%s", dir_to, dir_cmd, dir_num);
                if (pack_compound(msg, fr) && nout < max) { memcpy(out[nout].text, fr, 13); out[nout++].bits = 0; }
            } else {
                memcpy(out[nout].text, dir, 13); out[nout++].bits = 0;
            }
            memmove(line, line + n, strlen(line + n) + 1);
            if (cmd_buffered(dir_cmd) && line[0]) {
                strcpy(tmp, lstrip(line));
                strcpy(line, tmp);
                if (cmd_checksum(dir_cmd) == 16) {
                    char ck[4];
                    js8v_checksum16(line, ck);
                    size_t ll = strlen(line);
                    if (ll + 5 < sizeof line) { line[ll] = ' '; memcpy(line + ll + 1, ck, 4); }
                }
            }
        } else if (m > 0) {
            has_data = 1;
            memcpy(out[nout].text, dat, 13);
            out[nout++].bits = fast ? JS8_TX_DATA : 0;
            size_t ll = strlen(line);
            if ((size_t)m > ll) m = (int)ll;
            memmove(line, line + m, ll - m + 1);
        } else {
            break; /* nothing encodable left (JS8Call would spin here) */
        }
    }
    if (nout) { out[0].bits |= JS8_TX_FIRST; out[nout - 1].bits |= JS8_TX_LAST; }
    return nout;
}

/* ------------------------------------------------------------------ */
/* Receive side (DecodedText)                                           */
/* ------------------------------------------------------------------ */

int js8v_interpret(const char frame[12], int bits, js8_rxframe_t *r)
{
    uint8_t b[72];
    memset(r, 0, sizeof *r);
    r->kind = JS8_FRAME_UNKNOWN;
    if (frame_to_bits72(frame, b)) return 0;

    if (bits & JS8_TX_DATA) {
        if (!J.mem) return 0;
        int n = -1;
        for (int i = 71; i >= 0; --i) if (!b[i]) { n = i; break; }
        if (n <= 0) return 0;
        jsc_decompress(b, n, r->message, sizeof r->message);
        if (!r->message[0]) return 0;
        r->kind = JS8_FRAME_DATA;
        return 1;
    }
    if (b[0]) {
        /* data frame: [1][compressed][payload][0][1...] */
        int compressed = b[1];
        int n = -1;
        for (int i = 70; i >= 0; --i) if (!b[1 + i]) { n = i; break; }
        if (n > 1) {
            if (compressed) { if (J.mem) jsc_decompress(b + 2, n - 1, r->message, sizeof r->message); }
            else huff_decode(b + 2, n - 1, r->message, sizeof r->message);
        }
        if (!r->message[0]) return 0;
        r->kind = JS8_FRAME_DATA;
        return 1;
    }
    int flag = (int)bv_get(b, 0, 3);
    if (flag == JS8_FRAME_HEARTBEAT || flag == JS8_FRAME_COMPOUND || flag == JS8_FRAME_COMPOUND_DIRECTED) {
        char call[16];
        unpack_an50(bv_get(b, 3, 50), call);
        int p8 = (int)bv_get(b, 64, 8);
        uint16_t num = (uint16_t)((bv_get(b, 53, 11) << 5) | (p8 >> 3));
        int bits3 = p8 & 7;
        strcpy(r->from, call);
        if (flag == JS8_FRAME_HEARTBEAT) {
            js8v_unpack_grid(num & 0x7FFF, r->grid);
            r->is_heartbeat = 1;
            r->is_cq = (num >> 15) & 1;
            if (r->is_cq)
                snprintf(r->message, sizeof r->message, "%s: @ALLCALL %s %s ", call, k_cqs[bits3], r->grid);
            else
                snprintf(r->message, sizeof r->message, "%s: @HB HEARTBEAT %s ", call, r->grid);
            r->kind = JS8_FRAME_HEARTBEAT;
            return 1;
        }
        char extra[48] = "";
        if (num <= NBASEGRID) {
            js8v_unpack_grid(num, r->grid);
            snprintf(extra, sizeof extra, " %s", r->grid);
        } else if (num >= NUSERGRID && num < NMAXGRID) {
            int n2 = 0, c = unpack_cmd(num - NUSERGRID, &n2);
            const char *cs = cmd_key(c);
            snprintf(r->cmd, sizeof r->cmd, "%s", cs);
            if (cmd_is_snr(cs)) { format_snr(n2 - 31, r->num); snprintf(extra, sizeof extra, "%s %s", cs, r->num); }
            else snprintf(extra, sizeof extra, "%s", cs);
        }
        r->kind = (js8_frame_kind_t)flag;
        if (flag == JS8_FRAME_COMPOUND) snprintf(r->message, sizeof r->message, "%s: ", call);
        else { snprintf(r->message, sizeof r->message, "%s%s ", call, extra); strcpy(r->to, call); r->from[0] = 0; }
        return 1;
    }
    if (flag == JS8_FRAME_DIRECTED) {
        int extra = (int)bv_get(b, 64, 8);
        int pf = (extra >> 7) & 1, pt = (extra >> 6) & 1;
        extra %= 64;
        js8v_unpack_callsign((uint32_t)bv_get(b, 3, 28), pf, r->from);
        js8v_unpack_callsign((uint32_t)bv_get(b, 31, 28), pt, r->to);
        const char *cmd = cmd_key((int)bv_get(b, 59, 5) % 32);
        snprintf(r->cmd, sizeof r->cmd, "%s", cmd);
        if (extra) {
            if (cmd_is_snr(cmd)) format_snr(extra - 31, r->num);
            else snprintf(r->num, sizeof r->num, "%d", extra - 31);
            snprintf(r->message, sizeof r->message, "%s: %s%s %s ", r->from, r->to, cmd, r->num);
        } else {
            snprintf(r->message, sizeof r->message, "%s: %s%s ", r->from, r->to, cmd);
        }
        r->kind = JS8_FRAME_DIRECTED;
        return 1;
    }
    return 0;
}
