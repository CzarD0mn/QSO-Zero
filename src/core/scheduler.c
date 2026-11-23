/* QSO-Zero - QSO logic: FT8/FT4 auto-sequencer, JS8 message assembly, ADIF log.
 * All functions here are called with a->lock held. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "app.h"
#include "core/clock.h"
#include "dsp/js8.h"
#include "dsp/js8_varicode.h"
#include "core/log.h"

#define FT8_MAX_TRIES 6
#define CQ_MAX_TRIES  10

static int is_grid4(const char *s)
{
    return strlen(s) == 4 && s[0] >= 'A' && s[0] <= 'R' && s[1] >= 'A' && s[1] <= 'R' &&
           isdigit((unsigned char)s[2]) && isdigit((unsigned char)s[3]) && strcmp(s, "RR73");
}
static int is_report(const char *s, int *v)
{
    if ((s[0] == '-' || s[0] == '+') && isdigit((unsigned char)s[1])) { *v = atoi(s); return 1; }
    return 0;
}

static int split(const char *text, char tok[6][16])
{
    int n = 0;
    const char *p = text;
    while (*p && n < 6) {
        while (*p == ' ') ++p;
        if (!*p) break;
        int k = 0;
        while (*p && *p != ' ' && k < 15) tok[n][k++] = *p++;
        tok[n][k] = 0;
        while (*p && *p != ' ') ++p;
        ++n;
    }
    return n;
}

static void fmt_report(int snr, char *out)
{
    if (snr < -30) snr = -30;
    if (snr > 30) snr = 30;
    sprintf(out, "%c%02d", snr < 0 ? '-' : '+', abs(snr));
}

static void queue_ft8(app_t *a, const char *msg)
{
    snprintf(a->seq.last_tx, sizeof a->seq.last_tx, "%s", msg);
    a->ntxq = 0;
    txitem_t t = {.kind = TXK_FT8, .slot_odd = a->seq.slot_odd};
    snprintf(t.text, sizeof t.text, "%s", msg);
    a->txq[a->ntxq++] = t;
    a->seq.tries = 0;
    a->dirty = 1;
}

void seq_reset(app_t *a)
{
    memset(&a->seq, 0, sizeof a->seq);
}

void seq_start_cq(app_t *a)
{
    seq_reset(a);
    a->seq.st = SEQ_CQ;
    a->seq.slot_odd = 0;
    char m[40];
    if (a->cfg.cq_mod[0]) snprintf(m, sizeof m, "CQ %s %s %.4s", a->cfg.cq_mod, a->cfg.callsign, a->cfg.grid);
    else snprintf(m, sizeof m, "CQ %s %.4s", a->cfg.callsign, a->cfg.grid);
    queue_ft8(a, m);
    app_status(a, "calling CQ%s%s (even slots)", a->cfg.cq_mod[0] ? " " : "", a->cfg.cq_mod);
}

static void log_once(app_t *a)
{
    ft8seq_t *s = &a->seq;
    if (s->logged) { qlog("seq", "%s: already logged, not logging again", s->dx); return; }
    adif_log(a, s->dx, s->dxgrid, mode_name(a->mode), s->snr_sent, s->snr_rcvd);
    s->logged = 1;
}

/* Advance the QSO from a message addressed to us by the current DX. */
static void advance(app_t *a, const char *third, int their_snr_of_us, int rx_snr)
{
    char m[40], rep[8];
    int v;
    ft8seq_t *s = &a->seq;
    if (is_grid4(third) || !third[0]) {               /* they answered our CQ */
        snprintf(s->dxgrid, sizeof s->dxgrid, "%s", third);
        s->snr_sent = rx_snr;
        fmt_report(rx_snr, rep);
        snprintf(m, sizeof m, "%s %s %s", s->dx, a->cfg.callsign, rep);
        s->st = SEQ_REPORT;
        queue_ft8(a, m);
    } else if (is_report(third, &v)) {                /* they sent our report */
        s->snr_rcvd = v;
        if (!s->snr_sent) s->snr_sent = rx_snr;
        fmt_report(s->snr_sent, rep);
        snprintf(m, sizeof m, "%s %s R%s", s->dx, a->cfg.callsign, rep);
        s->st = SEQ_RREPORT;
        queue_ft8(a, m);
    } else if (third[0] == 'R' && is_report(third + 1, &v)) {   /* R-report */
        /* answer with RR73; a repeated R-report means they missed our RR73,
         * so it goes out again, but the QSO is only logged once */
        s->snr_rcvd = v;
        snprintf(m, sizeof m, "%s %s RR73", s->dx, a->cfg.callsign);
        s->st = SEQ_RR73;
        queue_ft8(a, m);
        log_once(a);
    } else if (!strcmp(third, "RR73") || !strcmp(third, "RRR")) {
        snprintf(m, sizeof m, "%s %s 73", s->dx, a->cfg.callsign);
        s->st = SEQ_73;
        queue_ft8(a, m);
        log_once(a);
    } else if (!strcmp(third, "73")) {
        app_status(a, "QSO with %s complete", s->dx);
        seq_reset(a);
        a->ntxq = 0;
    }
    (void)their_snr_of_us;
}

