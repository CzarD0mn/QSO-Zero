/* Device discovery for the Setup pickers, against a fake /dev + /proc tree:
 * a Digirig (CP2102) and a QMX on USB, the Cap GPS UART, and three sound
 * cards (HDMI playback-only, Digirig C-Media, QMX). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "core/devices.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok    " __VA_ARGS__); else { printf("  FAIL  " __VA_ARGS__); ++fails; } printf("\n"); } while (0)

static char root[64];

static void mk(const char *rel)
{
    char p[256];
    snprintf(p, sizeof p, "%s%s", root, rel);
    FILE *f = fopen(p, "w");
    if (f) fclose(f);
}

static void md(const char *rel) { char p[256]; snprintf(p, sizeof p, "%s%s", root, rel); mkdir(p, 0755); }

static int find(const dev_opt_t *o, int n, const char *label)
{
    for (int i = 0; i < n; ++i) if (!strcmp(o[i].label, label)) return i;
    return -1;
}

int main(void)
{
    snprintf(root, sizeof root, "/tmp/qsoz-devXXXXXX");
    if (!mkdtemp(root)) return 1;
    md("/dev"); md("/dev/serial"); md("/dev/serial/by-id");
    mk("/dev/ttyUSB0"); mk("/dev/ttyACM0"); mk("/dev/ttyACM1"); mk("/dev/serial0"); mk("/dev/tty1");
    char l[256];
    snprintf(l, sizeof l, "%s/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_a1b2-if00-port0", root);
    if (symlink("../../ttyUSB0", l)) return 1;
    snprintf(l, sizeof l, "%s/dev/serial/by-id/usb-QRP_Labs_QMX_Transceiver_0001-if00", root);
    if (symlink("../../ttyACM0", l)) return 1;
    md("/proc"); md("/proc/asound");
    FILE *f;
    char p[256];
    snprintf(p, sizeof p, "%s/proc/asound/cards", root);
    f = fopen(p, "w");
    fputs(" 0 [vc4hdmi        ]: vc4-hdmi - vc4-hdmi\n                      vc4-hdmi\n"
          " 1 [Device         ]: USB-Audio - USB Audio Device\n                      C-Media Electronics Inc. USB Audio Device at usb-...\n"
          " 2 [QMX            ]: USB-Audio - QMX Transceiver\n                      QRP Labs QMX Transceiver at usb-...\n", f);
    fclose(f);
    md("/proc/asound/card0"); mk("/proc/asound/card0/pcm0p");
    md("/proc/asound/card1"); mk("/proc/asound/card1/pcm0p"); mk("/proc/asound/card1/pcm0c");
    md("/proc/asound/card2"); mk("/proc/asound/card2/pcm0p"); mk("/proc/asound/card2/pcm0c");

    dev_opt_t o[DEV_MAX];
    printf("[1] serial ports\n");
    int n = dev_serial_list(root, "default /dev/ttyACM0", o, DEV_MAX);
    CHECK(n == 4, "auto + ttyACM0 + ttyACM1 + ttyUSB0 (got %d)", n);
    CHECK(!strcmp(o[0].label, "auto") && !o[0].value[0], "first entry is auto (blank value)");
    int u = find(o, n, "/dev/ttyUSB0"), q = find(o, n, "/dev/ttyACM0"), q1 = find(o, n, "/dev/ttyACM1");
    CHECK(u > 0 && strstr(o[u].value, "/dev/serial/by-id/usb-Silicon_Labs") == o[u].value, "Digirig saved by stable by-id path");
    CHECK(u > 0 && !strcmp(o[u].desc, "Silicon Labs CP2102N USB to UART Bridge Controller a1b2"), "Digirig described: \"%s\"", u > 0 ? o[u].desc : "");
    CHECK(q > 0 && strstr(o[q].desc, "QRP Labs QMX"), "QMX described: \"%s\"", q > 0 ? o[q].desc : "");
    CHECK(q1 > 0 && !strcmp(o[q1].value, "/dev/ttyACM1"), "port without by-id link listed by its /dev path");
    CHECK(find(o, n, "/dev/serial0") < 0 && find(o, n, "/dev/tty1") < 0, "GPS UART and consoles not offered for CAT");
    CHECK(q < q1 && q1 < u, "sorted by name");

    printf("[2] sound cards\n");
    n = dev_audio_list(root, 1, "match radio", o, DEV_MAX);
    CHECK(n == 4, "capture: auto, default, Device, QMX (got %d)", n);
    CHECK(find(o, n, "vc4hdmi") < 0, "HDMI (playback only) not offered for input");
    int c = find(o, n, "QMX");
    CHECK(c > 0 && !strcmp(o[c].value, "plughw:CARD=QMX") && !strcmp(o[c].desc, "QMX Transceiver"), "QMX -> plughw:CARD=QMX \"%s\"", c > 0 ? o[c].desc : "");
    n = dev_audio_list(root, 0, "same", o, DEV_MAX);
    CHECK(n == 5 && find(o, n, "vc4hdmi") > 0, "playback list includes HDMI (got %d)", n);
    CHECK(!strcmp(o[0].label, "same as input"), "output defaults to same as input");

    printf("[3] short names for the menu\n");
    char s[48];
    snprintf(l, sizeof l, "/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_a1b2-if00-port0");
    dev_short_name(root, l, s, sizeof s);
    CHECK(!strcmp(s, "ttyUSB0"), "by-id -> ttyUSB0 (%s)", s);
    dev_short_name(root, "/dev/serial/by-id/usb-gone-if00", s, sizeof s);
    CHECK(!strcmp(s, "(unplugged)"), "missing device -> (unplugged)");
    dev_short_name(root, "plughw:CARD=Device", s, sizeof s);
    CHECK(!strcmp(s, "Device"), "plughw:CARD=Device -> Device");
    dev_short_name(root, "", s, sizeof s);
    CHECK(!strcmp(s, "auto"), "blank -> auto");

    printf("[4] nothing plugged in\n");
    n = dev_serial_list("/nonexistent", "x", o, DEV_MAX);
    CHECK(n == 1, "only auto");
    n = dev_audio_list("/nonexistent", 1, "x", o, DEV_MAX);
    CHECK(n == 2, "only auto + default");

    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    if (system(cmd)) {}
    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}
