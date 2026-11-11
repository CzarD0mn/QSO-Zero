/* End-to-end engine test without hardware:
 *  1. real-time FT8: synthetic audio -> app_audio_in (ring + UTC tagging) ->
 *     decode thread -> receive log
 *  2. FT8 auto-sequencer through a full QSO, including the ADIF log
 *  3. JS8 TX frames -> modem -> decoder -> assembler -> complete message */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "core/app.h"
#include "core/clock.h"
#include "dsp/ft8w.h"
#include "dsp/js8.h"
#include "dsp/js8_varicode.h"
#include "dsp/wspr.h"
#include "dsp/wsprrx.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok    " __VA_ARGS__); else { printf("  FAIL  " __VA_ARGS__); ++fails; } printf("\n"); } while (0)

static double gauss(void)
{
    double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

static int find_line(app_t *a, const char *text, int flags)
{
    for (int i = 0; i < a->nlines; ++i)
        if (strstr(a->lines[i].text, text) && (a->lines[i].flags & flags) == flags) return i;
    return -1;
}

static void realtime_ft8(app_t *a)
{
    printf("[1] real-time FT8 through the audio ring and decode thread\n");
    /* audio covering the NEXT slot: signal starts 0.5 s into it */
    double slot = (floor(clk_now() / 15) + 1) * 15;
    int n = 16 * 12000;
    float *sig = calloc(n, sizeof(float));
    uint8_t tones[79];
    ft8w_encode(FT8W_FT8, "CQ W1AW FN31", tones);
    float *s1 = calloc(n, sizeof(float));
    size_t ns = ft8w_synth(FT8W_FT8, tones, 79, 1234, 12000, 1.0f, s1);
    for (size_t i = 0; i < ns; ++i) sig[6000 + i] = s1[i] * 0.3f;
    int16_t *pcm = malloc(sizeof(int16_t) * n);
    for (int i = 0; i < n; ++i) pcm[i] = (int16_t)((sig[i] + 0.05 * gauss()) * 8000);
    /* stream it like the capture thread does: 20 ms chunks, wall-clock paced */
    double t = slot - 0.5;
    while (clk_now() < t) usleep(10000);
    a->mode = MODE_FT8;
    for (int k = -6000; k < n; k += 240) {
        double due = slot + k / 12000.0;
        while (clk_now() < due + 0.02) usleep(2000);
        int16_t chunk[240];
        for (int i = 0; i < 240; ++i) chunk[i] = (k + i >= 0 && k + i < n) ? pcm[k + i] : (int16_t)(400 * gauss());
        app_audio_in(chunk, 240, due, a);
        if (find_line(a, "CQ W1AW FN31", LINE_CQ) >= 0) break;
    }
    for (int w = 0; w < 30 && find_line(a, "CQ W1AW FN31", LINE_CQ) < 0; ++w) usleep(100000);
    int i = find_line(a, "CQ W1AW FN31", LINE_CQ);
    CHECK(i >= 0, "decoded 'CQ W1AW FN31' from the live ring buffer");
    if (i >= 0) {
        CHECK(fabs(a->lines[i].freq - 1234) < 4, "frequency %.1f Hz (sent 1234)", a->lines[i].freq);
        CHECK(fabs(a->lines[i].dt) < 0.25, "DT %+.2f s (time tagging is consistent)", a->lines[i].dt);
        printf("        decode took %.0f ms on this machine\n", a->last_decode_ms);
    }
    free(sig); free(s1); free(pcm);
}

static rxline_t mk(const char *t, int snr, int odd)
{
    rxline_t l = {.utc = clk_now(), .snr = snr, .freq = 1200, .mode = MODE_FT8, .slot_odd = odd};
    snprintf(l.text, sizeof l.text, "%s", t);
    return l;
}

static void sequencer(app_t *a)
{
    printf("[2] FT8 auto-sequencer\n");
    snprintf(a->cfg.log_path, sizeof a->cfg.log_path, "/tmp/neonqrp_test.adi");
    remove(a->cfg.log_path);
    a->tx_enabled = 0;                 /* keep the TX thread from consuming the queue */
    a->mode = MODE_FT8;
    rxline_t cq = mk("CQ POTA VE3XYZ FN03", -14, 1);
    pthread_mutex_lock(&a->lock);
    seq_start_reply(a, &cq);
    CHECK(a->ntxq == 1 && !strcmp(a->txq[0].text, "VE3XYZ KN4CRD EM73"), "reply to CQ: '%s'", a->txq[0].text);
    CHECK(a->txq[0].slot_odd == 0, "transmits in the opposite (even) slot");
    rxline_t r1 = mk("KN4CRD VE3XYZ -09", -12, 1);
    seq_on_decode(a, &r1);
    CHECK(!strcmp(a->txq[0].text, "VE3XYZ KN4CRD R-14"), "got report -> '%s'", a->txq[0].text);
    rxline_t other = mk("KN4CRD W9ZZZ -03", -5, 1);
    seq_on_decode(a, &other);
    CHECK(!strcmp(a->txq[0].text, "VE3XYZ KN4CRD R-14"), "ignores a different caller mid-QSO");
    rxline_t r2 = mk("KN4CRD VE3XYZ RR73", -11, 1);
    seq_on_decode(a, &r2);
    CHECK(!strcmp(a->txq[0].text, "VE3XYZ KN4CRD 73"), "got RR73 -> '%s'", a->txq[0].text);
    a->ntxq = 0;
    seq_after_cycle(a, 1);
    CHECK(a->seq.st == SEQ_IDLE, "QSO closes after 73");
    /* CQ path */
    seq_start_cq(a);
    CHECK(!strcmp(a->txq[0].text, "CQ KN4CRD EM73"), "CQ: '%s'", a->txq[0].text);
    rxline_t ans = mk("KN4CRD JA1ABC PM95", -17, 1);
    seq_on_decode(a, &ans);
    CHECK(!strcmp(a->txq[0].text, "JA1ABC KN4CRD -17"), "answer to CQ -> '%s'", a->txq[0].text);
    rxline_t rr = mk("KN4CRD JA1ABC R-20", -16, 1);
    seq_on_decode(a, &rr);
    CHECK(!strcmp(a->txq[0].text, "JA1ABC KN4CRD RR73"), "R-report -> '%s'", a->txq[0].text);
    /* retries */
    a->ntxq = 0;
    a->seq.tries = 6;
    seq_after_cycle(a, 1);
    CHECK(a->seq.st == SEQ_IDLE, "gives up after retry limit");
    a->ntxq = 0;
    pthread_mutex_unlock(&a->lock);
    FILE *f = fopen(a->cfg.log_path, "r");
    char buf[2048] = "";
    if (f) { size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); }
    CHECK(strstr(buf, "<call:6>VE3XYZ") && strstr(buf, "<call:6>JA1ABC") && strstr(buf, "<gridsquare:4>PM95"),
          "ADIF log has both QSOs");
    a->tx_enabled = 1;
}

