/* QSO-Zero - sound card levels (ALSA simple mixer, loaded at run time).
 * The device is the one QSO-Zero uses, e.g. "plughw:CARD=Device,DEV=0";
 * the mixer of that card ("hw:CARD=Device") is opened. Needs the same
 * "audio" group as the sound devices. */
#ifndef QSOZERO_MIXER_H
#define QSOZERO_MIXER_H

typedef struct {
    int  ok;            /* a level control was found */
    int  percent;       /* 0-100 */
    int  muted;         /* its on/off switch is off */
    char control[32];   /* e.g. "Mic", "Speaker" */
    char error[96];     /* why not, when !ok */
} mixer_level_t;

/* capture=1: recording level of the device (RX audio in);
 * capture=0: playback level (TX audio out). */
int mixer_get(const char *dev, int capture, mixer_level_t *out);
/* Set 0-100 % and switch the control on. Returns 0 on success. */
int mixer_set(const char *dev, int capture, int percent, mixer_level_t *out);

/* Exposed for tests: which control to use, by name. For recording, any
 * control with a capture volume ("Mic", "Capture", ...). For playback,
 * prefer "Speaker"/"PCM"/"Master"/"Headphone"/"Playback" and avoid "Mic"
 * and "Line" (those are monitor loopbacks on USB codecs). -1 = none. */
int mixer_pick(const char *const names[], const int has_vol[], int n, int capture);

#endif
