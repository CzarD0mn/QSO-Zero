/* Battery level from a fake /sys/class/power_supply tree. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "core/app.h"
#include "core/battery.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok    " __VA_ARGS__); else { printf("  FAIL  " __VA_ARGS__); ++fails; } printf("\n"); } while (0)

static char root[64];

static void put(const char *sup, const char *file, const char *val)
{
    char p[256];
    snprintf(p, sizeof p, "%s/sys/class/power_supply/%s", root, sup);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/sys/class/power_supply/%s/%s", root, sup, file);
    FILE *f = fopen(p, "w");
    fprintf(f, "%s\n", val);
    fclose(f);
}

static void rm_sup(const char *sup)
{
    char c[256];
    snprintf(c, sizeof c, "rm -rf %s/sys/class/power_supply/%s", root, sup);
    if (system(c)) {}
}

static int near(double a, double b, double tol) { return a > b - tol && a < b + tol; }

int main(void)
{
    snprintf(root, sizeof root, "/tmp/qsoz-battXXXXXX");
    if (!mkdtemp(root)) return 1;
    char p[256];
    snprintf(p, sizeof p, "%s/sys", root); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/sys/class", root); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/sys/class/power_supply", root); mkdir(p, 0755);

    batt_state_t st;
    printf("[1] no battery\n");
    memset(&st, 0, sizeof st);
    batt_update("/nonexistent", &st, 0);
    CHECK(!st.now.present, "nothing shown when there is no power_supply");
    put("usb", "type", "USB"); put("usb", "online", "0");
    batt_update(root, &st, 0);
    CHECK(!st.now.present, "USB supply alone is not a battery");

    printf("[2] fuel gauge with current (discharging)\n");
    put("battery", "type", "Battery");
    put("battery", "status", "Discharging");
    put("battery", "capacity", "62");
    put("battery", "voltage_now", "3812000");      /* uV */
    put("battery", "current_now", "-450000");      /* uA, negative on some drivers */
    put("battery", "charge_full", "1500000");      /* uAh */
    put("battery", "charge_now", "930000");
    memset(&st, 0, sizeof st);
    batt_update(root, &st, 0);
    CHECK(st.now.present && st.now.percent == 62 && !st.now.charging, "62%%, on battery");
    CHECK(near(st.now.volts, 3.812, 1e-3) && near(st.now.amps, 0.45, 1e-3), "3.81 V, 0.45 A (sign ignored)");
    CHECK(near(st.now.pct_per_hour, 30, 0.1), "drain 30 %%/h (0.45 A of 1.5 Ah), got %.1f", st.now.pct_per_hour);
    CHECK(near(st.now.hours_left, 0.62 / 0.30, 0.01), "about %.2f h left (expect 2.07)", st.now.hours_left);

    printf("[3] capacity only: estimate from the trend\n");
    rm_sup("battery");
    put("battery", "type", "Battery");
    put("battery", "status", "Discharging");
    put("battery", "capacity", "80");
    memset(&st, 0, sizeof st);
    batt_update(root, &st, 0);
    CHECK(st.now.percent == 80 && st.now.hours_left == 0, "first reading: no estimate yet");
    put("battery", "capacity", "79");
    batt_update(root, &st, 240);
    CHECK(st.now.hours_left == 0, "under 5 minutes of data: still estimating");
    put("battery", "capacity", "78");
    batt_update(root, &st, 600);
    CHECK(near(st.now.pct_per_hour, 12, 0.01) && near(st.now.hours_left, 6.5, 0.01),
          "-2%% in 10 min -> 12 %%/h, 6.5 h left (got %.1f, %.2f)", st.now.pct_per_hour, st.now.hours_left);

    printf("[4] charging\n");
    put("battery", "status", "Charging");
    batt_update(root, &st, 660);
    CHECK(st.now.charging && st.now.hours_left == 0, "plugged in: charging, estimate restarts");
    put("battery", "status", "Not charging");
    put("usb", "online", "1");
    batt_update(root, &st, 700);
    CHECK(st.now.charging, "USB online + not full counts as charging");
    put("battery", "status", "Full"); put("battery", "capacity", "100");
    batt_update(root, &st, 760);
    CHECK(st.now.full && !st.now.charging, "full");
    put("usb", "online", "0");

    printf("[5] no capacity file: derived from charge\n");
    rm_sup("battery");
    put("battery", "type", "Battery");
    put("battery", "status", "Discharging");
    put("battery", "charge_full", "2000000");
    put("battery", "charge_now", "500000");
    memset(&st, 0, sizeof st);
    batt_update(root, &st, 0);
    CHECK(st.now.percent == 25, "500/2000 mAh -> 25%% (got %d)", st.now.percent);

    printf("[6] low-battery warning in the app\n");
    rm_sup("battery");
    put("battery", "type", "Battery");
    put("battery", "status", "Discharging");
    put("battery", "capacity", "14");
    put("battery", "current_now", "300000");
    put("battery", "charge_full", "1500000");
    setenv("QSOZERO_DEVROOT", root, 1);
    static app_t a;
    app_init(&a, "/nonexistent", 0);
    CHECK(a.batt.now.present && a.batt.now.percent == 14, "read at start-up");
    CHECK(strstr(a.status, "BATTERY 14%") && strstr(a.status, "min left"), "warns: \"%s\"", a.status);
    a.status[0] = 0;
    app_poll_battery(&a);
    CHECK(!a.status[0], "doesn't repeat at the same level");
    put("battery", "capacity", "4");
    app_poll_battery(&a);
    CHECK(strstr(a.status, "BATTERY 4%") != NULL, "warns again at 5%%: \"%s\"", a.status);

    snprintf(p, sizeof p, "rm -rf %s", root);
    if (system(p)) {}
    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}
