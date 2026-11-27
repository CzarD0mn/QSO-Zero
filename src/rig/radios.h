/* QSO-Zero - radio profiles. Hamlib model IDs taken from Hamlib's riglist.h. */
#ifndef QSOZERO_RADIOS_H
#define QSOZERO_RADIOS_H

typedef enum { PTT_CAT = 0, PTT_RTS, PTT_DTR, PTT_VOX } ptt_method_t;

typedef struct {
    const char  *id;            /* config key */
    const char  *name;          /* display name */
    int          hamlib_model;
    int          hamlib_fallback;  /* for older Hamlib (e.g. Debian 4.5) */
    int          baud;
    ptt_method_t ptt;
    const char  *serial_hint;   /* substring to look for in /dev/serial/by-id */
    const char  *serial_default;
    const char  *audio_hint;    /* substrings in /proc/asound/cards, '|' separated */
    const char  *data_mode;     /* Hamlib mode for digital: PKTUSB (USB-D / DATA-U); USB is the fallback */
    float        max_watts;
    unsigned     bands;         /* bit i = band_table()[i] the radio can work */
    const char  *notes;
    float        tx_level;      /* TX audio level set when this radio is chosen (0 = leave it) */
    int          qmx_ssb;       /* QMX: digital modes run in its USB (SSB) mode with the 3.2 kHz RX filter */
} radio_profile_t;

const radio_profile_t *radio_find(const char *id);
const radio_profile_t *radio_list(int *count);
const char *ptt_name(ptt_method_t p);

/* band bits, in band_table() order: 160 80 60 40 30 20 17 15 12 10 6 */
#define BANDS_ALL      0x7FFu
#define BANDS_NO160    (BANDS_ALL & ~0x001u)
#define BANDS_NO6      (BANDS_ALL & ~0x400u)
#define BANDS_80_TO_10 (BANDS_ALL & ~0x401u)
/* bit i = band_table()[i]: 160 80 60 40 30 20 17 15 12 10 6 */
#define BANDS_QMX_LOW  0x03Eu   /* 80 60 40 30 20 */
#define BANDS_QMX_MID  0x0FCu   /* 60 40 30 20 17 15 */
#define BANDS_QMX_HIGH 0x3E0u   /* 20 17 15 12 10 (and 11 m, which has no digital-mode spot here) */

#endif
