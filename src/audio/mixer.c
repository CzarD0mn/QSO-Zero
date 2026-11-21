/* QSO-Zero - sound card levels through the ALSA simple mixer API. */
#define _GNU_SOURCE
#include "audio/mixer.h"

#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

typedef struct snd_mixer snd_mixer_t;
typedef struct snd_mixer_elem snd_mixer_elem_t;

static struct {
    void *h;
    const char *(*strerror)(int);
    int (*open)(snd_mixer_t **, int);
    int (*attach)(snd_mixer_t *, const char *);
    int (*reg)(snd_mixer_t *, void *, void **);
    int (*load)(snd_mixer_t *);
    int (*close)(snd_mixer_t *);
    snd_mixer_elem_t *(*first)(snd_mixer_t *);
    snd_mixer_elem_t *(*next)(snd_mixer_elem_t *);
    const char *(*name)(snd_mixer_elem_t *);
    int (*active)(snd_mixer_elem_t *);
    int (*has_cvol)(snd_mixer_elem_t *);
    int (*has_pvol)(snd_mixer_elem_t *);
    int (*crange)(snd_mixer_elem_t *, long *, long *);
    int (*prange)(snd_mixer_elem_t *, long *, long *);
    int (*cget)(snd_mixer_elem_t *, int, long *);
    int (*pget)(snd_mixer_elem_t *, int, long *);
    int (*cset)(snd_mixer_elem_t *, long);
    int (*pset)(snd_mixer_elem_t *, long);
    int (*has_csw)(snd_mixer_elem_t *);
    int (*has_psw)(snd_mixer_elem_t *);
    int (*csw_get)(snd_mixer_elem_t *, int, int *);
    int (*psw_get)(snd_mixer_elem_t *, int, int *);
    int (*csw_set)(snd_mixer_elem_t *, int);
    int (*psw_set)(snd_mixer_elem_t *, int);
} M;

static int load(char *err, int esz)
{
    if (M.h) return 0;
    void *h = dlopen("libasound.so.2", RTLD_NOW);
    if (!h) { snprintf(err, esz, "libasound.so.2 not found"); return -1; }
#define S(f, n) if (!(*(void **)&M.f = dlsym(h, n))) { snprintf(err, esz, "missing %s", n); return -1; }
    S(strerror, "snd_strerror");
    S(open, "snd_mixer_open"); S(attach, "snd_mixer_attach"); S(reg, "snd_mixer_selem_register");
    S(load, "snd_mixer_load"); S(close, "snd_mixer_close");
    S(first, "snd_mixer_first_elem"); S(next, "snd_mixer_elem_next");
    S(name, "snd_mixer_selem_get_name"); S(active, "snd_mixer_selem_is_active");
    S(has_cvol, "snd_mixer_selem_has_capture_volume"); S(has_pvol, "snd_mixer_selem_has_playback_volume");
    S(crange, "snd_mixer_selem_get_capture_volume_range"); S(prange, "snd_mixer_selem_get_playback_volume_range");
    S(cget, "snd_mixer_selem_get_capture_volume"); S(pget, "snd_mixer_selem_get_playback_volume");
    S(cset, "snd_mixer_selem_set_capture_volume_all"); S(pset, "snd_mixer_selem_set_playback_volume_all");
    S(has_csw, "snd_mixer_selem_has_capture_switch"); S(has_psw, "snd_mixer_selem_has_playback_switch");
    S(csw_get, "snd_mixer_selem_get_capture_switch"); S(psw_get, "snd_mixer_selem_get_playback_switch");
    S(csw_set, "snd_mixer_selem_set_capture_switch_all"); S(psw_set, "snd_mixer_selem_set_playback_switch_all");
#undef S
    M.h = h;
    return 0;
}

int mixer_pick(const char *const names[], const int has_vol[], int n, int capture)
{
    static const char *cap_pref[] = {"Mic", "Capture", "Line", "ADC", "Digital", NULL};
    static const char *play_pref[] = {"Speaker", "PCM", "Master", "Headphone", "Playback", "Line Out", "DAC", "Digital", NULL};
    const char *const *pref = capture ? cap_pref : play_pref;
    for (int p = 0; pref[p]; ++p)
        for (int i = 0; i < n; ++i)
            if (has_vol[i] && !strncasecmp(names[i], pref[p], strlen(pref[p]))) return i;
    for (int i = 0; i < n; ++i) {
        if (!has_vol[i]) continue;
        if (!capture && (strcasestr(names[i], "mic") || !strncasecmp(names[i], "line", 4) || strcasestr(names[i], "capture"))) continue;
        return i;
    }
    return -1;
}

