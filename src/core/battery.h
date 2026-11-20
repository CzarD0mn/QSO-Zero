/* QSO-Zero - battery level from Linux power-supply sysfs
 * (/sys/class/power_supply/<name>/ with type "Battery"), the same source
 * M5Stack's CardputerZero template uses. */
#ifndef QSOZERO_BATTERY_H
#define QSOZERO_BATTERY_H

#define BATT_HIST 64

typedef struct {
    int    present;       /* a Battery supply was found */
    int    percent;       /* 0-100, -1 unknown */
    int    charging;      /* status "Charging" (or external power online and not full) */
    int    full;          /* status "Full" */
    double volts, amps, watts;   /* 0 = not reported */
    char   name[32];
    /* filled in by batt_update(): */
    double pct_per_hour;  /* > 0 while discharging, 0 unknown */
    double hours_left;    /* to empty (discharging) or full (charging); 0 unknown */
} batt_t;

typedef struct {
    batt_t now;
    double t[BATT_HIST]; int pct[BATT_HIST]; int n, head;
    double charge_now, charge_full, energy_now, energy_full;   /* raw, for time left */
    int    warned;        /* lowest warning level shown (15, 5) */
} batt_state_t;

/* Read once. root "" on the device. Returns 0 if a battery was found. */
int  batt_read(const char *root, batt_state_t *st);
/* Read and update the rate / time-left estimate. t = monotonic seconds. */
void batt_update(const char *root, batt_state_t *st, double t);

#endif
