#define _GNU_SOURCE                                      /* strcasestr */
#include "logbook.h"
#include "uplink.h"
#include "core/log.h"
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef QSOZERO_VERSION
#define QSOZERO_VERSION "dev"
#endif
#define UA "QSO-Zero/" QSOZERO_VERSION " (Cardputer Zero)"
#define QRZ_URL "https://logbook.qrz.com/api"
#define PASS_S 300

static const char *const NAMES[LB_N] = {"QRZ", "Wavelog"};
static const char *const FILES[LB_N] = {"qrz", "wavelog"};

const char *lb_name(int svc) { return svc >= 0 && svc < LB_N ? NAMES[svc] : "?"; }

const char *logbook_conf_path(void)
{
    static char p[256];
    const char *home = getenv("HOME");
    snprintf(p, sizeof p, "%s/.config/qso-zero-logbook.conf", home ? home : "/root");
    return p;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) ++s;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    return s;
}

int lb_cfg_load(lb_cfg_t *c, const char *path)
{
    memset(c, 0, sizeof *c);
    c->qrz_auto = c->wl_auto = 1;
    snprintf(c->qrz_url, sizeof c->qrz_url, "%s", QRZ_URL);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
#define S_(name, f) else if (!strcmp(k, name)) snprintf(c->f, sizeof c->f, "%s", v)
#define I_(name, f) else if (!strcmp(k, name)) c->f = atoi(v)
        if (0) {}
        S_("qrz_api_key", qrz_key); I_("qrz_upload", qrz_auto); S_("qrz_url", qrz_url);
        S_("wavelog_url", wl_url); S_("wavelog_api_key", wl_key); S_("wavelog_station_id", wl_station); I_("wavelog_upload", wl_auto);
#undef S_
#undef I_
    }
    fclose(f);
    /* the QRZ key only ever goes to QRZ over HTTPS (a test server on this
     * machine is the one exception) */
    if (strncmp(c->qrz_url, "https://", 8) && strncmp(c->qrz_url, "http://127.0.0.1:", 17))
        snprintf(c->qrz_url, sizeof c->qrz_url, "%s", QRZ_URL);
    return 0;
}

int lb_configured(const lb_cfg_t *c, int svc)
{
    if (svc == LB_QRZ) return c->qrz_key[0] != 0;
    return c->wl_url[0] && c->wl_key[0] && c->wl_station[0];
}

void lb_sent_path(const char *adi_path, int svc, char *out, size_t n)
{
    const char *sl = strrchr(adi_path, '/');
    int dl = sl ? (int)(sl - adi_path) : 1;
    snprintf(out, n, "%.*s/uploads/%s.txt", dl, sl ? adi_path : ".", FILES[svc]);
}

/* https://log.example.com[/][index.php][/api/qso] -> .../index.php/api/<api> */
void lb_wavelog_url(const char *base, const char *api, char *out, size_t n)
{
    char b[200];
    snprintf(b, sizeof b, "%s", base);
    size_t l = strlen(b);
    while (l && b[l - 1] == '/') b[--l] = 0;
    const char *tails[] = {"/api/qso", "/api/qso/", "/api"};
    for (int i = 0; i < 3; ++i) {
        size_t tl = strlen(tails[i]);
        if (l > tl && !strcasecmp(b + l - tl, tails[i])) { b[l -= tl] = 0; break; }
    }
    while (l && b[l - 1] == '/') b[--l] = 0;
    if (l >= 10 && !strcasecmp(b + l - 10, "/index.php")) snprintf(out, n, "%s/api/%s", b, api);
    else snprintf(out, n, "%s/index.php/api/%s", b, api);
}

/* ------------------------------------------------------------------ */
/* ADIF reading                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *rec;          /* points into the file buffer */
    size_t len;               /* through <eor> */
    char key[80];             /* CALL|YYYYMMDD|HHMMSS|BAND|MODE */
} qso_t;

static const char *ci_find(const char *s, const char *end, const char *pat)
{
    size_t pl = strlen(pat);
    for (; s + pl <= end; ++s)
        if (!strncasecmp(s, pat, pl)) return s;
    return NULL;
}

