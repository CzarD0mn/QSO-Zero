#include "uplink.h"
#include "core/log.h"
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "core/clock.h"

#define MAXQ 256
#define CONNECT_MS 5000

static pthread_t g_thr;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static volatile int g_run, g_kick;
static char g_host[64], g_ca[256];
static int g_https_port, g_http_port;
static const char *(*g_rcall)(void), *(*g_rgrid)(void);
static uplink_spot_t g_q[MAXQ];
static int g_nq;
static uplink_stats_t g_st;

/* ------------------------------------------------------------------ */
/* libssl, loaded at runtime (OpenSSL 3 or 1.1: same function ABI)      */
/* ------------------------------------------------------------------ */

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_method_st SSL_METHOD;

static struct {
    void *h;
    int (*init)(uint64_t, const void *);
    const SSL_METHOD *(*method)(void);
    SSL_CTX *(*ctx_new)(const SSL_METHOD *);
    void (*ctx_free)(SSL_CTX *);
    int (*default_paths)(SSL_CTX *);
    int (*load_locations)(SSL_CTX *, const char *, const char *);
    void (*set_verify)(SSL_CTX *, int, void *);
    SSL *(*new_)(SSL_CTX *);
    void (*free_)(SSL *);
    int (*set_fd)(SSL *, int);
    long (*ctrl)(SSL *, int, long, void *);
    int (*set1_host)(SSL *, const char *);
    int (*connect)(SSL *);
    int (*write)(SSL *, const void *, int);
    int (*read)(SSL *, void *, int);
    int (*shutdown)(SSL *);
} S;
static SSL_CTX *g_ctx;
static int g_tls_tried;

#define SSL_VERIFY_PEER 1
#define SSL_CTRL_SET_TLSEXT_HOSTNAME 55
#define TLSEXT_NAMETYPE_host_name 0

static int tls_load(void)
{
    if (g_tls_tried) return g_ctx ? 0 : -1;
    g_tls_tried = 1;
    const char *libs[] = {"libssl.so.3", "libssl.so.1.1", "libssl.so"};
    for (int i = 0; i < 3 && !S.h; ++i) S.h = dlopen(libs[i], RTLD_NOW);
    if (!S.h) return -1;
#define SYM(f, n) if (!(*(void **)&S.f = dlsym(S.h, n))) return -1;
    SYM(init, "OPENSSL_init_ssl");
    SYM(method, "TLS_client_method");
    SYM(ctx_new, "SSL_CTX_new");
    SYM(ctx_free, "SSL_CTX_free");
    SYM(default_paths, "SSL_CTX_set_default_verify_paths");
    SYM(load_locations, "SSL_CTX_load_verify_locations");
    SYM(set_verify, "SSL_CTX_set_verify");
    SYM(new_, "SSL_new");
    SYM(free_, "SSL_free");
    SYM(set_fd, "SSL_set_fd");
    SYM(ctrl, "SSL_ctrl");
    SYM(set1_host, "SSL_set1_host");
    SYM(connect, "SSL_connect");
    SYM(write, "SSL_write");
    SYM(read, "SSL_read");
    SYM(shutdown, "SSL_shutdown");
#undef SYM
    S.init(0, NULL);
    g_ctx = S.ctx_new(S.method());
    if (!g_ctx) return -1;
    if (g_ca[0]) S.load_locations(g_ctx, g_ca, NULL);
    else S.default_paths(g_ctx);
    S.set_verify(g_ctx, SSL_VERIFY_PEER, NULL);    /* reject bad / untrusted certificates */
    return 0;
}

/* ------------------------------------------------------------------ */
/* connections                                                          */
/* ------------------------------------------------------------------ */

typedef struct { int fd; SSL *ssl; } conn_t;

/* TCP connect with a timeout; returns fd or -1. Doubles as the internet
 * check: it needs working DNS, a route and the server answering. */
