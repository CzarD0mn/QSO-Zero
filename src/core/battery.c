/* QSO-Zero - battery level and drain estimate from power-supply sysfs. */
#include "core/battery.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_str(const char *dir, const char *f, char *out, int sz)
{
    char p[400];
    snprintf(p, sizeof p, "%s/%s", dir, f);
    FILE *fp = fopen(p, "r");
    if (!fp) return -1;
    int ok = fgets(out, sz, fp) != NULL;
    fclose(fp);
    if (!ok) return -1;
    out[strcspn(out, "\r\n")] = 0;
    return 0;
}

static int read_ll(const char *dir, const char *f, long long *v)
{
    char b[64];
    if (read_str(dir, f, b, sizeof b)) return -1;
    char *e;
    *v = strtoll(b, &e, 10);
    return e == b ? -1 : 0;
}

int batt_read(const char *root, batt_state_t *st)
{
    char base[300], dir[400], b[64];
    snprintf(base, sizeof base, "%s/sys/class/power_supply", root);
    batt_t *n = &st->now;
    double pph = n->pct_per_hour, hl = n->hours_left;
    memset(n, 0, sizeof *n);
    n->percent = -1;
    n->pct_per_hour = pph; n->hours_left = hl;
    st->charge_now = st->charge_full = st->energy_now = st->energy_full = 0;
    DIR *d = opendir(base);
    if (!d) return -1;
    int ext_power = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(dir, sizeof dir, "%s/%s", base, e->d_name);
        if (read_str(dir, "type", b, sizeof b)) continue;
        long long v;
        if (strcmp(b, "Battery")) {                       /* Mains / USB: is it plugged in? */
            if (!read_ll(dir, "online", &v) && v > 0) ext_power = 1;
            continue;
        }
        if (n->present) continue;                         /* first battery only */
        n->present = 1;
        snprintf(n->name, sizeof n->name, "%s", e->d_name);
        if (!read_ll(dir, "charge_now", &v)) st->charge_now = (double)v;
        if (!read_ll(dir, "charge_full", &v)) st->charge_full = (double)v;
        if (!read_ll(dir, "energy_now", &v)) st->energy_now = (double)v;
        if (!read_ll(dir, "energy_full", &v)) st->energy_full = (double)v;
        if (!read_ll(dir, "capacity", &v)) n->percent = (int)v;
        else if (st->charge_full > 0) n->percent = (int)lround(100 * st->charge_now / st->charge_full);
        else if (st->energy_full > 0) n->percent = (int)lround(100 * st->energy_now / st->energy_full);
        if (n->percent > 100) n->percent = 100;
        if (!read_ll(dir, "voltage_now", &v)) n->volts = v / 1e6;          /* uV */
        if (!read_ll(dir, "current_now", &v)) n->amps = fabs(v / 1e6);     /* uA, sign varies by driver */
        if (!read_ll(dir, "power_now", &v)) n->watts = fabs(v / 1e6);      /* uW */
        if (!read_str(dir, "status", b, sizeof b)) {
            n->charging = !strcmp(b, "Charging");
            n->full = !strcmp(b, "Full");
        }
    }
    closedir(d);
    if (n->present && ext_power && !n->full && !n->charging && n->percent < 100) n->charging = 1;
    return n->present ? 0 : -1;
}

void batt_update(const char *root, batt_state_t *st, double t)
{
    int was_charging = st->now.charging;
    if (batt_read(root, st)) return;
    batt_t *n = &st->now;
    if (n->charging != was_charging) st->n = 0;           /* plugged / unplugged: start over */
    if (n->percent >= 0) {
        st->t[st->head] = t;
        st->pct[st->head] = n->percent;
        st->head = (st->head + 1) % BATT_HIST;
        if (st->n < BATT_HIST) ++st->n;
    }
    n->pct_per_hour = 0;
    n->hours_left = 0;
    /* 1. the gauge's own numbers: charge / current, energy / power */
    double rate_h = 0;
    if (n->amps > 0.005 && st->charge_full > 0) rate_h = n->amps * 1e6 / st->charge_full;       /* fraction per hour */
    else if (n->watts > 0.01 && st->energy_full > 0) rate_h = n->watts * 1e6 / st->energy_full;
    if (rate_h > 0) {
        n->pct_per_hour = 100 * rate_h;
        double frac = n->percent >= 0 ? n->percent / 100.0 : 0;
        n->hours_left = n->charging ? (1 - frac) / rate_h : frac / rate_h;
    }
    /* 2. otherwise the trend of the percentage, over at least 5 minutes */
    if (rate_h <= 0 && st->n >= 2) {
        int oldest = (st->head - st->n + BATT_HIST) % BATT_HIST, newest = (st->head - 1 + BATT_HIST) % BATT_HIST;
        double dt = st->t[newest] - st->t[oldest];
        int dp = st->pct[newest] - st->pct[oldest];
        if (dt >= 300 && dp != 0) {
            double r = abs(dp) / (dt / 3600.0);               /* %/h */
            n->pct_per_hour = r;
            if (n->charging && dp > 0) n->hours_left = (100 - n->percent) / r;
            else if (!n->charging && dp < 0) n->hours_left = n->percent / r;
        }
    }
    if (n->charging || n->full) n->pct_per_hour = n->charging ? -n->pct_per_hour : 0;
}
