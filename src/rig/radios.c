#include "radios.h"
#include <string.h>

/*
 * Hamlib model numbers (RIG_MAKE_MODEL(mfg, n) = mfg*1000 + n):
 *   Yaesu FT-891      1036   Kenwood TS-480 2028   QRP Labs QMX 2057
 *   Xiegu X6100       3087   Xiegu G90      3088   Xiegu X6200  3091
 *   Icom IC-705       3085
 * QMX / QMX+ data_mode is plain "USB" on purpose: on the owner's QMX
 * (firmware 1_04), FT8 decodes only with the radio in its USB (SSB) mode,
 * not in its Digi mode, so the CLAUDE.md "USB only if PKTUSB misbehaves"
 * exception applies. For the full passband the QMX's SSB menu "Filter RX"
 * must be 3200 Hz (default 2700); see qmx_usb() in core/app.c.
 * Band coverage: the QMX comes in three hardware versions with different
 * filters - 80-20 m, 60-15 m and 20-10 m - that look the same over CAT (it
 * would happily tune a band it has no filters for), so each is its own entry;
 * QMX+ 160-6 m, G90 160-10 m, X6100/X6200/FT-891/IC-705 160-6 m (the IC-705's
 * 2 m / 70 cm have no FT8/JS8/WSPR spots in the band table).
 * IC-705: native USB (no UART bridge), two CDC-ACM ports named
 * usb-Icom_Inc._IC-705_<serial>-if00 / -if02. CAT (CI-V, "Serial Port A") is
 * the first one, if00; if02 is USB (B) for GPS / RTTY decode / DV data. Audio
 * is a Burr-Brown "USB Audio CODEC" (ALSA id CODEC). CI-V address 0xA4, the
 * Hamlib default. Not yet tried on a Cardputer Zero: the device listing in
 * tests/test_devices.c is from the documentation and still to be replaced by
 * a real one.
 * FT-891: the radio's built-in CP2105 (Enhanced port = CAT, interface 0) is
 * the only serial port on its CAT path - the mini-DIN DATA jack has no serial
 * lines. The Digirig DR-891 passes that USB port through its own hub; its
 * PTT is CAT only (no RTS), confirmed on the owner's FT-891 + DR-891. 9600
 * baud is the rate Digirig's DR-891 manual gives for menu 05-06 CAT RATE.
 * Seen on the owner's Zero: usb-Silicon_Labs_CP2105_Dual_USB_to_UART_Bridge_
 * Controller_<serial>-if00-port0 (CAT) / -if01-port0, and the DR-891's C-Media
 * codec as ALSA card "Device" (tests/test_devices.c [1d]).
 * QMX and X6200 entries need Hamlib >= 4.6; on older Hamlib the app falls
 * back to the TS-480 / X6100 backends, which speak the same protocol.
 *
 * RULE FOR EVERY RADIO (existing and future): digital modes run in the
 * radio's DATA / USB-Digital mode. Set data_mode to "PKTUSB" (Hamlib's name
 * for USB-D / U-DIG / DATA-U / PKT-U). If a radio has no data mode, the app
 * falls back to USB at the widest passband the radio accepts (3 kHz first,
 * usually the maximum) - see set_data_mode() in core/app.c. Only put "USB"
 * here if the radio is known to misbehave when asked for PKTUSB.
 */
