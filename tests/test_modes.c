/* FT8 CQ modifiers and JS8 -> APRS (@APRSIS) messages.
 *  1. modifier rule (1-4 letters or 3 digits) and over-the-air round trip
 *  2. the auto-sequencer's CQ carries the modifier
 *  3. APRS: the frames QSO-Zero queues, reassembled the way JS8Call does
 *     (concatenate, strip, check + drop the 16-bit checksum), give exactly
 *     the APRS text JS8Call relays to APRS-IS */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/app.h"
#include "dsp/ft8w.h"
#include "dsp/js8_varicode.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok    " __VA_ARGS__); else { printf("  FAIL  " __VA_ARGS__); ++fails; } printf("\n"); } while (0)

/* encode -> GFSK audio -> decoder; returns the decoded text or "" */
static const char *ft8_roundtrip(const char *text, char *out)
{
    uint8_t tones[79];
    out[0] = 0;
    if (ft8w_encode(FT8W_FT8, text, tones) < 0) return out;
    int n = 15 * 12000;
    float *f = calloc(n, sizeof(float));
    int16_t *pcm = calloc(n, sizeof(int16_t));
    size_t ns = ft8w_synth(FT8W_FT8, tones, 79, 1500, 12000, 1.0f, f + 6000);
    (void)ns;
    for (int i = 0; i < n; ++i) pcm[i] = (int16_t)(f[i] * 8000 + (rand() % 200 - 100));
    ft8w_t *w = ft8w_new(FT8W_FT8, 12000, 200, 3000);
    ft8w_decode_t d[8];
    int nd = ft8w_decode(w, pcm, n, d, 8);
    if (nd > 0) strcpy(out, d[0].text);
    ft8w_free(w);
    free(f); free(pcm);
    return out;
}

/* What JS8Call does with our queued frames: concatenate the text after the
 * directed "@APRSIS CMD" frame, strip, check and remove the checksum. */
static int js8call_view(app_t *a, char *cmd_text, size_t sz)
{
    char msg[256] = "";
    int directed = 0;
    for (int i = 0; i < a->ntxq; ++i) {
        js8_rxframe_t r;
        memset(&r, 0, sizeof r);
        if (!js8v_interpret(a->txq[i].text, a->txq[i].bits, &r)) return -1;
        if (i == 0) {
            if (strcmp(r.to, "@APRSIS") || strcmp(r.cmd, " CMD")) return -2;
            directed = 1;
            continue;
        }
        strncat(msg, r.message, sizeof msg - strlen(msg) - 1);
    }
    if (!directed) return -3;
    char *s = msg;
    while (*s == ' ') ++s;
    size_t L = strlen(s);
    while (L && s[L - 1] == ' ') s[--L] = 0;
    if (L < 4) return -4;
    char sum[4], want[4];
    memcpy(sum, s + L - 3, 3); sum[3] = 0;
    s[L - 4] = 0;                                  /* "<text> <sum>" */
    js8v_checksum16(s, want);
    if (memcmp(sum, want, 3)) return -5;
    snprintf(cmd_text, sz, "%s", s);
    return 0;
}

