#include "radios.h"
#include <string.h>

/*
 * Hamlib model numbers (RIG_MAKE_MODEL(mfg, n) = mfg*1000 + n):
 *   Yaesu FT-891      1036   Kenwood TS-480 2028   QRP Labs QMX 2057
 *   Xiegu X6100       3087   Xiegu G90      3088   Xiegu X6200  3091
 * Band coverage: QMX 80-10 m (the 5-band QMX ships in low- or high-band
 * versions; ones it lacks are detected when the radio refuses the QSY and
 * skipped), QMX+ 160-6 m, G90 160-10 m, X6100/X6200/FT-891 160-6 m.
 * QMX and X6200 entries need Hamlib >= 4.6; on older Hamlib the app falls
 * back to the TS-480 / X6100 backends, which speak the same protocol.
 */
static const radio_profile_t k_radios[] = {
    {"qmx", "QRP Labs QMX", 2057, 2028, 115200, PTT_CAT,
     "QMX", "/dev/ttyACM0", "QMX|QRP", "PKTUSB", 5.0f, BANDS_80_TO_10,
     "Single USB cable: CAT + audio. Set QMX to Digi mode; TX audio level ~50%."},
    {"qmxplus", "QRP Labs QMX+", 2057, 2028, 115200, PTT_CAT,
     "QMX", "/dev/ttyACM0", "QMX|QRP", "PKTUSB", 5.0f, BANDS_ALL,
     "Same CAT/audio as QMX. 160-6 m band coverage."},
    {"g90", "Xiegu G90 + Digirig", 3088, 0, 19200, PTT_CAT,
     "CP210", "/dev/ttyUSB0", "Device|C-Media|USB Audio", "PKTUSB", 20.0f, BANDS_NO6,
     "Digirig CAT cable to G90 COMM port, audio to the head. Turn G90 power down for digital duty cycle."},
    {"x6100", "Xiegu X6100", 3087, 0, 19200, PTT_CAT,
     "1a86|USB_Dual_Serial|CH34|CP210|@if02", "/dev/ttyACM1", "X6100|CODEC|USB Audio", "PKTUSB", 10.0f, BANDS_ALL,
     "Built-in USB CAT + audio. Hamlib default CI-V address 0xA4; set CAT baud 19200 in the radio."},
    {"x6200", "Xiegu X6200", 3091, 3087, 19200, PTT_CAT,
     "1a86|USB_Dual_Serial|CH34|CP210|@if02", "/dev/ttyACM1", "X6200|CODEC|USB Audio", "PKTUSB", 8.0f, BANDS_ALL,
     "Built-in USB CAT + audio (uses Hamlib X6200 backend, X6100 on older Hamlib). CAT baud 19200."},
    {"ft891", "Yaesu FT-891 + Digirig", 1036, 0, 38400, PTT_RTS,
     "CP210", "/dev/ttyUSB0", "Device|C-Media|USB Audio", "PKTUSB", 100.0f, BANDS_ALL,
     "In the radio menu set CAT RATE to 38400, data input to the rear DATA jack and PTT to RTS. Turn power down for QRP."},
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
