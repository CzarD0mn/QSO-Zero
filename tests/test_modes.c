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
#include <unistd.h>
#include <sys/stat.h>
#include "core/app.h"
#include "dsp/ft8w.h"
#include "dsp/js8_varicode.h"
#include "ui/input.h"
#include "ui/views.h"

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

    printf("[6] ADIF log location\n");
    {
        char home[] = "/tmp/qsoz-adifXXXXXX", p[400], cfg[300], q[300];
        if (!mkdtemp(home)) return 1;
        setenv("HOME", home, 1);
        static app_t b;
        memset(&b, 0, sizeof b);
        app_init(&b, "/nonexistent", 0);
        snprintf(p, sizeof p, "%s/qso-zero/qso-zero.adi", home);
        struct stat st;
        snprintf(q, sizeof q, "%s/qso-zero", home);
        CHECK(!strcmp(b.cfg.log_path, p) && stat(q, &st) == 0 && S_ISDIR(st.st_mode), "default %s, folder created", b.cfg.log_path);
        snprintf(b.cfg.callsign, sizeof b.cfg.callsign, "KM4FLG");
        b.mode = MODE_FT8;
        adif_log(&b, "W1AW", "FN31", "FT8", -10, -12);
        FILE *f = fopen(p, "r");
        char buf[600] = "";
        size_t nr = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
        buf[nr] = 0;
        if (f) fclose(f);
        CHECK(strstr(buf, "<call:4>W1AW") && strstr(buf, "<eoh>"), "contact written there");
        app_shutdown(&b);

        /* Save settings doesn't pin the default path */
        snprintf(cfg, sizeof cfg, "%s/saved.conf", home);
        config_save(&b.cfg, cfg);
        f = fopen(cfg, "r");
        nr = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
        buf[nr] = 0;
        if (f) fclose(f);
        CHECK(nr > 0 && !strstr(buf, "log_path"), "Save settings leaves the default log path out of the config");

        /* JS8 inbox: ~/qso-zero/js8/qso-zero-inbox.txt, folder made at start, default not saved */
        snprintf(p, sizeof p, "%s/qso-zero/js8/qso-zero-inbox.txt", home);
        snprintf(q, sizeof q, "%s/qso-zero/js8", home);
        CHECK(!strcmp(b.cfg.inbox_path, p) && stat(q, &st) == 0 && S_ISDIR(st.st_mode), "JS8 inbox %s, folder created", b.cfg.inbox_path);
        CHECK(!strstr(buf, "inbox_path"), "Save settings leaves the default inbox path out of the config");
        {   /* settings saved with the previous default, and a message stored there */
            char prev[300], c2[300];
            snprintf(prev, sizeof prev, "%s/qso-zero-inbox.txt", home);
            FILE *o = fopen(prev, "w"); fprintf(o, "2026-11-22 20:00:00Z  W1AW -> KM4FLG: TEST\n"); fclose(o);
            snprintf(c2, sizeof c2, "%s/old.conf", home);
            o = fopen(c2, "w"); fprintf(o, "inbox_path = %s\n", prev); fclose(o);
            static app_t d;
            memset(&d, 0, sizeof d);
            app_init(&d, c2, 0);
            o = fopen(p, "r");
            char ib[200] = "";
            if (o) { if (!fgets(ib, sizeof ib, o)) ib[0] = 0; fclose(o); }
            CHECK(!strcmp(d.cfg.inbox_path, p) && strstr(ib, "W1AW") && access(prev, F_OK) != 0,
                  "old default inbox setting -> ~/qso-zero/js8/, earlier messages moved there");
            app_shutdown(&d);
        }

        /* a custom path is kept (and saved), and its folder made */
        f = fopen(cfg, "w"); fprintf(f, "log_path = %s/logs/field/my.adi\n", home); fclose(f);
        memset(&b, 0, sizeof b);
        app_init(&b, cfg, 0);
        snprintf(q, sizeof q, "%s/logs/field", home);
        CHECK(strstr(b.cfg.log_path, "/logs/field/my.adi") && stat(q, &st) == 0, "custom log_path kept, folder created");
        app_shutdown(&b);
        snprintf(q, sizeof q, "rm -rf %s", home); if (system(q)) {}
    }

    printf("[7] CQ-only filter (/cqonly)\n");
    {
        static app_t c;
        app_init(&c, "/nonexistent", 0);
        snprintf(c.cfg.callsign, sizeof c.cfg.callsign, "KM4FLG");
        c.mode = MODE_FT8;
        const struct { const char *t; int f; } L[] = {
            {"CQ W1AW FN31", LINE_CQ}, {"K1ABC W9XYZ R-08", 0}, {"CQ POTA N7LD DM43", LINE_CQ},
            {"W2XX K3YY -12", 0}, {"KM4FLG KJ5DZV -03", LINE_TOME}, {"KJ5DZV KM4FLG R-12", LINE_TX}, {"DL1XX EA8ZZ RR73", 0}};
        for (int i = 0; i < 7; ++i) {
            rxline_t l = {.mode = MODE_FT8, .flags = L[i].f};
            snprintf(l.text, sizeof l.text, "%s", L[i].t);
            app_add_line(&c, &l);
        }
        int shown = 0;
        for (int i = 0; i < c.nlines; ++i) shown += line_shown(&c, &c.lines[i]);
        CHECK(shown == 7, "filter off: all 7 listed");
        snprintf(c.input, sizeof c.input, "/cqonly"); app_submit_input(&c);
        shown = 0;
        for (int i = 0; i < c.nlines; ++i) shown += line_shown(&c, &c.lines[i]);
        CHECK(c.cfg.cq_only && shown == 4, "/cqonly: 2 CQs + the reply to us + our TX listed (%d)", shown);
        /* up/down only visit listed lines */
        keyev_t up = {.key = K_UP}, dn = {.key = K_DOWN};
        int path[6], np = 0;
        c.sel = -1;
        for (int k = 0; k < 5; ++k) { ui_key(&c, &up); path[np++] = c.sel; }
        CHECK(path[0] == 5 && path[1] == 4 && path[2] == 2 && path[3] == 0 && path[4] == 0, "up walks 5,4,2,0 (skips hidden) [%d %d %d %d %d]", path[0], path[1], path[2], path[3], path[4]);
        ui_key(&c, &dn); int d1 = c.sel; ui_key(&c, &dn); int d2 = c.sel; ui_key(&c, &dn); ui_key(&c, &dn); int d4 = c.sel;
        CHECK(d1 == 2 && d2 == 4 && d4 == -1, "down walks 2,4 then back to live (%d %d %d)", d1, d2, d4);
        snprintf(c.input, sizeof c.input, "/filter"); app_submit_input(&c);
        shown = 0;
        for (int i = 0; i < c.nlines; ++i) shown += line_shown(&c, &c.lines[i]);
        CHECK(!c.cfg.cq_only && shown == 7, "/filter toggles it off: earlier non-CQ decodes are back (%d)", shown);
        snprintf(c.input, sizeof c.input, "/cqonly on"); app_submit_input(&c);
        c.mode = MODE_JS8;
        rxline_t j = {.mode = MODE_JS8};
        snprintf(j.text, sizeof j.text, "W1AW: KM4FLG HELLO");
        CHECK(line_shown(&c, &j), "JS8 is never filtered");
    }

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}
