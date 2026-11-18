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

/* ALSA cards with a capture (capture=1) or playback (capture=0) device.
 * "auto"/"same as input" first, then "default", then each card as
 * plughw:CARD=<id>. */
int dev_audio_list(const char *root, int capture, const char *auto_desc, dev_opt_t *o, int max);

/* Short display name for a config value: by-id path -> ttyUSB0,
 * plughw:CARD=QMX -> QMX, "" -> auto. */
void dev_short_name(const char *root, const char *value, char *out, int outsz);

#endif