static int tcp_connect(const char *host, int port, int timeout_ms)
{
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hint = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *res, *ai;
    if (getaddrinfo(host, ps, &hint, &res)) return -1;
    int fd = -1;
    for (ai = res; ai && fd < 0; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, 0);
        if (fd < 0) continue;
        fcntl(fd, F_SETFL, O_NONBLOCK);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) && errno != EINPROGRESS) { close(fd); fd = -1; continue; }
        struct pollfd p = {.fd = fd, .events = POLLOUT};
        int err = 0;
        socklen_t el = sizeof err;
        if (poll(&p, 1, timeout_ms) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) || err) { close(fd); fd = -1; }
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        fcntl(fd, F_SETFL, 0);
        struct timeval tv = {10, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }
    return fd;
}

static void set_err(const char *m)
{
    pthread_mutex_lock(&g_lock);
    snprintf(g_st.last_error, sizeof g_st.last_error, "%s", m);
    pthread_mutex_unlock(&g_lock);
}

static int conn_open(conn_t *c, int tls)
{
    c->ssl = NULL;
    c->fd = tcp_connect(g_host, tls ? g_https_port : g_http_port, CONNECT_MS);
    if (c->fd < 0) { set_err(tls ? "https: no connection" : "http: no connection"); return -1; }
    if (!tls) return 0;
    if (tls_load()) { close(c->fd); set_err("https: libssl not available"); return -1; }
    c->ssl = S.new_(g_ctx);
    if (!c->ssl) { close(c->fd); return -1; }
    S.set_fd(c->ssl, c->fd);
    S.ctrl(c->ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, g_host);   /* SNI */
    S.set1_host(c->ssl, g_host);                                                       /* name check */
    if (S.connect(c->ssl) != 1) {
        set_err("https: TLS handshake / certificate failed");
        S.free_(c->ssl);
        close(c->fd);
        c->ssl = NULL;
        return -1;
    }
    return 0;
}

static int conn_write(conn_t *c, const char *b, int n)
{
    return c->ssl ? S.write(c->ssl, b, n) : (int)send(c->fd, b, n, MSG_NOSIGNAL);
}

static int conn_read(conn_t *c, char *b, int n)
{
    return c->ssl ? S.read(c->ssl, b, n) : (int)recv(c->fd, b, n, 0);
}

static void conn_close(conn_t *c)
{
    if (c->ssl) { S.shutdown(c->ssl); S.free_(c->ssl); }
    if (c->fd >= 0) close(c->fd);
}

/* ------------------------------------------------------------------ */
/* wsprnet request                                                      */
/* ------------------------------------------------------------------ */

static void urlenc(const char *s, char *out, size_t n)
{
    size_t o = 0;
    for (; *s && o + 4 < n; ++s) {
        if (isalnum((unsigned char)*s) || *s == '-' || *s == '.' || *s == '_') out[o++] = *s;
        else o += (size_t)snprintf(out + o, n - o, "%%%02X", (unsigned char)*s);
    }
    out[o] = 0;
}