/* value of an ADIF field inside one record, upper-cased */
static void adif_field(const char *r, const char *end, const char *name, char *out, size_t n)
{
    out[0] = 0;
    size_t nl = strlen(name);
    for (const char *p = r; p < end; ++p) {
        if (*p != '<' || p + nl + 2 >= end || strncasecmp(p + 1, name, nl) || p[nl + 1] != ':') continue;
        const char *q = p + nl + 2;
        int len = atoi(q);
        q = memchr(q, '>', (size_t)(end - q));
        if (!q || len < 0) return;
        ++q;
        if (q + len > end) len = (int)(end - q);
        size_t o = 0;
        for (int i = 0; i < len && o + 1 < n; ++i) out[o++] = (char)toupper((unsigned char)q[i]);
        out[o] = 0;
        return;
    }
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 65536, n = 0;
    char *b = malloc(cap);
    for (;;) {
        if (n + 4096 + 1 > cap) { cap *= 2; char *nb = realloc(b, cap); if (!nb) { free(b); fclose(f); return NULL; } b = nb; }
        size_t r = fread(b + n, 1, cap - n - 1, f);
        if (!r) break;
        n += r;
    }
    fclose(f);
    b[n] = 0;
    *len = n;
    return b;
}

/* every complete record (ending in <eor>) of the log */
static int adif_records(const char *buf, size_t len, qso_t **out)
{
    const char *end = buf + len, *p = ci_find(buf, end, "<eoh>");
    p = p ? p + 5 : buf;
    int n = 0, cap = 64;
    qso_t *v = malloc(sizeof *v * cap);
    for (;;) {
        const char *e = ci_find(p, end, "<eor>");
        if (!e) break;
        while (p < e && *p != '<') ++p;
        const char *re = e + 5;
        char call[24], date[12], tm[12], band[12], mode[16], sub[16];
        adif_field(p, e, "call", call, sizeof call);
        adif_field(p, e, "qso_date", date, sizeof date);
        adif_field(p, e, "time_on", tm, sizeof tm);
        adif_field(p, e, "band", band, sizeof band);
        adif_field(p, e, "mode", mode, sizeof mode);
        adif_field(p, e, "submode", sub, sizeof sub);
        if (call[0] && date[0]) {
            if (n == cap) { cap *= 2; qso_t *nv = realloc(v, sizeof *v * cap); if (!nv) break; v = nv; }
            v[n].rec = p;
            v[n].len = (size_t)(re - p);
            snprintf(v[n].key, sizeof v[n].key, "%s|%s|%s|%s|%s", call, date, tm, band, sub[0] ? sub : mode);
            ++n;
        }
        p = re;
    }
    *out = v;
    return n;
}

/* ------------------------------------------------------------------ */
/* record of what was sent                                              */
/* ------------------------------------------------------------------ */

typedef struct { char key[80]; int rejected; } done_t;

static int done_cmp(const void *a, const void *b) { return strcmp(((const done_t *)a)->key, ((const done_t *)b)->key); }

/* lines: KEY <tab> sent|dupe|rejected|skipped <tab> UTC <tab> detail. A QSO
 * with any line other than "rejected" is in the logbook (see done_find). */
static int done_load(const char *path, done_t **out)
{
    int n = 0, cap = 64;
    done_t *v = malloc(sizeof *v * cap);
    FILE *f = fopen(path, "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        char *t = strchr(line, '\t');
        if (!t) continue;
        *t = 0;
        if (n == cap) { cap *= 2; done_t *nv = realloc(v, sizeof *v * cap); if (!nv) break; v = nv; }
        snprintf(v[n].key, sizeof v[n].key, "%s", line);
        v[n].rejected = !strncmp(t + 1, "rejected", 8);
        ++n;
    }
    if (f) fclose(f);
    qsort(v, n, sizeof *v, done_cmp);
    *out = v;
    return n;
}

static const done_t *done_find(const done_t *v, int n, const char *key)
{
    done_t k;
    snprintf(k.key, sizeof k.key, "%s", key);
    const done_t *d = bsearch(&k, v, n, sizeof *v, done_cmp);
    if (!d) return NULL;
    /* any non-rejected line for the key means it is in the logbook */
    const done_t *lo = d, *hi = d;
    while (lo > v && !strcmp(lo[-1].key, key)) --lo;
    while (hi + 1 < v + n && !strcmp(hi[1].key, key)) ++hi;
    for (const done_t *x = lo; x <= hi; ++x)
        if (!x->rejected) return x;
    return d;
}

