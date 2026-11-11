/* NeonQRP - JS8 automatic replies, following JS8Call's behaviour and wording
 * (mainwindow.cpp processCommandActivity). Called with a->lock held, once a
 * message is complete (its LAST frame arrived).
 *
 * Only messages addressed to our callsign are answered (heartbeat ACKs are
 * the exception: they answer @HB/@ALLCALL heartbeats, rate-limited per
 * station). Nothing is sent while we already have something queued or are
 * transmitting, so an auto-reply never interrupts the operator. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "app.h"
#include "core/clock.h"
#include "dsp/js8_varicode.h"

#define HB_ACK_HOLDOFF (15 * 60)   /* JS8Call: no repeat ACK to the same station for 15 min */
#define HEARD_AGE      (60 * 60)

void js8_heard(app_t *a, const char *call, int snr, double utc)
{
    if (!call[0] || call[0] == '<' || !strcmp(call, a->cfg.callsign)) return;
    int i;
    for (i = 0; i < a->nheard; ++i)
        if (!strcmp(a->heard[i].call, call)) break;
    if (i == a->nheard) {
        if (a->nheard == MAX_HEARD) {          /* evict the oldest */
            int old = 0;
            for (int k = 1; k < a->nheard; ++k) if (a->heard[k].utc < a->heard[old].utc) old = k;
            i = old;
        } else a->nheard++;
        memset(&a->heard[i], 0, sizeof a->heard[i]);
        snprintf(a->heard[i].call, sizeof a->heard[i].call, "%s", call);
    }
    a->heard[i].utc = utc;
    a->heard[i].snr = snr;
}

static heard_t *heard_find(app_t *a, const char *call)
{
    for (int i = 0; i < a->nheard; ++i)
        if (!strcmp(a->heard[i].call, call)) return &a->heard[i];
    return NULL;
}

static void fmt_snr(int snr, char *out)
{
    if (snr < -60) snr = -60;
    if (snr > 60) snr = 60;
    sprintf(out, "%c%02d", snr < 0 ? '-' : '+', abs(snr));
}

static void note(app_t *a, const char *fmt, const char *arg)
{
    rxline_t l = {.utc = clk_now(), .mode = MODE_JS8, .flags = LINE_SYS, .freq = (float)a->tx_off};
    snprintf(l.text, sizeof l.text, fmt, arg);
    app_add_line(a, &l);
}

static void reply(app_t *a, const char *text)
{
    app_queue_js8_locked(a, text);
    a->autoreplies++;
    note(a, "AUTO-REPLY queued: %s", text);
}

/* "VE3XYZ: KN4CRD MSG HELLO THERE 4Q7" -> from, to, rest (after "TO ") */
static int parse_directed(const char *text, char *from, char *to, const char **rest)
{
    const char *colon = strchr(text, ':');
    if (!colon || colon == text || colon - text > 15) return 0;
    memcpy(from, text, (size_t)(colon - text));
    from[colon - text] = 0;
    const char *p = colon + 1;
    while (*p == ' ') ++p;
    int n = 0;
    while (*p && *p != ' ' && n < 15) to[n++] = *p++;
    to[n] = 0;
    *rest = p;
    return n > 0;
}