static const radio_profile_t k_radios[] = {
    {"qmx", "QRP Labs QMX 80-20m", 2057, 2028, 115200, PTT_CAT,
     "QMX", "/dev/ttyACM0", "QMX|QRP", "USB", 5.0f, BANDS_QMX_LOW,
     "QMX 80/60/40/30/20 m version. Single USB cable: CAT + audio. USB mode with SSB Filter RX 3200 Hz; TX audio 100%: the QMX keys up only above 80% audio.",
     1.0f, 1},
    {"qmxmid", "QRP Labs QMX 60-15m", 2057, 2028, 115200, PTT_CAT,
     "QMX", "/dev/ttyACM0", "QMX|QRP", "USB", 5.0f, BANDS_QMX_MID,
     "QMX 60/40/30/20/17/15 m version. Single USB cable: CAT + audio. USB mode with SSB Filter RX 3200 Hz; TX audio 100%.",
     1.0f, 1},
    {"qmxhigh", "QRP Labs QMX 20-10m", 2057, 2028, 115200, PTT_CAT,
     "QMX", "/dev/ttyACM0", "QMX|QRP", "USB", 5.0f, BANDS_QMX_HIGH,
     "QMX 20/17/15/12/11/10 m version. Single USB cable: CAT + audio. USB mode with SSB Filter RX 3200 Hz; TX audio 100%.",
     1.0f, 1},
    {"qmxplus", "QRP Labs QMX+", 2057, 2028, 115200, PTT_CAT,
     "QMX", "/dev/ttyACM0", "QMX|QRP", "USB", 5.0f, BANDS_ALL,
     "Same CAT/audio as QMX. 160-6 m band coverage.", 0, 1},
    {"g90", "Xiegu G90 + Digirig", 3088, 0, 19200, PTT_CAT,
     "CP210", "/dev/ttyUSB0", "Device|C-Media|USB Audio", "PKTUSB", 20.0f, BANDS_NO6,
     "Digirig CAT cable to G90 COMM port, audio to the head. Turn G90 power down for digital duty cycle."},
    {"x6100", "Xiegu X6100", 3087, 0, 19200, PTT_CAT,
     "1a86|USB_Dual_Serial|CH34|CP210|@if02", "/dev/ttyACM1", "X6100|CODEC|USB Audio", "PKTUSB", 10.0f, BANDS_ALL,
     "Built-in USB CAT + audio. Hamlib default CI-V address 0xA4; set CAT baud 19200 in the radio."},
    {"x6200", "Xiegu X6200", 3091, 3087, 19200, PTT_CAT,
     "1a86|USB_Dual_Serial|CH34|CP210|@if02", "/dev/ttyACM1", "X6200|CODEC|USB Audio", "PKTUSB", 8.0f, BANDS_ALL,
     "Built-in USB CAT + audio (uses Hamlib X6200 backend, X6100 on older Hamlib). CAT baud 19200."},
    {"ic705", "Icom IC-705", 3085, 0, 19200, PTT_CAT,
     "Icom|IC-705|IC705", "/dev/ttyACM0", "CODEC|Burr-Brown|IC-705|IC705", "PKTUSB", 10.0f, BANDS_ALL,
     "One USB cable to the micro-USB port: CAT (Serial Port A, if00) + audio. On the radio: SET > Connectors > MOD Input > "
     "DATA MOD = USB; CI-V address A4h; CI-V USB Echo Back OFF. 10 W on 13.8 V, 5 W on the battery."},
    {"ft891", "Yaesu FT-891 + Digirig", 1036, 0, 9600, PTT_CAT,
     "CP2105|CP210", "/dev/ttyUSB0", "Device|C-Media|USB Audio", "PKTUSB", 100.0f, BANDS_ALL,
     "CAT through the radio's own USB port (CP2105, Enhanced port), PTT over CAT: works with the Digirig DR-891 and with a "
     "Digirig Mobile plus the radio's USB cable. Radio menu: 05-06 CAT RATE 9600, 05-08 CAT RTS DISABLE, 08-09 DATA IN SELECT REAR, "
     "08-12 DATA BFO USB. Turn power down for QRP."},
};

const radio_profile_t *radio_find(const char *id)
{
    for (size_t i = 0; i < sizeof k_radios / sizeof *k_radios; ++i)
        if (!strcmp(k_radios[i].id, id)) return &k_radios[i];
    return NULL;
}

const radio_profile_t *radio_list(int *count)
{
    *count = (int)(sizeof k_radios / sizeof *k_radios);
    return k_radios;
}

const char *ptt_name(ptt_method_t p)
{
    switch (p) {
    case PTT_CAT: return "CAT";
    case PTT_RTS: return "RTS";
    case PTT_DTR: return "DTR";
    default: return "VOX";
    }
}