static void js8_path(app_t *a)
{
    printf("[3] JS8: build frames -> modem -> decode -> assemble\n");
    a->mode = MODE_JS8;
    a->js8_sub = JS8_TURBO;
    a->nlines = 0;
    const char *msg = "HELLO FROM THE PARK, QMX AT 5W";
    js8_txframe_t f[16];
    int nf = js8v_build_frames("VE3XYZ", "FN03", "KN4CRD", msg, 0, 0, JS8_TURBO, f, 16);
    js8_decoder_t *d = js8_decoder_new(JS8_TURBO);
    int nsamp = js8_decoder_samples(d);
    int16_t *pcm = malloc(sizeof(int16_t) * nsamp);
    float *sig = malloc(sizeof(float) * nsamp);
    int ok_frames = 0;
    for (int i = 0; i < nf; ++i) {
        uint8_t tones[79];
        js8_encode(JS8_TURBO, f[i].bits, f[i].text, tones);
        memset(sig, 0, sizeof(float) * nsamp);
        size_t ns = js8_synth(JS8_TURBO, tones, 1500, 12000, 0.4f, sig + 1200);
        (void)ns;
        for (int k = 0; k < nsamp; ++k) pcm[k] = (int16_t)((sig[k] + 0.06 * gauss()) * 8000);
        js8_decode_t out[4];
        int nd = js8_decode(d, pcm, nsamp, 200, 2900, 1500, out, 4);
        if (nd == 1 && !strcmp(out[0].text, f[i].text) && out[0].frame_type == f[i].bits) {
            ++ok_frames;
            pthread_mutex_lock(&a->lock);
            js8_assemble(a, out[0].text, out[0].frame_type, out[0].snr, out[0].freq, out[0].dt, clk_now());
            pthread_mutex_unlock(&a->lock);
        }
    }
    CHECK(ok_frames == nf, "%d/%d frames survived the air path", ok_frames, nf);
    int li = a->nlines - 1;
    CHECK(a->nlines == 1, "assembled into one line");
    if (li >= 0) {
        printf("        \"%s\"\n", a->lines[li].text);
        CHECK(strstr(a->lines[li].text, msg) != NULL, "message text intact");
        CHECK(a->lines[li].flags & LINE_TOME, "flagged as addressed to KN4CRD");
        CHECK(!(a->lines[li].flags & LINE_PARTIAL), "marked complete on the LAST frame");
    }
    js8_decoder_free(d);
    free(pcm);
    free(sig);
}