/* open the card's mixer and find the control; returns the element or NULL */
static snd_mixer_elem_t *find(const char *dev, int capture, snd_mixer_t **mx, mixer_level_t *o)
{
    memset(o, 0, sizeof *o);
    *mx = NULL;
    if (!dev || !dev[0]) { snprintf(o->error, sizeof o->error, "no device"); return NULL; }
    if (load(o->error, sizeof o->error)) return NULL;
    char card[64];
    const char *c = strstr(dev, "CARD=");
    if (c) snprintf(card, sizeof card, "hw:CARD=%.*s", (int)strcspn(c + 5, ","), c + 5);
    else if (!strncmp(dev, "plughw:", 7) || !strncmp(dev, "hw:", 3)) {
        const char *d = strchr(dev, ':') + 1;
        snprintf(card, sizeof card, "hw:%.*s", (int)strcspn(d, ","), d);
    } else snprintf(card, sizeof card, "default");
    int e;
    if ((e = M.open(mx, 0)) < 0) { snprintf(o->error, sizeof o->error, "mixer: %s", M.strerror(e)); return NULL; }
    if ((e = M.attach(*mx, card)) < 0 || (e = M.reg(*mx, NULL, NULL)) < 0 || (e = M.load(*mx)) < 0) {
        snprintf(o->error, sizeof o->error, e == -EACCES || e == -EPERM ? "no permission for the mixer (run qso-zero-setup)" : "mixer %s: %s",
                 card, M.strerror(e));
        M.close(*mx); *mx = NULL;
        return NULL;
    }
    const char *names[64];
    int has[64], n = 0;
    snd_mixer_elem_t *els[64];
    for (snd_mixer_elem_t *el = M.first(*mx); el && n < 64; el = M.next(el)) {
        if (!M.active(el)) continue;
        els[n] = el;
        names[n] = M.name(el);
        has[n] = capture ? M.has_cvol(el) : M.has_pvol(el);
        ++n;
    }
    int k = mixer_pick(names, has, n, capture);
    if (k < 0) {
        snprintf(o->error, sizeof o->error, "no %s level control on %s", capture ? "recording" : "playback", card);
        M.close(*mx); *mx = NULL;
        return NULL;
    }
    snprintf(o->control, sizeof o->control, "%s", names[k]);
    return els[k];
}

static void read_level(snd_mixer_elem_t *el, int capture, mixer_level_t *o)
{
    long lo = 0, hi = 0, v = 0;
    if (capture) { M.crange(el, &lo, &hi); M.cget(el, 0, &v); }
    else { M.prange(el, &lo, &hi); M.pget(el, 0, &v); }
    o->percent = hi > lo ? (int)lround(100.0 * (v - lo) / (hi - lo)) : 0;
    int sw = 1;
    if (capture ? M.has_csw(el) : M.has_psw(el)) {
        if (capture) M.csw_get(el, 0, &sw); else M.psw_get(el, 0, &sw);
    }
    o->muted = !sw;
    o->ok = 1;
}

int mixer_get(const char *dev, int capture, mixer_level_t *o)
{
    snd_mixer_t *mx;
    snd_mixer_elem_t *el = find(dev, capture, &mx, o);
    if (!el) return -1;
    read_level(el, capture, o);
    M.close(mx);
    return 0;
}

int mixer_set(const char *dev, int capture, int percent, mixer_level_t *o)
{
    snd_mixer_t *mx;
    snd_mixer_elem_t *el = find(dev, capture, &mx, o);
    if (!el) return -1;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    long lo = 0, hi = 0;
    if (capture) M.crange(el, &lo, &hi); else M.prange(el, &lo, &hi);
    long v = lo + lround((hi - lo) * percent / 100.0);
    int e = capture ? M.cset(el, v) : M.pset(el, v);
    if (capture ? M.has_csw(el) : M.has_psw(el)) { if (capture) M.csw_set(el, 1); else M.psw_set(el, 1); }
    if (e < 0) snprintf(o->error, sizeof o->error, "%s: %s", o->control, M.strerror(e));
    read_level(el, capture, o);
    M.close(mx);
    return e < 0 ? -1 : 0;
}