void seq_on_decode(app_t *a, const rxline_t *l)
{
    char t[6][16];
    int n = split(l->text, t);
    if (n < 2 || strcmp(t[0], a->cfg.callsign)) return;
    ft8seq_t *s = &a->seq;
    static const char *stn[] = {"idle", "CQ", "calling", "report", "R-report", "RR73", "73"};
    if (s->st == SEQ_IDLE) {                           /* never answer unattended */
        qlog("seq", "heard \"%s\" for us, but no QSO is in progress: not answering (select the line and press ENTER)", l->text);
        return;
    }
    if (s->st == SEQ_CQ) {                              /* first caller wins */
        snprintf(s->dx, sizeof s->dx, "%s", t[1]);
        s->slot_odd = !l->slot_odd;
        app_status(a, "%s answered", s->dx);
    } else if (strcmp(t[1], s->dx)) {
        qlog("seq", "heard \"%s\": from %s, but our QSO is with %s: ignored", l->text, t[1], s->dx);
        return;
    }
    seq_state_t was = s->st;
    advance(a, n >= 3 ? t[2] : "", 0, l->snr);
    qlog("seq", "heard \"%s\" (%+d dB, %s slot): %s -> %s, next \"%s\" in the %s slot", l->text, l->snr,
         l->slot_odd ? "odd" : "even", stn[was], stn[s->st], s->st == SEQ_IDLE ? "-" : s->last_tx, s->slot_odd ? "odd" : "even");
}

/* The operator typed a standard message to a station ("W1AW KN4CRD EM73",
 * "W1AW KN4CRD -12", ...): run the QSO with them from here on, in the slot
 * opposite the one they were last heard in. Returns 1 if it took over. */
int seq_start_typed(app_t *a, const char *text)
{
    char t[6][16];
    int n = split(text, t), v;
    if (n < 2 || n > 3 || strcmp(t[1], a->cfg.callsign) || !strcmp(t[0], "CQ") || t[0][0] == '<') return 0;
    const char *third = n == 3 ? t[2] : "";
    seq_state_t st;
    if (!third[0] || is_grid4(third)) st = SEQ_REPLYING;
    else if (is_report(third, &v)) st = SEQ_REPORT;
    else if (third[0] == 'R' && is_report(third + 1, &v)) st = SEQ_RREPORT;
    else if (!strcmp(third, "RR73") || !strcmp(third, "RRR")) st = SEQ_RR73;
    else if (!strcmp(third, "73")) st = SEQ_73;
    else return 0;
    int odd = -1, snr = 0;
    for (int i = a->nlines - 1; i >= 0 && i >= a->nlines - 200; --i) {
        const rxline_t *l = &a->lines[i];
        if (l->flags & (LINE_TX | LINE_SYS) || !mode_is_ft(l->mode)) continue;
        char u[6][16];
        int m = split(l->text, u);
        for (int k = 0; k < m; ++k)
            if (!strcmp(u[k], t[0])) { odd = l->slot_odd; snr = l->snr; break; }
        if (odd >= 0) break;
    }
    if (odd < 0) return 0;                              /* never heard them: plain queued message */
    if (strcmp(a->seq.dx, t[0])) seq_reset(a);
    ft8seq_t *s = &a->seq;
    snprintf(s->dx, sizeof s->dx, "%s", t[0]);
    s->slot_odd = !odd;
    if (!s->snr_sent) s->snr_sent = snr;
    if (st == SEQ_REPORT && is_report(third, &v)) s->snr_sent = v;
    if (st == SEQ_RREPORT && is_report(third + 1, &v)) s->snr_sent = v;
    s->st = st;
    queue_ft8(a, text);
    qlog("seq", "typed \"%s\": running the QSO with %s, our slot %s", text, s->dx, s->slot_odd ? "odd" : "even");
    app_status(a, "calling %s (%s slots)", s->dx, s->slot_odd ? "odd" : "even");
    return 1;
}