static int mkdir_for(const char *file)
{
    char d[256];
    snprintf(d, sizeof d, "%s", file);
    for (char *p = d + 1; *p; ++p)
        if (*p == '/') { *p = 0; if (mkdir(d, 0755) && errno != EEXIST) return -1; *p = '/'; }
    return 0;
}

static void done_add(const char *path, const char *key, const char *status, const char *detail)
{
    mkdir_for(path);
    FILE *f = fopen(path, "a");
    if (!f) { qlog("logbook", "cannot write %s", path); return; }
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    char ts[24], d[160];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%SZ", &tm);
    snprintf(d, sizeof d, "%s", detail ? detail : "");
    for (char *p = d; *p; ++p) if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
    fprintf(f, "%s\t%s\t%s\t%s\n", key, status, ts, d);
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* the two services                                                     */
/* ------------------------------------------------------------------ */

static void urlenc(const char *s, size_t len, char *out, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < len && o + 4 < n; ++i) {
        unsigned char ch = (unsigned char)s[i];
        if (isalnum(ch) || ch == '-' || ch == '.' || ch == '_' || ch == '~') out[o++] = (char)ch;
        else o += (size_t)snprintf(out + o, n - o, "%%%02X", ch);
    }
    out[o] = 0;
}

static void urldec(char *s)
{
    char *o = s;
    for (; *s; ++s) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else *o++ = *s == '+' ? ' ' : *s;
    }
    *o = 0;
}

/* NAME=value from a QRZ name=value&... reply */
static void kv_get(const char *r, const char *name, char *out, size_t n)
{
    out[0] = 0;
    size_t nl = strlen(name);
    for (const char *p = r; (p = strstr(p, name)); p += nl) {
        if ((p == r || p[-1] == '&' || p[-1] == '\n') && p[nl] == '=') {
            p += nl + 1;
            size_t l = strcspn(p, "&\r\n");
            snprintf(out, n, "%.*s", (int)(l < n ? l : n - 1), p);
            urldec(out);
            return;
        }
    }
}

