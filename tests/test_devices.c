/* Device discovery for the Setup pickers, against a fake /dev + /proc tree:
 * a Digirig (CP2102) and a QMX on USB, the Cap GPS UART, and three sound
 * cards (HDMI playback-only, Digirig C-Media, QMX). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "core/devices.h"
#include "rig/radios.h"

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
    size_t L = strlen(label);
    for (int i = 0; i < n; ++i) if (!strcmp(o[i].label, label) || (!strncmp(o[i].label, label, L) && o[i].label[L] == ' ')) return i;
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
    /* a Cardputer Zero with a QMX and a Digirig plugged in */
    md("/proc"); md("/proc/asound");
    FILE *f;
    char p[256];
    snprintf(p, sizeof p, "%s/proc/asound/cards", root);
    f = fopen(p, "w");
    fputs(" 0 [ES8388Audio    ]: simple-card - ES8388-Audio\n                      ES8388-Audio\n"
          " 1 [vc4hdmi        ]: vc4-hdmi - vc4-hdmi\n                      vc4-hdmi\n"
          " 2 [QMX            ]: USB-Audio - QMX Transceiver\n                      QRP Labs QMX Transceiver at usb-...\n"
          " 3 [Device         ]: USB-Audio - USB Audio Device\n                      C-Media Electronics Inc. USB Audio Device at usb-...\n", f);
    fclose(f);
    snprintf(p, sizeof p, "%s/proc/asound/pcm", root);
    f = fopen(p, "w");
    fputs("00-00: ES8388 HiFi es8388-hifi-0 : ES8388 HiFi es8388-hifi-0 : playback 1 : capture 1\n"
          "01-00: MAI PCM i2s-hifi-0 : MAI PCM i2s-hifi-0 : playback 1\n"
          "02-00: USB Audio : USB Audio : playback 1 : capture 1\n"
          "03-00: USB Audio : USB Audio : playback 1 : capture 1\n", f);
    fclose(f);

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

    printf("[1b] automatic CAT port\n");
    {
        char r3[] = "/tmp/qsoz-serXXXXXX", q[300], v3[160];
        if (!mkdtemp(r3)) return 1;
        snprintf(q, sizeof q, "mkdir -p %s/dev/serial/by-id && cd %s/dev && touch ttyACM0 ttyACM1 ttyS0 serial0 && cd serial/by-id && "
                 "ln -s ../../ttyACM0 usb-1a86_USB_Dual_Serial_56F3067297-if00 && ln -s ../../ttyACM1 usb-1a86_USB_Dual_Serial_56F3067297-if02", r3, r3);
        if (system(q)) return 1;
        /* the X6200 on the user's Cardputer Zero: CH342 dual serial; the radio answered on if02 */
        CHECK(!dev_serial_auto(r3, "1a86|USB_Dual_Serial|CH34|CP210|@if02", "/dev/ttyACM1", v3, sizeof v3)
              && strstr(v3, "usb-1a86_USB_Dual_Serial_56F3067297-if02"), "X6200 -> %s", v3);
        char sn[48];
        dev_short_name(r3, v3, sn, sizeof sn);
        CHECK(!strcmp(sn, "ttyACM1"), "shown as ttyACM1");
        char cand[4][160];
        int nc = dev_serial_candidates(r3, "1a86|USB_Dual_Serial|CH34|CP210|@if02", cand, 4);
        CHECK(nc == 2 && strstr(cand[0], "-if02") && strstr(cand[1], "-if00"), "fallback order: if02, then if00 (%d)", nc);
        nc = dev_serial_candidates(r3, "1a86", cand, 4);
        CHECK(nc == 2 && strstr(cand[0], "-if00"), "no @ifNN: first interface first");
        CHECK(!dev_serial_auto(r3, "CP210", "/dev/ttyUSB0", v3, sizeof v3) && (dev_short_name(r3, v3, sn, sizeof sn), !strcmp(sn, "ttyACM0")),
              "no match and default missing -> first USB serial port (%s), not the GPS UART", v3);
        CHECK(!dev_serial_auto(r3, "nothing", "/dev/ttyACM1", v3, sizeof v3) && !strcmp(v3, "/dev/ttyACM1"), "existing default kept (%s)", v3);
        snprintf(q, sizeof q, "rm -rf %s", r3); if (system(q)) {}
    }

    printf("[1c] Icom IC-705 (names from Icom / fldigi documentation; replace with a real device listing)\n");
    {
        /* PROVISIONAL: not yet captured from a Cardputer Zero with an IC-705.
         * Native USB CDC-ACM, VID 0c26 PID 0036: CAT (CI-V, "Serial Port A")
         * on if00, USB (B) (GPS / RTTY decode / DV data) on if02; audio is a
         * Burr-Brown "USB Audio CODEC". */
        char r4[] = "/tmp/qsoz-705XXXXXX", q[600], v4[160];
        if (!mkdtemp(r4)) return 1;
        snprintf(q, sizeof q, "mkdir -p %s/dev/serial/by-id %s/proc/asound && cd %s/dev && touch ttyACM0 ttyACM1 ttyS0 serial0 && cd serial/by-id && "
                 "ln -s ../../ttyACM0 usb-Icom_Inc._IC-705_12001234-if00 && ln -s ../../ttyACM1 usb-Icom_Inc._IC-705_12001234-if02", r4, r4, r4);
        if (system(q)) return 1;
        const radio_profile_t *ic = radio_find("ic705");
        CHECK(ic && !strcmp(ic->data_mode, "PKTUSB") && ic->hamlib_model == 3085, "ic705 profile: Hamlib 3085, PKTUSB (USB-D)");
        CHECK(ic && !dev_serial_auto(r4, ic->serial_hint, ic->serial_default, v4, sizeof v4) && strstr(v4, "IC-705_12001234-if00"),
              "IC-705 CAT -> %s (Serial Port A, not the GPS port)", v4);
        snprintf(q, sizeof q, "%s/proc/asound/cards", r4);
        f = fopen(q, "w");
        fputs(" 0 [vc4hdmi        ]: vc4-hdmi - vc4-hdmi\n"
              "                      vc4-hdmi\n"
              " 1 [ES8389Audio    ]: cardputerzero-a - ES8389-Audio\n"
              "                      ES8389-Audio\n"
              " 2 [CODEC          ]: USB-Audio - USB Audio CODEC\n"
              "                      Burr-Brown from TI USB Audio CODEC at usb-3f980000.usb-1.2, full speed\n", f);
        fclose(f);
        snprintf(q, sizeof q, "%s/proc/asound/pcm", r4);
        f = fopen(q, "w");
        fputs("00-00: MAI PCM i2s-hifi-0 : MAI PCM i2s-hifi-0 : playback 1\n"
              "01-00: (null)-ES8389 HiFi ES8389 HiFi-0 : (null)-ES8389 HiFi ES8389 HiFi-0 : playback 1 : capture 1\n"
              "02-00: USB Audio : USB Audio : playback 1 : capture 1\n", f);
        fclose(f);
        char vi[96] = "", vo[96] = "";
        CHECK(ic && !dev_audio_auto(r4, ic->audio_hint, 1, NULL, vi, sizeof vi) && !strcmp(vi, "plughw:CARD=CODEC,DEV=0")
              && !dev_audio_auto(r4, ic->audio_hint, 0, "CODEC", vo, sizeof vo) && !strcmp(vo, "plughw:CARD=CODEC,DEV=0"),
              "IC-705 audio in %s, out %s", vi, vo);
        snprintf(q, sizeof q, "rm -rf %s", r4); if (system(q)) {}
    }

    printf("[1d] Yaesu FT-891: its own CP2105 (Enhanced = if00) wins over a Digirig Mobile CP2102N\n");
    {
        /* PROVISIONAL names (Silicon Labs CP2105 / CP2102N by-id form); replace
         * with the listing from the owner's FT-891 + Digirig DR-891 */
        char r5[] = "/tmp/qsoz-891XXXXXX", q[700], v5[160];
        if (!mkdtemp(r5)) return 1;
        snprintf(q, sizeof q, "mkdir -p %s/dev/serial/by-id && cd %s/dev && touch ttyUSB0 ttyUSB1 ttyUSB2 && cd serial/by-id && "
                 "ln -s ../../ttyUSB0 usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_a1b2-if00-port0 && "
                 "ln -s ../../ttyUSB1 usb-Silicon_Labs_CP2105_Dual_USB_to_UART_Bridge_Controller_01A2B3C4-if00-port0 && "
                 "ln -s ../../ttyUSB2 usb-Silicon_Labs_CP2105_Dual_USB_to_UART_Bridge_Controller_01A2B3C4-if01-port0", r5, r5);
        if (system(q)) return 1;
        const radio_profile_t *ft = radio_find("ft891");
        CHECK(ft && ft->ptt == PTT_CAT && ft->baud == 9600, "ft891 profile: PTT over CAT, 9600 baud (Digirig DR-891)");
        CHECK(ft && !dev_serial_auto(r5, ft->serial_hint, ft->serial_default, v5, sizeof v5) && strstr(v5, "CP2105_Dual_USB_to_UART_Bridge_Controller_01A2B3C4-if00"),
              "FT-891 CAT -> %s (the radio's Enhanced port, not the Digirig's CP2102N)", v5);
        snprintf(q, sizeof q, "rm -f %s/dev/serial/by-id/*CP2102N*", r5); if (system(q)) {}
        CHECK(ft && !dev_serial_auto(r5, ft->serial_hint, ft->serial_default, v5, sizeof v5) && strstr(v5, "01A2B3C4-if00"),
              "DR-891 alone (radio's CP2105 only) -> Enhanced port if00");
        snprintf(q, sizeof q, "rm -rf %s", r5); if (system(q)) {}
    }

    printf("[2] sound devices, split by direction\n");
    n = dev_audio_list(root, 1, "auto", o, DEV_MAX);
    CHECK(n == 4, "Audio in: auto + QMX, Device, ES8388Audio (got %d)", n);
    CHECK(find(o, n, "vc4hdmi") < 0, "HDMI (playback only) is not offered as an input");
    CHECK(!strncmp(o[1].label, "QMX", 3) || !strncmp(o[1].label, "Device", 6), "USB devices listed first (%s)", o[1].label);
    int c = find(o, n, "QMX"), es = find(o, n, "ES8388Audio");
    CHECK(c > 0 && !strcmp(o[c].value, "plughw:CARD=QMX,DEV=0") && !strcmp(o[c].desc, "USB in: QRP Labs QMX Transceiver"),
          "QMX -> plughw:CARD=QMX,DEV=0 \"%s\"", c > 0 ? o[c].desc : "");
    CHECK(es > 0 && !strncmp(o[es].desc, "built-in", 8), "built-in codec marked: \"%s\"", es > 0 ? o[es].desc : "");
    CHECK(!strcmp(o[0].label, "auto"), "auto first; no \"default\" / \"same as input\"");
    n = dev_audio_list(root, 0, "auto", o, DEV_MAX);
    CHECK(n == 5 && find(o, n, "vc4hdmi") > 0, "Audio out: auto + QMX, Device, ES8388Audio, HDMI (got %d)", n);

    printf("[3] automatic choice\n");
    char v[96];
    CHECK(!dev_audio_auto(root, "QMX|QRP", 1, NULL, v, sizeof v) && !strcmp(v, "plughw:CARD=QMX,DEV=0"), "QMX radio: in = %s", v);
    CHECK(!dev_audio_auto(root, "QMX|QRP", 0, "QMX", v, sizeof v) && !strcmp(v, "plughw:CARD=QMX,DEV=0"), "QMX radio: out = %s", v);
    CHECK(!dev_audio_auto(root, "Device|C-Media|USB Audio", 1, NULL, v, sizeof v) && !strcmp(v, "plughw:CARD=Device,DEV=0"), "G90/FT-891 (Digirig): in = %s", v);
    CHECK(!dev_audio_auto(root, "nothing-matches", 1, NULL, v, sizeof v) && (strstr(v, "CARD=QMX") || strstr(v, "CARD=Device")),
          "no hint match: still a USB card, never the built-in (%s)", v);
    CHECK(dev_audio_present(root, "plughw:CARD=QMX,DEV=0", 1) && !dev_audio_present(root, "plughw:CARD=Gone,DEV=0", 1),
          "unplugged saved device detected");
    CHECK(!dev_audio_present(root, "plughw:CARD=vc4hdmi,DEV=0", 1), "HDMI is not an input");
    /* radio unplugged: only built-in + HDMI */
    snprintf(p, sizeof p, "%s/proc/asound/pcm", root);
    f = fopen(p, "w");
    fputs("00-00: ES8388 HiFi es8388-hifi-0 : ES8388 HiFi es8388-hifi-0 : playback 1 : capture 1\n"
          "01-00: MAI PCM i2s-hifi-0 : MAI PCM i2s-hifi-0 : playback 1\n", f);
    fclose(f);
    CHECK(dev_audio_auto(root, "QMX|QRP", 1, NULL, v, sizeof v) != 0, "no USB sound card -> auto finds nothing (not the built-in)");
    unlink(p);                                     /* no /proc/asound/pcm: use the card directories */
    md("/proc/asound/card2"); mk("/proc/asound/card2/pcm0p"); mk("/proc/asound/card2/pcm0c");
    CHECK(!dev_audio_auto(root, "QMX", 1, NULL, v, sizeof v) && !strcmp(v, "plughw:CARD=QMX,DEV=0"), "fallback to card directories: %s", v);

    printf("[3b] a real Cardputer Zero + Xiegu X6200 over USB (from the device)\n");
    {
        char r2[] = "/tmp/qsoz-realXXXXXX", q[256];
        if (!mkdtemp(r2)) return 1;
        snprintf(q, sizeof q, "mkdir -p %s/proc/asound", r2); if (system(q)) {}
        snprintf(q, sizeof q, "%s/proc/asound/cards", r2);
        f = fopen(q, "w");
        fputs(" 0 [vc4hdmi        ]: vc4-hdmi - vc4-hdmi\n"
              "                      vc4-hdmi\n"
              " 1 [ES8389Audio    ]: cardputerzero-a - ES8389-Audio\n"
              "                      ES8389-Audio\n"
              " 2 [Device         ]: USB-Audio - USB Audio Device\n"
              "                      C-Media Electronics Inc. USB Audio Device at usb-3f980000.usb-1.2.2, full speed\n", f);
        fclose(f);
        snprintf(q, sizeof q, "%s/proc/asound/pcm", r2);
        f = fopen(q, "w");
        fputs("00-00: MAI PCM i2s-hifi-0 : MAI PCM i2s-hifi-0 : playback 1\n"
              "01-00: (null)-ES8389 HiFi ES8389 HiFi-0 : (null)-ES8389 HiFi ES8389 HiFi-0 : playback 1 : capture 1\n"
              "02-00: USB Audio : USB Audio : playback 1 : capture 1\n", f);
        fclose(f);
        n = dev_audio_list(r2, 1, "auto", o, DEV_MAX);
        CHECK(n == 3 && !strcmp(o[1].label, "Device (input)") && !strcmp(o[2].label, "ES8389Audio (input)"),
              "Audio in: auto, \"%s\", \"%s\"", o[1].label, o[2].label);
        CHECK(!strcmp(o[1].desc, "USB in: C-Media USB Audio Device"), "X6200 codec described: \"%s\"", o[1].desc);
        CHECK(!strcmp(o[2].desc, "built-in in: ES8389-Audio"), "no \"(null)-\" noise: \"%s\"", o[2].desc);
        n = dev_audio_list(r2, 0, "auto", o, DEV_MAX);
        CHECK(n == 4 && !strcmp(o[1].label, "Device (output)") && !strcmp(o[1].desc, "USB out: C-Media USB Audio Device")
              && find(o, n, "vc4hdmi") == 3, "Audio out: auto, \"%s\", ES8389Audio, vc4hdmi", o[1].label);
        const char *radios[] = {"QMX|QRP", "Device|C-Media|USB Audio", "X6100|CODEC|USB Audio"};
        int all = 1;
        for (int k = 0; k < 3; ++k) {
            char vi[96], vo[96];
            all &= !dev_audio_auto(r2, radios[k], 1, NULL, vi, sizeof vi) && !strcmp(vi, "plughw:CARD=Device,DEV=0");
            all &= !dev_audio_auto(r2, radios[k], 0, "Device", vo, sizeof vo) && !strcmp(vo, "plughw:CARD=Device,DEV=0");
        }
        CHECK(all, "auto = the radio's USB codec (plughw:CARD=Device,DEV=0) for in and out, never ES8389 or HDMI");
        snprintf(q, sizeof q, "rm -rf %s", r2); if (system(q)) {}
    }

    printf("[4] short names for the menu\n");
    char s[48];
    snprintf(l, sizeof l, "/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_a1b2-if00-port0");
    dev_short_name(root, l, s, sizeof s);
    CHECK(!strcmp(s, "ttyUSB0"), "by-id -> ttyUSB0 (%s)", s);
    dev_short_name(root, "/dev/serial/by-id/usb-gone-if00", s, sizeof s);
    CHECK(!strcmp(s, "(unplugged)"), "missing device -> (unplugged)");
    dev_short_name(root, "plughw:CARD=Device,DEV=0", s, sizeof s);
    CHECK(!strcmp(s, "Device"), "plughw:CARD=Device,DEV=0 -> Device");
    dev_short_name(root, "plughw:CARD=X6100,DEV=1", s, sizeof s);
    CHECK(!strcmp(s, "X6100,1"), "DEV=1 -> X6100,1");
    dev_short_name(root, "", s, sizeof s);
    CHECK(!strcmp(s, "auto"), "blank -> auto");

    printf("[5] nothing plugged in\n");
    n = dev_serial_list("/nonexistent", "x", o, DEV_MAX);
    CHECK(n == 1, "only auto");
    n = dev_audio_list("/nonexistent", 1, "x", o, DEV_MAX);
    CHECK(n == 1, "only auto");

    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    if (system(cmd)) {}
    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}
