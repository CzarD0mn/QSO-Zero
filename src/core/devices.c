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

/* does /proc/asound/card<N> have a pcm*c (capture) or pcm*p (playback) device? */
static int card_has(const char *root, int num, char kind)
{
    char dir[300];
    snprintf(dir, sizeof dir, "%s/proc/asound/card%d", root, num);
    DIR *d = opendir(dir);
    if (!d) return 0;
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t L = strlen(e->d_name);
        if (!strncmp(e->d_name, "pcm", 3) && L > 3 && e->d_name[L - 1] == kind) { found = 1; break; }
    }
    closedir(d);
    return found;
}

int dev_audio_list(const char *root, int capture, const char *auto_desc, dev_opt_t *o, int max)
{
    int n = 0;
    if (max < 2) return 0;
    set(&o[n++], "", capture ? "auto" : "same as input", auto_desc);
    set(&o[n++], "default", "default", "system default device");
    char path[300], line[256];
    snprintf(path, sizeof path, "%s/proc/asound/cards", root);
    FILE *f = fopen(path, "r");
    if (!f) return n;
    while (fgets(line, sizeof line, f) && n < max) {
        /* " 1 [QMX            ]: USB-Audio - QMX" */
        char *lb = strchr(line, '['), *rb = lb ? strchr(lb, ']') : NULL;
        if (!lb || !rb) continue;
        int num = atoi(line);
        char id[32];
        size_t L = (size_t)(rb - lb - 1);
        if (L >= sizeof id) L = sizeof id - 1;
        memcpy(id, lb + 1, L);
        id[L] = 0;
        while (L && id[L - 1] == ' ') id[--L] = 0;
        if (!card_has(root, num, capture ? 'c' : 'p')) continue;
        char *name = strstr(rb, " - ");
        name = name ? name + 3 : rb + 2;
        name[strcspn(name, "\r\n")] = 0;
        char v[64];
        snprintf(v, sizeof v, "plughw:CARD=%s", id);
        set(&o[n++], v, id, name);
    }
    fclose(f);
    return n;
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
    if (!strncmp(value, "plughw:CARD=", 12)) { snprintf(out, outsz, "%s", value + 12); return; }
    snprintf(out, outsz, "%s", value);
}