/* rest begins with " CMD..." exactly as varicode emits it */
static int cmd_is(const char *rest, const char *cmd)
{
    size_t L = strlen(cmd);
    return !strncmp(rest, cmd, L) && (rest[L] == ' ' || rest[L] == 0);
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    char *p = s;
    while (*p == ' ') ++p;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

/* MSG bodies end with a 3-char CRC-16 checksum: verify and strip it */
static int check_msg(char *body)
{
    trim(body);
    size_t n = strlen(body);
    if (n < 5 || body[n - 4] != ' ') return 0;
    char ck[4];
    body[n - 4] = 0;
    js8v_checksum16(body, ck);
    return !strcmp(ck, body + n - 3);
}

static void store_inbox(app_t *a, const char *from, const char *msg)
{
    FILE *f = fopen(a->cfg.inbox_path, "a");
    if (!f) return;
    time_t t = (time_t)clk_now();
    struct tm tm;
    gmtime_r(&t, &tm);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%SZ", &tm);
    fprintf(f, "%s  %s -> %s: %s\n", ts, from, a->cfg.callsign, msg);
    fclose(f);
}

void js8_auto_on_message(app_t *a, const rxline_t *line)
{
    rxline_t copy = *line;            /* note() may shift a->lines under us */
    const rxline_t *l = &copy;
    if (l->flags & (LINE_TX | LINE_SYS | LINE_PARTIAL)) return;
    char from[16] = "", to[16] = "", buf[200];
    const char *rest = "";
    if (!parse_directed(l->text, from, to, &rest)) return;
    if (!a->cfg.js8_autoreply || a->mode != MODE_JS8) return;
    if (from[0] == '<' || !strcmp(from, a->cfg.callsign)) return;
    if (a->ntxq || a->tx_active || !a->tx_enabled) return;    /* never interrupt the operator */

    int to_me = !strcmp(to, a->cfg.callsign);
    char snr[8];
    fmt_snr(l->snr, snr);

    /* heartbeat acknowledgement */
    if ((l->flags & LINE_HB) && (!strcmp(to, "@HB") || !strcmp(to, "@ALLCALL"))) {
        if (!a->cfg.js8_hb_ack) return;
        heard_t *h = heard_find(a, from);
        double now = clk_now();
        if (h && h->last_hb_ack > 0 && now - h->last_hb_ack < HB_ACK_HOLDOFF) return;
        if (h) h->last_hb_ack = now;
        snprintf(buf, sizeof buf, "%s HEARTBEAT SNR %s", from, snr);
        reply(a, buf);
        return;
    }
    if (!to_me) return;

    if (cmd_is(rest, " SNR?")) {
        snprintf(buf, sizeof buf, "%s SNR %s", from, snr);
        reply(a, buf);
    } else if (cmd_is(rest, " GRID?")) {
        if (!a->cfg.grid[0]) return;
        snprintf(buf, sizeof buf, "%s GRID %s", from, a->cfg.grid);
        reply(a, buf);
    } else if (cmd_is(rest, " INFO?")) {
        if (!a->cfg.js8_info[0]) return;
        snprintf(buf, sizeof buf, "%s INFO %s", from, a->cfg.js8_info);
        reply(a, buf);
    } else if (cmd_is(rest, " STATUS?")) {
        if (!a->cfg.js8_status[0]) return;
        snprintf(buf, sizeof buf, "%s STATUS %s", from, a->cfg.js8_status);
        reply(a, buf);
    } else if (cmd_is(rest, " HEARING?")) {
        /* up to 4 most recently heard stations, newest first, not the asker */
        int n = snprintf(buf, sizeof buf, "%s HEARING", from), used[MAX_HEARD] = {0}, cnt = 0;
        double now = clk_now();
        while (cnt < 4) {
            int best = -1;
            for (int i = 0; i < a->nheard; ++i) {
                if (used[i] || !strcmp(a->heard[i].call, from) || a->heard[i].call[0] == '@') continue;
                if (now - a->heard[i].utc > HEARD_AGE) continue;
                if (best < 0 || a->heard[i].utc > a->heard[best].utc) best = i;
            }
            if (best < 0) break;
            used[best] = 1;
            n += snprintf(buf + n, sizeof buf - n, " %s", a->heard[best].call);
            ++cnt;
        }
        if (!cnt) return;   /* JS8Call sends nothing useful with an empty list either */
        reply(a, buf);
    } else if (cmd_is(rest, " AGN?")) {
        if (!a->js8_last_tx[0]) return;
        snprintf(buf, sizeof buf, "%s", a->js8_last_tx);
        reply(a, buf);
    } else if (cmd_is(rest, " MSG")) {
        char body[200];
        snprintf(body, sizeof body, "%s", rest + 4);
        if (!check_msg(body)) { note(a, "MSG from %s failed checksum: not acknowledged", from); return; }
        store_inbox(a, from, body);
        note(a, "MSG stored in inbox from %s", from);
        snprintf(buf, sizeof buf, "%s ACK", from);
        reply(a, buf);
    } else if (cmd_is(rest, " QUERY MSGS")) {
        snprintf(buf, sizeof buf, "%s NO", from);   /* we don't hold messages for others */
        reply(a, buf);
    }
}