/* One spot = one GET, the same request format wsprd/rtlsdr-wsprd use. */
static int post_spot(const uplink_spot_t *s, const char *rcall, const char *rgrid, int tls)
{
    conn_t c;
    if (conn_open(&c, tls)) return -1;
    time_t t = (time_t)s->utc;
    struct tm tm;
    gmtime_r(&t, &tm);
    char rc[40], rg[16], tc[40], tg[16], req[768];
    urlenc(rcall, rc, sizeof rc); urlenc(rgrid, rg, sizeof rg);
    urlenc(s->call, tc, sizeof tc); urlenc(s->grid, tg, sizeof tg);
    double mhz = (s->dial_hz + s->audio_hz) / 1e6;
    int n = snprintf(req, sizeof req,
        "GET /post?function=wspr&rcall=%s&rgrid=%s&rqrg=%.6f&date=%02d%02d%02d&time=%02d%02d"
        "&sig=%d&dt=%.1f&drift=%d&tqrg=%.6f&tcall=%s&tgrid=%s&dbm=%d&version=QSO-Zero-1&mode=2 HTTP/1.0\r\n"
        "Host: %s\r\nUser-Agent: QSO-Zero\r\nConnection: close\r\n\r\n",
        rc, rg, mhz, tm.tm_year % 100, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
        s->snr, s->dt, s->drift, mhz, tc, tg, s->dbm, g_host);
    int ok = conn_write(&c, req, n) == n;
    char resp[64] = "";
    int got = 0;
    while (ok && got < 12) {
        int r = conn_read(&c, resp + got, (int)sizeof resp - 1 - got);
        if (r <= 0) break;
        got += r;
    }
    resp[got] = 0;
    conn_close(&c);
    ok = ok && got > 12 && (strstr(resp, " 200") || strstr(resp, " 30"));   /* 2xx/3xx accepted */
    if (!ok) set_err(tls ? "https: server rejected the spot" : "http: server rejected the spot");
    return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* the 5-minute cycle                                                   */
/* ------------------------------------------------------------------ */

static void cycle(void)
{
    /* internet check: either port answering counts as online */
    int online = 0, fd = tcp_connect(g_host, g_https_port, CONNECT_MS);
    if (fd < 0) fd = tcp_connect(g_host, g_http_port, CONNECT_MS);
    if (fd >= 0) { online = 1; close(fd); }

    pthread_mutex_lock(&g_lock);
    uplink_spot_t q[MAXQ];
    int n = g_nq;
    memcpy(q, g_q, sizeof(uplink_spot_t) * n);
    g_nq = 0;                                      /* sent or dropped: never kept */
    g_st.online = online;
    g_st.last_check = clk_now();
    if (!online) g_st.dropped += n;
    pthread_mutex_unlock(&g_lock);
    qlog("net", "%s %s, %d WSPR spot%s %s", g_host, online ? "reachable" : "unreachable", n, n == 1 ? "" : "s", online ? "to send" : "dropped");
    if (!online || !n) return;

    const char *rcall = g_rcall(), *rgrid = g_rgrid();
    if (!rcall[0] || !strcmp(rcall, "N0CALL")) { pthread_mutex_lock(&g_lock); g_st.dropped += n; pthread_mutex_unlock(&g_lock); return; }
    int https = 0, http = 0, failed = 0, use_tls = 1;
    for (int i = 0; i < n; ++i) {
        /* HTTPS first; on failure fall back to HTTP. Once HTTPS has failed in
         * this cycle, go straight to HTTP for the remaining spots. */
        if (use_tls && post_spot(&q[i], rcall, rgrid, 1) == 0) { ++https; continue; }
        use_tls = 0;
        if (post_spot(&q[i], rcall, rgrid, 0) == 0) ++http; else ++failed;
    }
    pthread_mutex_lock(&g_lock);
    g_st.sent_https += https;
    g_st.sent_http += http;
    g_st.failed += failed;
    g_st.tls_available = g_ctx != NULL;
    pthread_mutex_unlock(&g_lock);
    qlog("net", "wsprnet: %d by HTTPS, %d by HTTP, %d failed%s%s", https, http, failed, g_st.last_error[0] ? ": " : "", g_st.last_error);
}

static void *thread_fn(void *arg)
{
    (void)arg;
    while (g_run) {
        cycle();
        pthread_mutex_lock(&g_lock);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += UPLINK_INTERVAL_S;
        while (g_run && !g_kick && pthread_cond_timedwait(&g_cond, &g_lock, &ts) != ETIMEDOUT) {}
        g_kick = 0;
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

void uplink_set_ca_file(const char *path) { snprintf(g_ca, sizeof g_ca, "%s", path); }

void uplink_start(const char *host, int https_port, int http_port,
                  const char *(*rcall)(void), const char *(*rgrid)(void))
{
    snprintf(g_host, sizeof g_host, "%s", host);
    g_https_port = https_port;
    g_http_port = http_port;
    g_rcall = rcall;
    g_rgrid = rgrid;
    g_run = 1;
    pthread_create(&g_thr, NULL, thread_fn, NULL);
}

void uplink_stop(void)
{
    if (!g_run) return;
    pthread_mutex_lock(&g_lock);
    g_run = 0;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
    pthread_join(g_thr, NULL);
}

void uplink_add_wspr(const uplink_spot_t *s)
{
    pthread_mutex_lock(&g_lock);
    if (g_nq < MAXQ) g_q[g_nq++] = *s;
    pthread_mutex_unlock(&g_lock);
}

void uplink_check_now(void)
{
    pthread_mutex_lock(&g_lock);
    g_kick = 1;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

void uplink_get(uplink_stats_t *out)
{
    pthread_mutex_lock(&g_lock);
    *out = g_st;
    pthread_mutex_unlock(&g_lock);
}
