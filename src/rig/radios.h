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
    const char  *data_mode;     /* Hamlib mode for digital: USB or PKTUSB */
    float        max_watts;
    const char  *notes;
} radio_profile_t;

const radio_profile_t *radio_find(const char *id);
const radio_profile_t *radio_list(int *count);
const char *ptt_name(ptt_method_t p);

#endif
