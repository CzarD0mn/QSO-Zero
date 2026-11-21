#define _GNU_SOURCE
/* QSO-Zero - serial port and sound card discovery for the Setup pickers. */
#include "core/devices.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void set(dev_opt_t *o, const char *v, const char *l, const char *d)
{
    snprintf(o->value, sizeof o->value, "%s", v);
    snprintf(o->label, sizeof o->label, "%s", l);
    snprintf(o->desc, sizeof o->desc, "%s", d);
}

static int cmp_label(const void *x, const void *y)
{
    return strcmp(((const dev_opt_t *)x)->label, ((const dev_opt_t *)y)->label);
}

/* by-id link -> "/dev/ttyUSB0", or "" */
static void link_target(const char *root, const char *path, char *out, int outsz)
{
    char full[300], t[256];
    snprintf(full, sizeof full, "%s%s", root, path);
    ssize_t n = readlink(full, t, sizeof t - 1);
    out[0] = 0;
    if (n <= 0) return;
    t[n] = 0;
    const char *b = strrchr(t, '/');
    snprintf(out, outsz, "/dev/%s", b ? b + 1 : t);
}

/* "usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_1234-if00-port0"
 *   -> "Silicon Labs CP2102N USB to UART Bridge Controller 1234" */
static void tidy_id(const char *id, char *out, int outsz)
{
    const char *s = !strncmp(id, "usb-", 4) ? id + 4 : id;
    snprintf(out, outsz, "%s", s);
    char *e = strstr(out, "-if0");
    if (e) *e = 0;
    for (char *p = out; *p; ++p) if (*p == '_') *p = ' ';
}

int dev_serial_list(const char *root, const char *auto_desc, dev_opt_t *o, int max)
{
    int n = 0;
    if (max < 1) return 0;
    set(&o[n++], "", "auto", auto_desc);
    int first = n;
    char dir[300], p[300], tgt[64], desc[128];
    snprintf(dir, sizeof dir, "%s/dev/serial/by-id", root);
    DIR *d = opendir(dir);
    struct dirent *e;
    if (d) {
        while ((e = readdir(d)) && n < max) {
            if (e->d_name[0] == '.') continue;
            snprintf(p, sizeof p, "/dev/serial/by-id/%s", e->d_name);
            link_target(root, p, tgt, sizeof tgt);
            if (!tgt[0]) continue;
            tidy_id(e->d_name, desc, sizeof desc);
            set(&o[n++], p, tgt, desc);
        }
        closedir(d);
    }
    snprintf(dir, sizeof dir, "%s/dev", root);
    if ((d = opendir(dir))) {
        while ((e = readdir(d)) && n < max) {
            if (strncmp(e->d_name, "ttyUSB", 6) && strncmp(e->d_name, "ttyACM", 6)) continue;
            snprintf(p, sizeof p, "/dev/%s", e->d_name);
            int dup = 0;
            for (int i = first; i < n; ++i) if (!strcmp(o[i].label, p)) dup = 1;
            if (!dup) set(&o[n++], p, p, "USB serial");
        }
        closedir(d);
    }
    qsort(o + first, n - first, sizeof *o, cmp_label);
    return n;
}

/* ---- sound devices -------------------------------------------------- */

typedef struct { int num; char id[32]; char driver[32]; char name[80]; char longname[80]; } card_t;

static int read_cards(const char *root, card_t *c, int max)
{
    char path[300], line[256], line2[256];
    snprintf(path, sizeof path, "%s/proc/asound/cards", root);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int n = 0;
    while (n < max && fgets(line, sizeof line, f)) {
        /* " 1 [QMX            ]: USB-Audio - QMX Transceiver" + a long-name line */
        char *lb = strchr(line, '['), *rb = lb ? strchr(lb, ']') : NULL;
        if (!lb || !rb) continue;
        card_t *k = &c[n];
        memset(k, 0, sizeof *k);
        k->num = atoi(line);
        size_t L = (size_t)(rb - lb - 1);
        if (L >= sizeof k->id) L = sizeof k->id - 1;
        memcpy(k->id, lb + 1, L);
        while (L && k->id[L - 1] == ' ') k->id[--L] = 0;
        const char *p = rb + 1;
        if (*p == ':') ++p;
        while (*p == ' ') ++p;
        const char *dash = strstr(p, " - ");
        size_t dl = dash ? (size_t)(dash - p) : strcspn(p, "\r\n");
        if (dl >= sizeof k->driver) dl = sizeof k->driver - 1;
        memcpy(k->driver, p, dl);
        snprintf(k->name, sizeof k->name, "%s", dash ? dash + 3 : k->id);
        k->name[strcspn(k->name, "\r\n")] = 0;
        if (fgets(line2, sizeof line2, f)) {          /* long name: "C-Media ... USB Audio Device at usb-..., full speed" */
            char *q = line2;
            while (*q == ' ') ++q;
            q[strcspn(q, "\r\n")] = 0;
            char *at = strstr(q, " at usb-");
            if (at) *at = 0;
            char *inc = strstr(q, " Electronics Inc.");             /* "C-Media Electronics Inc. USB ..." */
            if (inc) memmove(inc, inc + 17, strlen(inc + 17) + 1);
            snprintf(k->longname, sizeof k->longname, "%s", q);
        }
        ++n;
    }
    fclose(f);
    return n;
}