/* ---------------- JS8 auto-replies ---------------- */

/* deliver a message "sent" by `from` through frame building + assembly */
static void rx_js8(app_t *a, const char *from, const char *text, int snr, float freq)
{
    js8_txframe_t f[16];
    int n = js8v_build_frames(from, "FN03", "", text, 0, 0, (js8_submode_t)a->js8_sub, f, 16);
    pthread_mutex_lock(&a->lock);
    for (int i = 0; i < n; ++i) js8_assemble(a, f[i].text, f[i].bits, snr, freq, 0.1f, clk_now());
    pthread_mutex_unlock(&a->lock);
}

/* what we'd put on the air: interpret every queued JS8 frame, then clear */
static void queued(app_t *a, char *out, size_t sz)
{
    out[0] = 0;
    pthread_mutex_lock(&a->lock);
    for (int i = 0; i < a->ntxq; ++i) {
        js8_rxframe_t r;
        js8v_interpret(a->txq[i].text, a->txq[i].bits, &r);
        strncat(out, r.message, sz - strlen(out) - 1);
    }
    a->ntxq = 0;
    pthread_mutex_unlock(&a->lock);
    size_t n = strlen(out);
    while (n && out[n - 1] == ' ') out[--n] = 0;
}

static void autoreply(app_t *a)
{
    char q[512];
    printf("[4] JS8 auto-replies\n");
    a->mode = MODE_JS8;
    a->js8_sub = JS8_NORMAL;
    a->tx_enabled = 1;           /* TX thread not started yet: the queue stays put for inspection */
    a->cfg.js8_autoreply = 0;
    rx_js8(a, "VE3XYZ", "KN4CRD SNR?", -9, 1400);
    queued(a, q, sizeof q);
    CHECK(!q[0], "auto-reply OFF: no response to SNR?");

    a->cfg.js8_autoreply = 1;
    rx_js8(a, "VE3XYZ", "KN4CRD SNR?", -9, 1400);
    queued(a, q, sizeof q);
    CHECK(!strcmp(q, "KN4CRD: VE3XYZ SNR -09"), "SNR?      -> \"%s\"", q);

    rx_js8(a, "VE3XYZ", "KN4CRD GRID?", -9, 1400);
    queued(a, q, sizeof q);
    CHECK(!strcmp(q, "KN4CRD: VE3XYZ GRID EM73"), "GRID?     -> \"%s\"", q);

    rx_js8(a, "VE3XYZ", "KN4CRD INFO?", -9, 1400);
    queued(a, q, sizeof q);
    CHECK(!q[0], "INFO? with no info text set: silent");
    snprintf(a->cfg.js8_info, sizeof a->cfg.js8_info, "QMX 5W EFHW POTA K-1234");
    rx_js8(a, "VE3XYZ", "KN4CRD INFO?", -9, 1400);
    queued(a, q, sizeof q);
    CHECK(strstr(q, "KN4CRD: VE3XYZ INFO") && strstr(q, "QMX 5W EFHW POTA K-1234"), "INFO?     -> \"%s\"", q);

    rx_js8(a, "W1AW", "@HB HEARTBEAT FN31", -15, 900);
    rx_js8(a, "JA1ABC", "@HB HEARTBEAT PM95", -20, 1700);
    queued(a, q, sizeof q);
    CHECK(!q[0], "heartbeats with HB-ACK off: silent");
    rx_js8(a, "VE3XYZ", "KN4CRD HEARING?", -9, 1400);
    queued(a, q, sizeof q);
    CHECK(strstr(q, "VE3XYZ HEARING") && strstr(q, "JA1ABC") && strstr(q, "W1AW") && !strstr(q, "HEARING VE3XYZ"),
          "HEARING?  -> \"%s\"", q);

    snprintf(a->js8_last_tx, sizeof a->js8_last_tx, "VE3XYZ QTH HILLTOP NEAR BOSTON");
    rx_js8(a, "VE3XYZ", "KN4CRD AGN?", -9, 1400);
    queued(a, q, sizeof q);
    CHECK(strstr(q, "QTH HILLTOP NEAR BOSTON") != NULL, "AGN?      -> \"%s\"", q);

    snprintf(a->cfg.inbox_path, sizeof a->cfg.inbox_path, "/tmp/neonqrp_inbox.txt");
    remove(a->cfg.inbox_path);
    rx_js8(a, "VE3XYZ", "KN4CRD MSG MEET ON 7078 AT 1800Z", -9, 1400);
    queued(a, q, sizeof q);
    CHECK(!strcmp(q, "KN4CRD: VE3XYZ ACK"), "MSG (valid checksum) -> \"%s\"", q);
    FILE *f = fopen(a->cfg.inbox_path, "r");
    char ib[256] = "";
    if (f) { if (!fgets(ib, sizeof ib, f)) ib[0] = 0; fclose(f); }
    CHECK(strstr(ib, "VE3XYZ -> KN4CRD: MEET ON 7078 AT 1800Z") != NULL, "MSG stored in inbox without its checksum");
    /* corrupt the checksum: build the frames ourselves with a wrong CRC */
    rx_js8(a, "VE3XYZ", "KN4CRD MSG", -9, 1400);   /* empty body */
    queued(a, q, sizeof q);
    CHECK(!q[0], "MSG with missing/invalid checksum: not ACKed");

    rx_js8(a, "VE3XYZ", "W9ZZZ SNR?", -9, 1400);
    queued(a, q, sizeof q);
    CHECK(!q[0], "query to another station: ignored");

    a->cfg.js8_hb_ack = 1;
    rx_js8(a, "G4ABC", "@HB HEARTBEAT IO91", -17, 2100);
    queued(a, q, sizeof q);
    CHECK(!strcmp(q, "KN4CRD: G4ABC HEARTBEAT SNR -17"), "heartbeat ACK -> \"%s\"", q);
    rx_js8(a, "G4ABC", "@HB HEARTBEAT IO91", -17, 2100);
    queued(a, q, sizeof q);
    CHECK(!q[0], "second heartbeat within 15 min: not ACKed again");

    pthread_mutex_lock(&a->lock);
    a->txq[a->ntxq++] = (txitem_t){.kind = TXK_JS8, .text = "HELLOWORLD12", .slot_odd = -1};
    pthread_mutex_unlock(&a->lock);
    rx_js8(a, "VE3XYZ", "KN4CRD SNR?", -9, 1400);
    pthread_mutex_lock(&a->lock);
    int n = a->ntxq;
    a->ntxq = 0;
    pthread_mutex_unlock(&a->lock);
    CHECK(n == 1, "operator has TX queued: auto-reply holds off");
    a->tx_enabled = 0;
    rx_js8(a, "VE3XYZ", "KN4CRD SNR?", -9, 1400);
    queued(a, q, sizeof q);
    CHECK(!q[0], "TX halted (Ctrl+X): auto-reply stays silent");
    a->cfg.js8_autoreply = 0;
    a->cfg.js8_hb_ack = 0;
    a->tx_enabled = 1;
}

