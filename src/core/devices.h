/* QSO-Zero - lists the serial ports and sound cards that are plugged in,
 * for the Setup pickers. `root` is "" on the device (tests use a fake tree). */
#ifndef QSOZERO_DEVICES_H
#define QSOZERO_DEVICES_H

#define DEV_MAX 24

typedef struct {
    char value[128];   /* what goes in the config ("" = automatic) */
    char label[40];    /* short name: /dev/ttyUSB0, QMX, ... */
    char desc[80];     /* what it is */
} dev_opt_t;

/* Serial ports: "auto" first, then /dev/serial/by-id links (value = the
 * by-id path so the choice survives ttyUSB renumbering; label = the
 * /dev/ttyUSB# / ttyACM# it points to), then any other ttyUSB / ttyACM. */
int dev_serial_list(const char *root, const char *auto_desc, dev_opt_t *o, int max);

/* Automatic CAT port: the /dev/serial/by-id entry matching one of the
 * '|'-separated hints (first interface, if00, preferred), else `def` if it
 * exists, else the first ttyUSB/ttyACM. Writes the path ("" if nothing). */
int dev_serial_auto(const char *root, const char *hints, const char *def, char *out, int outsz);

/* Sound devices of one direction (capture=1: things that record, for
 * Audio in; capture=0: things that play, for Audio out), one entry per
 * ALSA PCM device as plughw:CARD=<id>,DEV=<n>. "auto" first, then USB
 * devices, then built-in / HDMI. */
int dev_audio_list(const char *root, int capture, const char *auto_desc, dev_opt_t *o, int max);

/* Automatic choice: the best USB sound device of that direction (radio
 * codecs and the Digirig are USB). hints ('|'-separated) and prefer_card
 * (a card id, e.g. the input's, for the output) raise the score. Never the
 * built-in codec or HDMI. Returns 0 and fills out, or -1 if none. */
int dev_audio_auto(const char *root, const char *hints, int capture, const char *prefer_card,
                   char *out, int outsz);

/* Is this configured device (plughw:CARD=X[,DEV=n]) plugged in? */
int dev_audio_present(const char *root, const char *value, int capture);

/* Short display name for a config value: by-id path -> ttyUSB0,
 * plughw:CARD=QMX -> QMX, "" -> auto. */
void dev_short_name(const char *root, const char *value, char *out, int outsz);

#endif