int main(void)
{
    static app_t a;
    char got[64];
    printf("[1] FT8 CQ modifier rule\n");
    const char *good[] = {"", "DX", "POTA", "SOTA", "QRP", "NA", "WWFF", "290"};
    const char *bad[] = {"PARKS", "QRPP1", "P0TA", "29", "1234", "PO/A", "pota"};
    int okg = 1, okb = 1;
    for (size_t i = 0; i < sizeof good / sizeof *good; ++i) okg &= cq_mod_valid(good[i]);
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) okb &= !cq_mod_valid(bad[i]);
    CHECK(okg, "accepted: DX POTA SOTA QRP NA WWFF 290");
    CHECK(okb, "rejected: PARKS (5), QRPP1, P0TA (mixed), 29, 1234, PO/A");

    printf("[2] over the air (encode -> audio -> decode)\n");
    const char *ota[] = {"CQ POTA KN4CRD EM73", "CQ SOTA KN4CRD EM73", "CQ QRP KN4CRD EM73", "CQ DX KN4CRD EM73", "CQ 290 KN4CRD EM73"};
    for (size_t i = 0; i < sizeof ota / sizeof *ota; ++i) {
        ft8_roundtrip(ota[i], got);
        CHECK(!strcmp(got, ota[i]), "%-20s -> \"%s\"", ota[i], got);
    }
    uint8_t tones[79];
    got[0] = 0;
    int enc = ft8w_encode(FT8W_FT8, "CQ PARKS KN4CRD EM73", tones);
    if (enc >= 0) ft8_roundtrip("CQ PARKS KN4CRD EM73", got);
    CHECK(enc < 0 || strcmp(got, "CQ PARKS KN4CRD EM73"), "5-letter CQ PARKS can't go out as typed (%s)", enc < 0 ? "encoder refuses it" : got);

    printf("[2b] FT4 over the air\n");
    {
        const char *m4[] = {"CQ POTA KN4CRD EM73", "KN4CRD W1AW FN31", "W1AW KN4CRD -07", "KN4CRD W1AW RR73"};
        for (int i = 0; i < 4; ++i) {
            uint8_t t4[105];
            int nt = ft8w_encode(FT8W_FT4, m4[i], t4);
            int n = 75 * 1200;
            float *f = calloc(n, sizeof(float));
            int16_t *pcm = calloc(n, sizeof(int16_t));
            ft8w_synth(FT8W_FT4, t4, nt, 1200, 12000, 1.0f, f + 6000);   /* on time: +0.5 s */
            for (int k = 0; k < n; ++k) pcm[k] = (int16_t)(f[k] * 6000 + (rand() % 400 - 200));
            ft8w_t *w = ft8w_new(FT8W_FT4, 12000, 100, 3000);
            ft8w_decode_t d[8];
            int nd = ft8w_decode(w, pcm, (int)(5.8 * 12000), d, 8);
            CHECK(nt == 105 && nd >= 1 && !strcmp(d[0].text, m4[i]) && fabsf(d[0].dt) < 0.05f,
                  "FT4 %-20s -> \"%s\" dt %+.2f", m4[i], nd ? d[0].text : "", nd ? d[0].dt : 0);
            ft8w_free(w); free(f); free(pcm);
        }
    }

    printf("[3] the auto-sequencer's CQ\n");
    app_init(&a, "/nonexistent", 0);
    snprintf(a.cfg.callsign, sizeof a.cfg.callsign, "KN4CRD");
    snprintf(a.cfg.grid, sizeof a.cfg.grid, "EM73TU");
    a.ntxq = 0;
    CHECK(app_set_cq_mod(&a, " pota ") == 0 && !strcmp(a.cfg.cq_mod, "POTA"), "\" pota \" accepted as POTA");
    CHECK(app_set_cq_mod(&a, "PARKS") != 0 && !strcmp(a.cfg.cq_mod, "POTA"), "PARKS refused, POTA kept: \"%s\"", a.status);
    a.mode = MODE_FT8;
    app_call_cq(&a);
    CHECK(a.ntxq == 1 && !strcmp(a.txq[0].text, "CQ POTA KN4CRD EM73"), "queued \"%s\"", a.ntxq ? a.txq[0].text : "");
    app_halt_tx(&a); a.ntxq = 0;
    app_set_cq_mod(&a, "none");
    app_call_cq(&a);
    CHECK(a.ntxq == 1 && !strcmp(a.txq[0].text, "CQ KN4CRD EM73"), "none -> plain \"%s\"", a.ntxq ? a.txq[0].text : "");
    app_halt_tx(&a); a.ntxq = 0;
    app_set_mode(&a, MODE_FT4);
    app_set_cq_mod(&a, "SOTA");
    app_call_cq(&a);
    CHECK(a.mode == MODE_FT4 && a.ntxq == 1 && !strcmp(a.txq[0].text, "CQ SOTA KN4CRD EM73"), "FT4 CQ uses the modifier too: \"%s\"", a.ntxq ? a.txq[0].text : "");
    CHECK(mode_period(&a) == 7.5, "FT4 period 7.5 s");
    app_halt_tx(&a); a.ntxq = 0;
    app_set_cq_mod(&a, "none");
    app_set_mode(&a, MODE_FT8);

    printf("[3b] switching modes clears the decode list\n");
    {
        rxline_t l = {.snr = -10, .freq = 1200, .mode = MODE_FT8};
        snprintf(l.text, sizeof l.text, "CQ W1AW FN31");
        app_set_mode(&a, MODE_FT8);
        a.nlines = 0;
        app_add_line(&a, &l); app_add_line(&a, &l);
        a.sel = 1;
        app_set_mode(&a, MODE_FT8);
        CHECK(a.nlines == 2 && a.sel == 1, "same mode again (e.g. radio reconnect): list kept");
        app_set_mode(&a, MODE_FT4);
        CHECK(a.nlines == 0 && a.sel == -1, "FT8 -> FT4: list cleared, nothing selected");
        app_add_line(&a, &l);
        app_ghostnet_band(&a, 3);
        CHECK(a.mode == MODE_JS8 && a.nlines == 0, "GHOSTNET from the menu (FT4 -> JS8): cleared");
        a.cfg.ghostnet = 0;
        app_set_mode(&a, MODE_FT8);
    }

    printf("[4] JS8 -> APRS frames, as a JS8Call relay sees them\n");
    app_set_mode(&a, MODE_JS8);
    a.ntxq = 0;
    a.aprs_msgno = 7;
    char t[200];
    CHECK(app_aprs_sms(&a, "5551234567", "Arrived at camp OK") == 0, "SMS queued (%d frames)", a.ntxq);
    int rc = js8call_view(&a, t, sizeof t);
    CHECK(rc == 0 && !strcmp(t, ":SMS      :@5551234567 ARRIVED AT CAMP OK{7"), "SMS  -> \"%s\" (rc %d)", t, rc);
    a.ntxq = 0;
    app_aprs_email(&a, "me@example.com", "On summit W4C-CM-001");
    rc = js8call_view(&a, t, sizeof t);
    CHECK(rc == 0 && !strcmp(t, ":EMAIL-2  :ME@EXAMPLE.COM ON SUMMIT W4C-CM-001{8"), "mail -> \"%s\" (rc %d)", t, rc);
    a.ntxq = 0;
    app_aprs_message(&a, "w1aw-7", "QRV 7.078 JS8");
    rc = js8call_view(&a, t, sizeof t);
    CHECK(rc == 0 && !strcmp(t, ":W1AW-7   :QRV 7.078 JS8{9"), "msg  -> \"%s\" (rc %d)", t, rc);
    a.ntxq = 0;
    app_aprs_position(&a);
    {
        js8_rxframe_t r0, r1;
        memset(&r0, 0, sizeof r0); memset(&r1, 0, sizeof r1);
        int ok = a.ntxq >= 1 && js8v_interpret(a.txq[0].text, a.txq[0].bits, &r0);
        if (a.ntxq >= 2) js8v_interpret(a.txq[1].text, a.txq[1].bits, &r1);
        char all[160];
        snprintf(all, sizeof all, "%s%s", r0.message, a.ntxq >= 2 ? r1.message : "");
        CHECK(ok && !strcmp(r0.to, "@APRSIS") && strstr(all, "GRID") && strstr(all, "EM73TU"), "position -> \"%s\"", all);
    }
    a.ntxq = 0;

    printf("[5] APRS limits\n");
    char longmsg[80];
    memset(longmsg, 'A', 70); longmsg[70] = 0;
    CHECK(app_aprs_message(&a, "W1AW", longmsg) != 0 && a.ntxq == 0, "over 67 characters refused: \"%s\"", a.status);
    CHECK(app_aprs_message(&a, "W1AW", "A|B") != 0 && a.ntxq == 0, "| refused");
    CHECK(app_aprs_email(&a, "notanaddress", "hi") != 0, "email without @ refused");
    CHECK(app_aprs_message(&a, "NOTACALLSIGN1", "hi") != 0, "bad callsign refused");
    app_set_mode(&a, MODE_FT8);
    CHECK(app_aprs_sms(&a, "5551234567", "hi") != 0 && a.ntxq == 0, "refused outside JS8: \"%s\"", a.status);

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}