static int is_usb(const card_t *k) { return !strcmp(k->driver, "USB-Audio"); }
static int is_hdmi(const card_t *k) { return strcasestr(k->id, "hdmi") || strcasestr(k->name, "hdmi"); }

/* pcm endpoints of one direction: from /proc/asound/pcm
 * ("01-00: USB Audio : USB Audio : playback 1 : capture 1"), else from the
 * card directories (pcm0c / pcm0p). */
typedef struct { int card, dev; char pname[64]; } ep_t;

static int read_endpoints(const char *root, int capture, ep_t *e, int max, const card_t *cards, int nc)
{
    char path[300], line[256];
    int n = 0;
    snprintf(path, sizeof path, "%s/proc/asound/pcm", root);
    FILE *f = fopen(path, "r");
    if (f) {
        while (n < max && fgets(line, sizeof line, f)) {
            int c, d;
            if (sscanf(line, "%d-%d:", &c, &d) != 2) continue;
            if (!strstr(line, capture ? "capture" : "playback")) continue;
            char *a = strstr(line, ": ");
            char *b = a ? strstr(a + 2, " : ") : NULL;      /* after the id field */
            char *z = b ? strstr(b + 3, " : ") : NULL;      /* end of the name field */
            e[n].card = c; e[n].dev = d; e[n].pname[0] = 0;
            if (b && z) snprintf(e[n].pname, sizeof e[n].pname, "%.*s", (int)(z - b - 3), b + 3);
            ++n;
        }
        fclose(f);
        return n;
    }
    for (int i = 0; i < nc && n < max; ++i) {
        snprintf(path, sizeof path, "%s/proc/asound/card%d", root, cards[i].num);
        DIR *d = opendir(path);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d)) && n < max) {
            size_t L = strlen(de->d_name);
            if (strncmp(de->d_name, "pcm", 3) || L < 5 || de->d_name[L - 1] != (capture ? 'c' : 'p')) continue;
            e[n].card = cards[i].num; e[n].dev = atoi(de->d_name + 3); e[n].pname[0] = 0;
            ++n;
        }
        closedir(d);
    }
    return n;
}

static const card_t *card_by_num(const card_t *c, int nc, int num)
{
    for (int i = 0; i < nc; ++i) if (c[i].num == num) return &c[i];
    return NULL;
}

static void ep_value(const card_t *k, int dev, char *out, size_t sz)
{
    snprintf(out, sz, "plughw:CARD=%s,DEV=%d", k->id, dev);
}

static int cmp_dev(const void *x, const void *y)    /* USB first, then by label */
{
    const dev_opt_t *a = x, *b = y;
    int ua = !strncmp(a->desc, "USB", 3), ub = !strncmp(b->desc, "USB", 3);
    if (ua != ub) return ub - ua;
    return strcmp(a->label, b->label);
}

