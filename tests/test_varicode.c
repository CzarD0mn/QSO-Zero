/* JS8 message layer round-trip tests (build frames -> interpret). */
#include "dsp/js8_varicode.h"
#include <stdio.h>
#include <string.h>

static int fails;

static void show(const char *label, const char *my, const char *sel, const char *text, js8_submode_t sm)
{
    js8_txframe_t f[32];
    int n = js8v_build_frames(my, "FN42", sel, text, 0, 0, sm, f, 32);
    char joined[1024] = "";
    printf("%-10s [%s] \"%s\"\n", label, js8_submode_info(sm)->name, text);
    for (int i = 0; i < n; ++i) {
        js8_rxframe_t r;
        int ok = js8v_interpret(f[i].text, f[i].bits, &r);
        /* also prove the frame survives the modem */
        uint8_t tones[79];
        if (js8_encode(sm, f[i].bits, f[i].text, tones)) { printf("   modem rejected frame!\n"); ++fails; }
        printf("   %s bits=%d kind=%-3d %s\n", f[i].text, f[i].bits, r.kind, ok ? r.message : "<uninterpretable>");
        if (!ok) ++fails;
        strncat(joined, r.message, sizeof joined - strlen(joined) - 1);
    }
    printf("   => \"%s\"\n", joined);
}

static void expect(const char *what, const char *got, const char *want)
{
    int ok = strcmp(got, want) == 0;
    printf("  %-34s %-24s %s\n", what, got, ok ? "ok" : "MISMATCH");
    if (!ok) { printf("    expected \"%s\"\n", want); ++fails; }
}

int main(void)
{
    if (js8v_load_jsc("data/js8.jsc")) { printf("cannot load data/js8.jsc\n"); return 1; }

    printf("== primitives ==\n");
    const char *calls[] = {"KN4CRD", "K1ABC", "W1AW", "AB1CD", "N0P", "9A1AA", "KN4CRD/P", "3DA0XYZ", "@ALLCALL", "@POTA"};
    for (size_t i = 0; i < sizeof calls / sizeof *calls; ++i) {
        int port = 0;
        uint32_t v = js8v_pack_callsign(calls[i], &port);
        char back[24];
        js8v_unpack_callsign(v, port, back);
        char lbl[48];
        snprintf(lbl, sizeof lbl, "callsign %s (%u)", calls[i], v);
        expect(lbl, back, calls[i]);
    }
    const char *grids[] = {"FN42", "EM73", "JO65", "QF56", "AA00", "RR99"};
    for (size_t i = 0; i < sizeof grids / sizeof *grids; ++i) {
        char back[8];
        js8v_unpack_grid(js8v_pack_grid(grids[i]), back);
        char lbl[48];
        snprintf(lbl, sizeof lbl, "grid %s", grids[i]);
        expect(lbl, back, grids[i]);
    }
    {
        char ck[4];
        js8v_checksum16("123456789", ck);
        char lbl[48];
        snprintf(lbl, sizeof lbl, "crc16/kermit(123456789)=0x%04X", js8v_crc16_kermit("123456789", 9));
        expect(lbl, js8v_crc16_kermit("123456789", 9) == 0x2189 ? "0x2189" : "bad", "0x2189");
    }

    printf("\n== message building ==\n");
    show("heartbeat", "KN4CRD", "", "@HB HEARTBEAT FN42", JS8_NORMAL);
    show("cq", "KN4CRD", "", "CQ CQ CQ FN42", JS8_NORMAL);
    show("cq qrp", "W1AW", "", "CQ QRP FN31", JS8_FAST);
    show("snr", "KN4CRD", "", "K1ABC SNR -12", JS8_NORMAL);
    show("query", "KN4CRD", "", "K1ABC SNR?", JS8_NORMAL);
    show("ack", "KN4CRD", "", "K1ABC ACK", JS8_NORMAL);
    show("freetext", "KN4CRD", "K1ABC", "HELLO FROM THE FIELD ON 5 WATTS", JS8_NORMAL);
    show("freetext", "KN4CRD", "K1ABC", "HELLO FROM THE FIELD ON 5 WATTS", JS8_FAST);
    show("msg", "KN4CRD", "", "K1ABC MSG MEET ON 7078 AT 1800Z", JS8_NORMAL);
    show("compound", "KN4CRD/P", "", "K1ABC ACK", JS8_NORMAL);
    show("compound2", "KN4CRD", "", "VE3/K1ABC ACK", JS8_NORMAL);
    show("allcall", "KN4CRD", "", "@ALLCALL QRV FOR POTA K-1234", JS8_TURBO);
    show("grid", "KN4CRD", "", "K1ABC GRID FN42", JS8_NORMAL);

    printf("\n== free-text round trip (all submodes) ==\n");
    const char *texts[] = {"THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 1234567890",
                           "QTH IS A HILLTOP NEAR BOSTON, ANT IS AN EFHW AT 30 FT. 73!",
                           "TEST"};
    for (int sm = 0; sm < 4; ++sm)
        for (size_t t = 0; t < sizeof texts / sizeof *texts; ++t) {
            js8_txframe_t f[32];
            int n = js8v_build_frames("KN4CRD", "FN42", "", texts[t], 0, 1, (js8_submode_t)sm, f, 32);
            char joined[1024] = "";
            for (int i = 0; i < n; ++i) {
                js8_rxframe_t r;
                js8v_interpret(f[i].text, f[i].bits, &r);
                strncat(joined, r.message, sizeof joined - strlen(joined) - 1);
            }
            char lbl[64];
            snprintf(lbl, sizeof lbl, "%s %d frames", js8_submode_info(sm)->name, n);
            expect(lbl, strcmp(joined, texts[t]) ? joined : "identical", "identical");
        }

    js8v_unload_jsc();
    printf(fails ? "\nFAIL (%d)\n" : "\nPASS\n", fails);
    return fails != 0;
}