void seq_start_reply(app_t *a, const rxline_t *l)
{
    char t[6][16], m[40];
    int n = split(l->text, t);
    if (n < 2) return;
    seq_reset(a);
    ft8seq_t *s = &a->seq;
    s->slot_odd = !l->slot_odd;
    s->snr_sent = l->snr;
    if (!strcmp(t[0], "CQ")) {
        int gi = is_grid4(t[n - 1]) ? n - 1 : -1;
        snprintf(s->dx, sizeof s->dx, "%s", t[gi > 0 ? n - 2 : n - 1]);
        if (gi > 0) snprintf(s->dxgrid, sizeof s->dxgrid, "%s", t[gi]);
    } else if (!strcmp(t[0], a->cfg.callsign)) {
        snprintf(s->dx, sizeof s->dx, "%s", t[1]);
        s->st = SEQ_REPLYING;
        advance(a, n >= 3 ? t[2] : "", 0, l->snr);
        return;
    } else {
        snprintf(s->dx, sizeof s->dx, "%s", t[1]);    /* tail-end the sender */
        if (n >= 3 && is_grid4(t[2])) snprintf(s->dxgrid, sizeof s->dxgrid, "%s", t[2]);
    }
    if (s->dx[0] == '<') { app_status(a, "can't reply to a hashed call"); seq_reset(a); return; }
    s->st = SEQ_REPLYING;
    snprintf(m, sizeof m, "%s %s %.4s", s->dx, a->cfg.callsign, a->cfg.grid);
    queue_ft8(a, m);
    qlog("seq", "calling %s with \"%s\" in the %s slot", s->dx, m, s->slot_odd ? "odd" : "even");
    app_status(a, "calling %s (%s slots)", s->dx, s->slot_odd ? "odd" : "even");
}

/* Called after each FT8 receive cycle: repeat the last message if the QSO
 * didn't advance, give up after a few tries. */
void seq_after_cycle(app_t *a, int rx_slot_odd)
{
    ft8seq_t *s = &a->seq;
    if (s->st == SEQ_IDLE || a->ntxq) return;
    if (rx_slot_odd == s->slot_odd) return;           /* that was our own slot */
    /* RR73 and 73 both end the QSO: once ours has gone out, nothing is
     * repeated. (If they repeat their R-report or RR73, advance() answers it
     * once more, before this runs, so that reply is still sent.) */
    if (s->st == SEQ_73 || s->st == SEQ_RR73) {
        app_status(a, "QSO with %s complete%s", s->dx, s->logged ? " (logged)" : "");
        qlog("seq", "QSO with %s complete after our %s: nothing more to send", s->dx, s->st == SEQ_73 ? "73" : "RR73");
        seq_reset(a);
        return;
    }
    int limit = s->st == SEQ_CQ ? CQ_MAX_TRIES : FT8_MAX_TRIES;
    if (s->tries >= limit) {
        app_status(a, s->st == SEQ_CQ ? "CQ: no answer" : "%s: no reply, giving up", s->dx);
        qlog("seq", "%s after %d tries", s->st == SEQ_CQ ? "CQ: no answer" : "no reply: giving up", s->tries);
        seq_reset(a);
        return;
    }
    txitem_t t = {.kind = TXK_FT8, .slot_odd = s->slot_odd};
    snprintf(t.text, sizeof t.text, "%s", s->last_tx);
    a->txq[a->ntxq++] = t;
    qlog("seq", "no answer from %s yet: repeating \"%s\" (try %d of %d)", s->dx[0] ? s->dx : "anyone", s->last_tx, s->tries + 1, limit);
}

/* ------------------------------------------------------------------ */
/* JS8 assembly: frames from the same audio frequency are joined into one
 * line until a frame carrying the LAST flag arrives. */