int dev_audio_list(const char *root, int capture, const char *auto_desc, dev_opt_t *o, int max)
{
    int n = 0;
    if (max < 1) return 0;
    set(&o[n++], "", "auto", auto_desc);
    card_t cards[16];
    ep_t ep[48];
    int nc = read_cards(root, cards, 16);
    int ne = read_endpoints(root, capture, ep, 48, cards, nc);
    int first = n;
    for (int i = 0; i < ne && n < max; ++i) {
        const card_t *k = card_by_num(cards, nc, ep[i].card);
        if (!k) continue;
        char v[96], l[48], d[200], pn[64];
        ep_value(k, ep[i].dev, v, sizeof v);
        int multi = 0;
        for (int j = 0; j < ne; ++j) if (j != i && ep[j].card == ep[i].card) multi = 1;
        /* like other computers show it: "Microphone (USB Audio Device)" / "Speakers (USB Audio Device)" */
        if (multi) snprintf(l, sizeof l, "%s,%d %s", k->id, ep[i].dev, capture ? "(input)" : "(output)");
        else snprintf(l, sizeof l, "%s %s", k->id, capture ? "(input)" : "(output)");
        /* device name, minus driver noise like "(null)-" and repeats of the card name */
        snprintf(pn, sizeof pn, "%s", !strncmp(ep[i].pname, "(null)-", 7) ? ep[i].pname + 7 : ep[i].pname);
        const char *cn = is_usb(k) && k->longname[0] ? k->longname : k->name;
        int show_pn = multi && pn[0] && !strstr(cn, pn);
        snprintf(d, sizeof d, "%s %s: %s%s%s", is_usb(k) ? "USB" : is_hdmi(k) ? "HDMI" : "built-in",
                 capture ? "in" : "out", cn, show_pn ? " - " : "", show_pn ? pn : "");
        set(&o[n++], v, l, d);
    }
    qsort(o + first, n - first, sizeof *o, cmp_dev);
    return n;
}

int dev_audio_auto(const char *root, const char *hints, int capture, const char *prefer_card,
                   char *out, int outsz)
{
    card_t cards[16];
    ep_t ep[48];
    int nc = read_cards(root, cards, 16);
    int ne = read_endpoints(root, capture, ep, 48, cards, nc);
    int best = -1, best_score = 0;
    for (int i = 0; i < ne; ++i) {
        const card_t *k = card_by_num(cards, nc, ep[i].card);
        if (!k || is_hdmi(k) || !is_usb(k)) continue;       /* a radio is a USB sound card */
        int score = 100 - ep[i].dev;
        if (hints && *hints) {
            char h[128];
            snprintf(h, sizeof h, "%s", hints);
            for (char *save = NULL, *t = strtok_r(h, "|", &save); t; t = strtok_r(NULL, "|", &save))
                if (strcasestr(k->id, t) || strcasestr(k->name, t)) { score += 50; break; }
        }
        if (prefer_card && *prefer_card && !strcmp(k->id, prefer_card)) score += 200;   /* same radio codec */
        if (score > best_score) { best_score = score; best = i; }
    }
    if (best < 0) { out[0] = 0; return -1; }
    ep_value(card_by_num(cards, nc, ep[best].card), ep[best].dev, out, (size_t)outsz);
    return 0;
}

int dev_audio_present(const char *root, const char *value, int capture)
{
    if (!value[0]) return 1;
    if (strncmp(value, "plughw:CARD=", 12) && strncmp(value, "hw:CARD=", 8)) return 1;   /* can't tell: let ALSA try */
    const char *id = strstr(value, "CARD=") + 5;
    char want[40];
    snprintf(want, sizeof want, "%.*s", (int)strcspn(id, ","), id);
    const char *dv = strstr(value, "DEV=");
    int dev = dv ? atoi(dv + 4) : -1;
    card_t cards[16];
    ep_t ep[48];
    int nc = read_cards(root, cards, 16);
    int ne = read_endpoints(root, capture, ep, 48, cards, nc);
    for (int i = 0; i < ne; ++i) {
        const card_t *k = card_by_num(cards, nc, ep[i].card);
        if (k && !strcmp(k->id, want) && (dev < 0 || dev == ep[i].dev)) return 1;
    }
    return 0;
}

void dev_short_name(const char *root, const char *value, char *out, int outsz)
{
    if (!value[0]) { snprintf(out, outsz, "auto"); return; }
    if (!strncmp(value, "/dev/serial/by-id/", 18)) {
        char t[64];
        link_target(root, value, t, sizeof t);
        snprintf(out, outsz, "%s", t[0] ? t + 5 : "(unplugged)");
        return;
    }
    if (!strncmp(value, "/dev/", 5)) { snprintf(out, outsz, "%s", value + 5); return; }
    if (!strncmp(value, "plughw:CARD=", 12)) {
        const char *c = strstr(value, ",DEV=");
        if (c && atoi(c + 5) == 0) snprintf(out, outsz, "%.*s", (int)(c - value - 12), value + 12);
        else if (c) snprintf(out, outsz, "%.*s,%d", (int)(c - value - 12), value + 12, atoi(c + 5));
        else snprintf(out, outsz, "%s", value + 12);
        return;
    }
    snprintf(out, outsz, "%s", value);
}