/* ---------------- WSPR receive through the engine ---------------- */

static void wspr_cycle(app_t *a)
{
    printf("[5] WSPR receive cycle through the audio ring and decode thread\n");
    double now = clk_now(), start = floor(now / 120) * 120;
    if (now - start > 100) start += 120;       /* too late in this cycle, use the next */
    a->nlines = 0;
    a->cfg.wspr_rx = 1;
    snprintf(a->cfg.grid, sizeof a->cfg.grid, "EM73");
    int n = 116 * 12000;
    float *mix = calloc(n, sizeof(float)), *w = malloc(sizeof(float) * 162 * 8192);
    const char *calls[] = {"K1ABC", "G4JNT"}, *grids[] = {"FN42", "IO90"};
    float fr[] = {1450, 1540}, snr[] = {-20, -25};
    for (int k = 0; k < 2; ++k) {
        uint8_t sym[162];
        wspr_encode(calls[k], grids[k], 37, sym);
        size_t m = wspr_synth(sym, fr[k], 12000, (float)sqrt(2 * (2500.0 / 6000) * pow(10, snr[k] / 10)), w);
        for (size_t i = 0; i < m; ++i) mix[12000 + i] += w[i];
    }
    int16_t *pcm = malloc(sizeof(int16_t) * n);
    for (int i = 0; i < n; ++i) pcm[i] = (int16_t)fmax(-32768, fmin(32767, (mix[i] + gauss()) * 3000));
    /* load the ring with the whole recording, time-stamped for this cycle */
    a->ring_total = 0;
    a->have_utc0 = 0;
    for (int k = 0; k < n; k += 960) app_audio_in(pcm + k, k + 960 <= n ? 960 : n - k, start + k / 12000.0, a);
    app_set_mode(a, MODE_WSPR);
    printf("        waiting for the decode at %+.0f s ...\n", start + 115 - clk_now());
    for (int i = 0; i < 1400 && a->nlines < 2; ++i) usleep(100000);
    int found = 0;
    for (int i = 0; i < a->nlines; ++i) {
        printf("        %+3d dB %7.1f Hz  %s\n", a->lines[i].snr, a->lines[i].freq, a->lines[i].text);
        if ((strstr(a->lines[i].text, "K1ABC") && strstr(a->lines[i].text, "FN42")) ||
            (strstr(a->lines[i].text, "G4JNT") && strstr(a->lines[i].text, "IO90"))) ++found;
    }
    CHECK(found == 2, "both WSPR spots decoded by the engine (%d)", found);
    CHECK(a->nlines && strstr(a->lines[0].text, "km"), "spots carry distance from my grid");
    CHECK(grid_distance_km("FN42", "IO90") > 5000 && grid_distance_km("FN42", "IO90") < 5600, "FN42->IO90 = %d km", grid_distance_km("FN42", "IO90"));
    free(mix); free(w); free(pcm);
}

int main(void)
{
    static app_t a;
    srand(42);
    app_init(&a, "/nonexistent", 0);
    snprintf(a.cfg.callsign, sizeof a.cfg.callsign, "KN4CRD");
    snprintf(a.cfg.grid, sizeof a.cfg.grid, "EM73");
    a.cfg.auto_grid = 0;
    sequencer(&a);
    js8_path(&a);
    autoreply(&a);
    app_start_threads(&a);
    realtime_ft8(&a);
    wspr_cycle(&a);
    app_stop_threads(&a);
    printf(fails ? "FAIL (%d)\n" : "PASS\n", fails);
    return fails != 0;
}