void js8_assemble(app_t *a, const char frame[13], int bits, int snr, float freq, float dt, double utc)
{
    js8_rxframe_t r;
    if (!js8v_interpret(frame, bits, &r)) return;
    const js8_submode_info_t *in = js8_submode_info((js8_submode_t)a->js8_sub);
    float tol = in->tone_spacing * 4;
    int idx = -1;
    for (int i = a->nlines - 1; i >= 0 && i >= a->nlines - 40; --i) {
        rxline_t *l = &a->lines[i];
        if (l->mode == MODE_JS8 && (l->flags & LINE_PARTIAL) && fabsf(l->freq - freq) <= tol) { idx = i; break; }
    }
    if (bits & JS8_TX_FIRST) idx = -1;
    if (idx < 0) {
        rxline_t l = {.utc = utc, .snr = snr, .dt = dt, .freq = freq, .mode = MODE_JS8};
        snprintf(l.text, sizeof l.text, "%s", r.message);
        if (!(bits & JS8_TX_LAST)) l.flags |= LINE_PARTIAL;
        if (r.is_heartbeat) l.flags |= r.is_cq ? LINE_CQ : LINE_HB;
        app_add_line(a, &l);
        idx = a->nlines - 1;
    } else {
        rxline_t *l = &a->lines[idx];
        strncat(l->text, r.message, sizeof l->text - strlen(l->text) - 1);
        l->snr = snr > l->snr ? snr : l->snr;
        l->utc = utc;
        if (bits & JS8_TX_LAST) l->flags &= ~LINE_PARTIAL;
        a->dirty = 1;
    }
    rxline_t *l = &a->lines[idx];
    const char *colon = strchr(l->text, ':');
    size_t cl = strlen(a->cfg.callsign);
    if (colon) {
        const char *p = colon + 1;
        while (*p == ' ') ++p;
        if (!strncmp(p, a->cfg.callsign, cl) && (p[cl] == ' ' || p[cl] == 0)) l->flags |= LINE_TOME;
        char from[16] = "";
        size_t fl = (size_t)(colon - l->text);
        if (fl < sizeof from && fl > 0) { memcpy(from, l->text, fl); from[fl] = 0; js8_heard(a, from, snr, utc); }
    }
    if (bits & JS8_TX_LAST) js8_auto_on_message(a, l);
}

/* ------------------------------------------------------------------ */

void adif_log(app_t *a, const char *call, const char *grid, const char *mode, int sent, int rcvd)
{
    log_dir_make(a->cfg.log_path);                       /* ~/qso-zero/ made if missing */
    FILE *f = fopen(a->cfg.log_path, "a");
    if (!f) { app_status(a, "log: cannot open %s", a->cfg.log_path); return; }
    if (ftell(f) == 0) fprintf(f, "QSO-Zero ADIF export\n<adif_ver:5>3.1.4 <programid:7>QSO-Zero <eoh>\n");
    time_t t = (time_t)clk_now();
    struct tm tm;
    gmtime_r(&t, &tm);
    int nb;
    const band_t *bt = band_table(&nb);
    char fr[16], rs[8], rr[8];
    snprintf(fr, sizeof fr, "%.6f", a->dial_hz / 1e6);
    snprintf(rs, sizeof rs, "%+03d", sent);
    snprintf(rr, sizeof rr, "%+03d", rcvd);
#define ADF(tag, v) fprintf(f, "<%s:%zu>%s ", tag, strlen(v), v)
    ADF("call", call);
    if (grid && *grid) ADF("gridsquare", grid);
    if (!strcmp(mode, "FT4")) { ADF("mode", "MFSK"); ADF("submode", "FT4"); }   /* ADIF 3.1: FT4 is a submode of MFSK */
    else ADF("mode", mode);
    ADF("rst_sent", rs);
    ADF("rst_rcvd", rr);
    char d[16], h[16];
    strftime(d, sizeof d, "%Y%m%d", &tm);
    strftime(h, sizeof h, "%H%M%S", &tm);
    ADF("qso_date", d);
    ADF("time_on", h);
    ADF("freq", fr);
    ADF("band", bt[a->band].name);
    ADF("station_callsign", a->cfg.callsign);
    ADF("my_gridsquare", a->cfg.grid);
#undef ADF
    fprintf(f, "<eor>\n");
    fclose(f);
    app_status(a, "logged %s", call);
}