static void json_esc(const char *s, size_t len, char *out, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < len && o + 7 < n; ++i) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '"' || ch == '\\') { out[o++] = '\\'; out[o++] = (char)ch; }
        else if (ch == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (ch == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (ch == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else if (ch < 0x20) o += (size_t)snprintf(out + o, n - o, "\\u%04x", ch);
        else out[o++] = (char)ch;
    }
    out[o] = 0;
}

/* "name": "value" | number | null, searched in [s, end) */
static int json_get(const char *s, const char *end, const char *name, char *out, size_t n)
{
    out[0] = 0;
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", name);
    const char *p = ci_find(s, end, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (p < end && (isspace((unsigned char)*p) || *p == ':')) ++p;
    size_t o = 0;
    if (p < end && *p == '"') {
        for (++p; p < end && *p != '"' && o + 1 < n; ++p) {
            if (*p == '\\' && p + 1 < end) ++p;
            out[o++] = *p;
        }
    } else {
        while (p < end && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p) && o + 1 < n) out[o++] = *p++;
        if (!strcmp(out, "null")) o = 0;
    }
    out[o] = 0;
    return 0;
}

/* the text of the "messages" array (or "reason"/"message"), for errors */
static void json_messages(const char *r, char *out, size_t n)
{
    out[0] = 0;
    const char *end = r + strlen(r), *m = ci_find(r, end, "\"messages\"");
    if (m && (m = strchr(m, '['))) {
        size_t o = 0;
        for (const char *p = m + 1; *p && *p != ']' && o + 3 < n; ++p) {
            if (*p != '"') continue;
            const char *q = p + 1;
            size_t start = o;
            while (*q && *q != '"' && o + 3 < n) { if (*q == '\\' && q[1]) ++q; out[o++] = *q++; }
            if (o > start && o + 3 < n) { out[o++] = ';'; out[o++] = ' '; }
            p = *q ? q : q - 1;
        }
        while (o && (out[o - 1] == ' ' || out[o - 1] == ';')) --o;
        out[o] = 0;
        /* Wavelog puts <br> in some messages */
        for (char *b; (b = strstr(out, "<br>")); ) memmove(b, b + 4, strlen(b + 4) + 1);
    }
    if (!out[0]) json_get(r, end, "reason", out, n);
    if (!out[0]) json_get(r, end, "message", out, n);
}

/* One QSO to QRZ. QRZ answers RESULT=OK|REPLACE|FAIL|AUTH&REASON=... */
static int send_qrz(const lb_cfg_t *c, const qso_t *q, char *detail, size_t dn)
{
    char k[200], body[8192], resp[4096], res[16], reason[256], logid[32];
    urlenc(c->qrz_key, strlen(c->qrz_key), k, sizeof k);
    int bl = snprintf(body, sizeof body, "KEY=%s&ACTION=INSERT&ADIF=", k);
    urlenc(q->rec, q->len, body + bl, sizeof body - (size_t)bl);
    int st;
    if (net_request("POST", c->qrz_url, "application/x-www-form-urlencoded", UA, body, resp, sizeof resp, &st)) {
        snprintf(detail, dn, "%s", resp);
        return LB_STOP;
    }
    kv_get(resp, "RESULT", res, sizeof res);
    kv_get(resp, "REASON", reason, sizeof reason);
    kv_get(resp, "LOGID", logid, sizeof logid);
    if (!strcasecmp(res, "OK") || !strcasecmp(res, "REPLACE")) { snprintf(detail, dn, "logid %s", logid); return LB_SENT; }
    if (strcasestr(reason, "duplicate")) { snprintf(detail, dn, "%s", reason); return LB_DUPE; }
    if (!strcasecmp(res, "AUTH") || strcasestr(reason, "key") || strcasestr(reason, "auth") || strcasestr(reason, "subscription")) {
        snprintf(detail, dn, "QRZ refused the API key: %s", reason[0] ? reason : res);
        return LB_STOP;
    }
    if (!res[0]) { snprintf(detail, dn, "unexpected reply from QRZ (HTTP %d)", st); return LB_STOP; }
    snprintf(detail, dn, "%s", reason[0] ? reason : res);
    return LB_REJECTED;
}

/* One QSO to Wavelog: POST {key, station_profile_id, type:"adif", string} */
static int send_wavelog(const lb_cfg_t *c, const qso_t *q, char *detail, size_t dn)
{
    char url[256], ek[200], es[200], adif[6144], body[8192], resp[8192], msg[256], status[32], errs[16];
    lb_wavelog_url(c->wl_url, "qso", url, sizeof url);
    json_esc(c->wl_key, strlen(c->wl_key), ek, sizeof ek);
    json_esc(c->wl_station, strlen(c->wl_station), es, sizeof es);
    json_esc(q->rec, q->len, adif, sizeof adif);
    snprintf(body, sizeof body, "{\"key\":\"%s\",\"station_profile_id\":\"%s\",\"type\":\"adif\",\"string\":\"%s\"}", ek, es, adif);
    int st;
    if (net_request("POST", url, "application/json", UA, body, resp, sizeof resp, &st)) {
        snprintf(detail, dn, "%s", resp);
        return LB_STOP;
    }
    const char *end = resp + strlen(resp);
    json_get(resp, end, "status", status, sizeof status);
    json_get(resp, end, "adif_errors", errs, sizeof errs);
    json_messages(resp, msg, sizeof msg);
    if (st >= 200 && st < 300 && !strcasecmp(status, "created") && atoi(errs) == 0) { snprintf(detail, dn, "created"); return LB_SENT; }
    if (strcasestr(msg, "duplicate") || strcasestr(msg, "dupe")) { snprintf(detail, dn, "%s", msg); return LB_DUPE; }
    if (st == 401 || st == 403 || !strcasecmp(status, "failed")) {
        snprintf(detail, dn, "Wavelog refused the key or station: %s", msg[0] ? msg : status[0] ? status : "HTTP error");
        return LB_STOP;
    }
    if (!status[0]) { snprintf(detail, dn, "unexpected reply from %s (HTTP %d) - check the Wavelog URL", url, st); return LB_STOP; }
    snprintf(detail, dn, "%s", msg[0] ? msg : status);
    return LB_REJECTED;
}

/* ------------------------------------------------------------------ */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static lb_stats_t g_st;

static void st_set_err(lb_stats_t *st, int svc, const char *m)
{
    if (st) snprintf(st->last_error[svc], sizeof st->last_error[svc], "%s", m);
}

int lb_upload(const lb_cfg_t *c, int svc, const char *adi_path, int retry, int verbose, lb_stats_t *st)
{
    if (!lb_configured(c, svc)) return 0;
    size_t len;
    char *buf = read_file(adi_path, &len);
    if (!buf) { if (verbose) printf("%s: no log yet (%s)\n", lb_name(svc), adi_path); return 0; }
    qso_t *q;
    int nq = adif_records(buf, len, &q);
    char sp[300];
    lb_sent_path(adi_path, svc, sp, sizeof sp);
    done_t *d;
    int nd = done_load(sp, &d);
    int pending = 0, sent = 0, dupes = 0, rej = 0, stopped = 0;
    for (int i = 0; i < nq; ++i) {
        const done_t *x = done_find(d, nd, q[i].key);
        if (x && (!x->rejected || !retry)) continue;
        /* the same QSO twice in the log is sent once */
        int seen = 0;
        for (int j = 0; j < i && !seen; ++j) seen = !strcmp(q[j].key, q[i].key);
        if (seen) continue;
        if (stopped) { ++pending; continue; }
        char det[200];
        int r = svc == LB_QRZ ? send_qrz(c, &q[i], det, sizeof det) : send_wavelog(c, &q[i], det, sizeof det);
        const char *what = r == LB_SENT ? "sent" : r == LB_DUPE ? "dupe" : r == LB_REJECTED ? "rejected" : NULL;
        if (verbose) printf("  %-8s %-40s %s\n", what ? what : "error", q[i].key, det);
        if (what) {
            done_add(sp, q[i].key, what, det);
            qlog("logbook", "%s: %s %s%s%s", lb_name(svc), q[i].key, what, det[0] ? ": " : "", det);
        }
        if (r == LB_SENT) ++sent;
        else if (r == LB_DUPE) ++dupes;
        else if (r == LB_REJECTED) { ++rej; st_set_err(st, svc, det); }
        else { stopped = 1; ++pending; st_set_err(st, svc, det); qlog("logbook", "%s: %s - will try again later", lb_name(svc), det); }
        if (r != LB_STOP) usleep(250000);                       /* be gentle with the servers */
    }
    if (st) {
        st->sent[svc] += sent;
        st->dupes[svc] += dupes;
        st->rejected[svc] += rej;
        st->pending[svc] = pending;
        st->in_log = nq;
        if (!stopped && !rej) st->last_error[svc][0] = 0;
    }
    if (sent || dupes || rej) qlog("logbook", "%s: %d sent, %d already there, %d rejected, %d pending", lb_name(svc), sent, dupes, rej, pending);
    free(q);
    free(d);
    free(buf);
    return pending;
}

/* QSOs of the log with no line in the service's record (or only rejected) */
static void count_pending(int svc, const char *adi_path, int *in_log, int *pending, int *rejected)
{
    *in_log = *pending = *rejected = 0;
    size_t len;
    char *buf = read_file(adi_path, &len);
    if (!buf) return;
    qso_t *q;
    int nq = adif_records(buf, len, &q);
    char sp[300];
    lb_sent_path(adi_path, svc, sp, sizeof sp);
    done_t *d;
    int nd = done_load(sp, &d);
    *in_log = nq;
    for (int i = 0; i < nq; ++i) {
        const done_t *x = done_find(d, nd, q[i].key);
        if (!x) ++*pending;
        else if (x->rejected) ++*rejected;
    }
    free(q);
    free(d);
    free(buf);
}

/* ------------------------------------------------------------------ */
/* background thread                                                    */
/* ------------------------------------------------------------------ */

static pthread_t g_thr;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static volatile int g_run, g_kick;
static char g_conf[256], g_adi[256];

static void *thread_fn(void *arg)
{
    (void)arg;
    int said = 0;
    while (g_run) {
        lb_cfg_t c;
        int have = lb_cfg_load(&c, g_conf) == 0;
        lb_stats_t st;
        pthread_mutex_lock(&g_lock);
        st = g_st;
        pthread_mutex_unlock(&g_lock);
        for (int s = 0; s < LB_N; ++s) {
            st.configured[s] = have && lb_configured(&c, s);
            st.auto_on[s] = s == LB_QRZ ? c.qrz_auto : c.wl_auto;
            if (st.configured[s] && st.auto_on[s]) lb_upload(&c, s, g_adi, 0, 0, &st);
            else if (st.configured[s]) { int r; count_pending(s, g_adi, &st.in_log, &st.pending[s], &r); }
        }
        if (!said) {
            said = 1;
            qlog("logbook", "uploads: QRZ %s, Wavelog %s (settings %s)",
                 st.configured[LB_QRZ] ? (st.auto_on[LB_QRZ] ? "automatic" : "manual") : "not set up",
                 st.configured[LB_WAVELOG] ? (st.auto_on[LB_WAVELOG] ? "automatic" : "manual") : "not set up", g_conf);
        }
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        st.last_pass = now.tv_sec + now.tv_nsec / 1e9;
        pthread_mutex_lock(&g_lock);
        g_st = st;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += PASS_S;
        while (g_run && !g_kick && pthread_cond_timedwait(&g_cond, &g_lock, &ts) != ETIMEDOUT) {}
        g_kick = 0;
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

void logbook_start(const char *conf_path, const char *adi_path)
{
    if (g_run) return;
    snprintf(g_conf, sizeof g_conf, "%s", conf_path);
    snprintf(g_adi, sizeof g_adi, "%s", adi_path);
    g_run = 1;
    pthread_create(&g_thr, NULL, thread_fn, NULL);
}

void logbook_kick(void)
{
    pthread_mutex_lock(&g_lock);
    g_kick = 1;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

void logbook_stop(void)
{
    if (!g_run) return;
    pthread_mutex_lock(&g_lock);
    g_run = 0;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
    pthread_join(g_thr, NULL);
}

void logbook_get(lb_stats_t *out)
{
    pthread_mutex_lock(&g_lock);
    *out = g_st;
    pthread_mutex_unlock(&g_lock);
}

/* ------------------------------------------------------------------ */
/* command line: qso-zero --logbook ...                                 */
/* ------------------------------------------------------------------ */

static int pick(const char *w, int svc)
{
    return !w || !strcmp(w, "all") || !strcasecmp(w, lb_name(svc)) || !strcasecmp(w, FILES[svc]);
}

static int cli_stations(const lb_cfg_t *c, int quiet)
{
    if (!c->wl_url[0] || !c->wl_key[0]) { printf("Wavelog: URL or API key not set\n"); return 1; }
    char url[400], resp[32768];
    lb_wavelog_url(c->wl_url, "station_info/", url, sizeof url);
    size_t ul = strlen(url);
    urlenc(c->wl_key, strlen(c->wl_key), url + ul, sizeof url - ul);
    int st;
    if (net_request("GET", url, "", UA, NULL, resp, sizeof resp, &st)) { printf("Wavelog: %s\n", resp); return 1; }
    if (st != 200 || !strchr(resp, '[')) {
        char m[200];
        json_messages(resp, m, sizeof m);
        printf("Wavelog: HTTP %d %s - check the URL and the API key\n", st, m);
        return 1;
    }
    int n = 0, found = 0;
    for (const char *p = resp; (p = strchr(p, '{')); ) {
        const char *e = strchr(p, '}');
        if (!e) break;
        char id[16], name[80], call[24], act[8], grid[12];
        json_get(p, e, "station_id", id, sizeof id);
        json_get(p, e, "station_profile_name", name, sizeof name);
        json_get(p, e, "station_callsign", call, sizeof call);
        json_get(p, e, "station_gridsquare", grid, sizeof grid);
        json_get(p, e, "station_active", act, sizeof act);
        if (id[0]) {
            ++n;
            int mine = !strcmp(id, c->wl_station);
            found |= mine;
            if (!quiet || mine)
                printf("  %s %-4s %-12s %-8s %s%s\n", mine ? "*" : " ", id, call, grid, name, !strcmp(act, "1") ? "  (active)" : "");
        }
        p = e + 1;
    }
    if (!n) printf("Wavelog: no station locations for this API key\n");
    return n ? (c->wl_station[0] && !found ? 2 : 0) : 1;
}

int logbook_cli(int argc, char **argv, const char *conf_path, const char *adi_path)
{
    const char *cmd = argc > 0 ? argv[0] : "status", *which = argc > 1 ? argv[1] : NULL;
    lb_cfg_t c;
    int have = lb_cfg_load(&c, conf_path) == 0;
    if (!strcmp(cmd, "stations")) return cli_stations(&c, 0);
    int rc = 0;
    for (int s = 0; s < LB_N; ++s) {
        if (!pick(which, s)) continue;
        int conf = have && lb_configured(&c, s), aut = s == LB_QRZ ? c.qrz_auto : c.wl_auto;
        if (!strcmp(cmd, "count")) {                     /* for qso-zero-setup: QSOs not sent yet */
            int n, p, r;
            count_pending(s, adi_path, &n, &p, &r);
            printf("%d\n", p);
            continue;
        }
        if (!strcmp(cmd, "status")) {
            int n, p, r;
            count_pending(s, adi_path, &n, &p, &r);
            if (!conf) printf("%-8s not set up (qso-zero-setup --logbook)\n", lb_name(s));
            else printf("%-8s %s upload: %d QSOs in the log, %d not sent yet, %d rejected\n", lb_name(s), aut ? "automatic" : "manual", n, p, r);
            continue;
        }
        if (!conf) { if (which) { printf("%s: not set up (qso-zero-setup --logbook)\n", lb_name(s)); rc = 1; } continue; }
        if (!strcmp(cmd, "test")) {
            if (s == LB_QRZ) {
                char k[200], body[300], resp[4096], res[16], reason[200];
                urlenc(c.qrz_key, strlen(c.qrz_key), k, sizeof k);
                snprintf(body, sizeof body, "KEY=%s&ACTION=STATUS", k);
                int st;
                if (net_request("POST", c.qrz_url, "application/x-www-form-urlencoded", UA, body, resp, sizeof resp, &st)) {
                    printf("QRZ: %s\n", resp); rc = 1; continue;
                }
                kv_get(resp, "RESULT", res, sizeof res);
                kv_get(resp, "REASON", reason, sizeof reason);
                if (strcasecmp(res, "OK")) { printf("QRZ: key NOT accepted: %s %s\n", res[0] ? res : "no reply", reason); rc = 1; continue; }
                /* DATA= holds the logbook's details as name=value pairs */
                char data[600] = "";
                const char *dp = strstr(resp, "DATA=");
                if (dp) { snprintf(data, sizeof data, "%s", dp + 5); urldec(data); }
                for (char *x = data; *x; ++x) if (*x == '&') *x = ' ';
                data[strcspn(data, "\r\n")] = 0;
                printf("QRZ: key OK%s%.200s\n", data[0] ? "  " : "", data);
            } else {
                printf("Wavelog: %s\n", c.wl_url);
                int r = cli_stations(&c, 1);
                if (r == 2) printf("Wavelog: station location %s is not one of this key's (qso-zero --logbook stations)\n", c.wl_station);
                else if (!r) printf("Wavelog: key OK, station location %s found (the API key must be read+write to upload)\n", c.wl_station);
                rc |= r != 0;
            }
        } else if (!strcmp(cmd, "upload")) {
            printf("%s:\n", lb_name(s));
            lb_stats_t st;
            memset(&st, 0, sizeof st);
            int left = lb_upload(&c, s, adi_path, 1, 1, &st);
            printf("%s: %d sent, %d already in the logbook, %d rejected, %d not sent%s%s\n", lb_name(s), st.sent[s], st.dupes[s],
                   st.rejected[s], left, st.last_error[s][0] && left ? ": " : "", left ? st.last_error[s] : "");
            rc |= left != 0 || st.rejected[s] != 0;
        } else if (!strcmp(cmd, "mark")) {
            size_t len;
            char *buf = read_file(adi_path, &len), sp[300];
            if (!buf) { printf("%s: no log yet\n", lb_name(s)); continue; }
            qso_t *q;
            int nq = adif_records(buf, len, &q), m = 0;
            lb_sent_path(adi_path, s, sp, sizeof sp);
            done_t *d;
            int nd = done_load(sp, &d);
            for (int i = 0; i < nq; ++i)
                if (!done_find(d, nd, q[i].key)) { done_add(sp, q[i].key, "skipped", "marked as already uploaded"); ++m; }
            printf("%s: %d QSOs marked as already uploaded; only QSOs logged from now on will be sent\n", lb_name(s), m);
            free(q); free(d); free(buf);
        } else {
            printf("usage: qso-zero --logbook status|test|stations|upload|mark [qrz|wavelog|all]\n");
            return 2;
        }
    }
    return rc;
}
