/*
 * QSO-Zero - JS8 message layer (port of JS8Call varicode.cpp / jsc.cpp /
 * decodedtext.cpp, GPLv3). Turns text into 12-char frames and back.
 */
#ifndef QSOZERO_JS8_VARICODE_H
#define QSOZERO_JS8_VARICODE_H

#include "js8.h"

/* Transmission flags carried in the modem's 3-bit frame type */
#define JS8_TX_FIRST 1
#define JS8_TX_LAST  2
#define JS8_TX_DATA  4   /* fast-data frame (non-Normal submodes), no header */

typedef enum {
    JS8_FRAME_HEARTBEAT = 0,
    JS8_FRAME_COMPOUND = 1,
    JS8_FRAME_COMPOUND_DIRECTED = 2,
    JS8_FRAME_DIRECTED = 3,
    JS8_FRAME_DATA = 4,
    JS8_FRAME_DATA_COMPRESSED = 6,
    JS8_FRAME_UNKNOWN = 255
} js8_frame_kind_t;

typedef struct {
    char text[JS8_FRAME_CHARS + 1];
    int  bits;            /* JS8_TX_* flags -> modem frame_type */
} js8_txframe_t;

typedef struct {
    js8_frame_kind_t kind;
    int  is_heartbeat, is_cq;
    char message[160];    /* human readable, as JS8Call shows it */
    char from[16], to[16], cmd[20], num[8], grid[8];
} js8_rxframe_t;

/* Load the JSC dictionary (data/js8.jsc). Needed for compressed free text.
 * Returns 0 on success. Without it, only Huffman data frames are used. */
int  js8v_load_jsc(const char *path);
void js8v_unload_jsc(void);

/* Split a line of text into frames the way JS8Call does. text is used as
 * typed (it is uppercased internally for Huffman frames). selcall may be NULL
 * or "" for no selected station. Returns number of frames (<= max). */
int js8v_build_frames(const char *mycall, const char *mygrid, const char *selcall,
                      const char *text, int force_identify, int force_data,
                      js8_submode_t submode, js8_txframe_t *out, int max);

/* Interpret one decoded frame. Returns 1 if understood. */
int js8v_interpret(const char frame[12], int bits, js8_rxframe_t *out);

/* Helpers exposed for tests / the UI */
uint32_t js8v_pack_callsign(const char *call, int *portable);
void     js8v_unpack_callsign(uint32_t v, int portable, char *out);
uint16_t js8v_pack_grid(const char *grid);
void     js8v_unpack_grid(uint16_t v, char *out);
int      js8v_is_valid_callsign(const char *call, int *is_compound);
uint16_t js8v_crc16_kermit(const char *s, size_t n);
void     js8v_checksum16(const char *s, char out[4]);

#endif
