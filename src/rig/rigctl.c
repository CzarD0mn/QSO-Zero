#include "rigctl.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int             g_sock = -1;
static pid_t           g_child = -1;
static int             g_ptt;
static char            g_err[160];
static rig_config_t    g_cfg;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

const char *rig_error(void) { return g_err; }
int rig_connected(void) { return g_sock >= 0; }
int rig_ptt_state(void) { return g_ptt; }

static void sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static int tcp_connect(int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return -1; }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return s;
}

static pid_t spawn_rigctld(int model, const rig_config_t *c)
{
    char m[16], s[16], p[16];
    snprintf(m, sizeof m, "%d", model);
    snprintf(s, sizeof s, "%d", c->baud ? c->baud : c->profile->baud);
    snprintf(p, sizeof p, "%d", c->port ? c->port : 4532);
    const char *argv[24];
    int n = 0;
    argv[n++] = "rigctld";
    argv[n++] = "-m"; argv[n++] = m;
    argv[n++] = "-r"; argv[n++] = c->serial;
    argv[n++] = "-s"; argv[n++] = s;
    argv[n++] = "-t"; argv[n++] = p;
    argv[n++] = "-T"; argv[n++] = "127.0.0.1";
    if (c->ptt == PTT_RTS || c->ptt == PTT_DTR) {
        argv[n++] = "-P"; argv[n++] = c->ptt == PTT_RTS ? "RTS" : "DTR";
        argv[n++] = "-p"; argv[n++] = c->ptt_device[0] ? c->ptt_device : c->serial;
    }
    argv[n] = NULL;
    pid_t pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }
        setpgid(0, 0);
        execvp("rigctld", (char *const *)argv);
        _exit(127);
    }
    return pid;
}

static void kill_child(void)
{
    if (g_child > 0) {
        kill(g_child, SIGTERM);
        for (int i = 0; i < 20; ++i) {
            if (waitpid(g_child, NULL, WNOHANG) == g_child) { g_child = -1; return; }
            sleep_ms(50);
        }
        kill(g_child, SIGKILL);
        waitpid(g_child, NULL, 0);
        g_child = -1;
    }
}

/* Send one command, read the reply up to "RPRT n" or a value line. */
static int rig_cmd(const char *cmd, char *reply, size_t rsz)
{
    if (g_sock < 0) { snprintf(g_err, sizeof g_err, "rig not connected"); return -1; }
    char buf[128];
    int n = snprintf(buf, sizeof buf, "+%s\n", cmd);   /* extended response protocol */
    if (send(g_sock, buf, n, MSG_NOSIGNAL) != n) { snprintf(g_err, sizeof g_err, "rigctld write failed"); return -1; }
    size_t got = 0;
    char acc[1024];
    for (;;) {
        struct pollfd p = {.fd = g_sock, .events = POLLIN};
        if (poll(&p, 1, 1500) <= 0) { snprintf(g_err, sizeof g_err, "rigctld timeout on '%s'", cmd); return -1; }
        ssize_t r = recv(g_sock, acc + got, sizeof acc - 1 - got, 0);
        if (r <= 0) { snprintf(g_err, sizeof g_err, "rigctld closed"); return -1; }
        got += r;
        acc[got] = 0;
        char *rp = strstr(acc, "RPRT ");
        if (rp && strchr(rp, '\n')) {
            int code = atoi(rp + 5);
            if (reply) {
                /* value lines are "Name: value" in extended mode */
                reply[0] = 0;
                char *line = strchr(acc, '\n');
                while (line && line < rp) {
                    char *next = strchr(line + 1, '\n');
                    char *colon = strchr(line + 1, ':');
                    if (colon && (!next || colon < next)) {
                        size_t L = (size_t)((next ? next : rp) - colon - 1);
                        while (L && (colon[1] == ' ')) { ++colon; --L; }
                        if (L >= rsz) L = rsz - 1;
                        memcpy(reply, colon + 1, L);
                        reply[L] = 0;
                        break;
                    }
                    line = next;
                }
            }
            if (code) snprintf(g_err, sizeof g_err, "Hamlib error %d on '%s'", code, cmd);
            return code;
        }
        if (got >= sizeof acc - 1) return -1;
    }
}

static int try_model(int model, const rig_config_t *c)
{
    if (c->spawn) {
        g_child = spawn_rigctld(model, c);
        if (g_child < 0) return -1;
    }
    for (int i = 0; i < 40; ++i) {            /* up to 4 s for rigctld to bind */
        sleep_ms(100);
        if (c->spawn && waitpid(g_child, NULL, WNOHANG) == g_child) {
            g_child = -1;
            snprintf(g_err, sizeof g_err, "rigctld exited (model %d, %s)", model, c->serial);
            return -1;
        }
        g_sock = tcp_connect(c->port ? c->port : 4532);
        if (g_sock >= 0) break;
    }
    if (g_sock < 0) { snprintf(g_err, sizeof g_err, "cannot reach rigctld"); kill_child(); return -1; }
    char f[64];
    if (rig_cmd("f", f, sizeof f) != 0) {      /* prove the radio answers */
        close(g_sock); g_sock = -1; kill_child();
        return -1;
    }
    return 0;
}

int rig_open(const rig_config_t *cfg)
{
    pthread_mutex_lock(&g_lock);
    g_cfg = *cfg;
    int rc = try_model(cfg->profile->hamlib_model, cfg);
    if (rc && cfg->profile->hamlib_fallback)
        rc = try_model(cfg->profile->hamlib_fallback, cfg);
    pthread_mutex_unlock(&g_lock);
    return rc;
}

void rig_close(void)
{
    rig_ptt(0);
    pthread_mutex_lock(&g_lock);
    if (g_sock >= 0) { close(g_sock); g_sock = -1; }
    kill_child();
    pthread_mutex_unlock(&g_lock);
}

int rig_set_freq(int64_t hz)
{
    char c[48];
    snprintf(c, sizeof c, "F %lld", (long long)hz);
    pthread_mutex_lock(&g_lock);
    int r = rig_cmd(c, NULL, 0);
    pthread_mutex_unlock(&g_lock);
    return r;
}

int64_t rig_get_freq(void)
{
    char v[64];
    pthread_mutex_lock(&g_lock);
    int r = rig_cmd("f", v, sizeof v);
    pthread_mutex_unlock(&g_lock);
    return r ? -1 : atoll(v);
}

int rig_set_mode(const char *mode, int pb)
{
    char c[48];
    snprintf(c, sizeof c, "M %s %d", mode, pb);
    pthread_mutex_lock(&g_lock);
    int r = rig_cmd(c, NULL, 0);
    pthread_mutex_unlock(&g_lock);
    return r;
}

int rig_ptt(int on)
{
    pthread_mutex_lock(&g_lock);
    int r = -1;
    if (g_sock >= 0) r = rig_cmd(on ? "T 1" : "T 0", NULL, 0);
    if (r != 0 && !on && g_child > 0) {      /* can't unkey via CAT: kill rigctld, port closes */
        kill(g_child, SIGTERM);
        r = 0;
    }
    if (r == 0) g_ptt = on;
    pthread_mutex_unlock(&g_lock);
    return r;
}

int rig_get_level(const char *level, float *val)
{
    char c[48], v[64];
    snprintf(c, sizeof c, "l %s", level);
    pthread_mutex_lock(&g_lock);
    int r = rig_cmd(c, v, sizeof v);
    pthread_mutex_unlock(&g_lock);
    if (!r) *val = (float)atof(v);
    return r;
}

int rig_set_level(const char *level, float val)
{
    char c[64];
    snprintf(c, sizeof c, "L %s %.3f", level, val);
    pthread_mutex_lock(&g_lock);
    int r = rig_cmd(c, NULL, 0);
    pthread_mutex_unlock(&g_lock);
    return r;
}

/* ---- safety: never leave a transmitter keyed ---- */

static void emergency_unkey(void)
{
    if (!g_ptt) return;
    if (g_sock >= 0) {
        const char *c = "T 0\n";
        (void)!send(g_sock, c, strlen(c), MSG_NOSIGNAL);
    }
    g_ptt = 0;
}

static void on_signal(int sig)
{
    emergency_unkey();
    if (g_child > 0) kill(g_child, SIGTERM);
    signal(sig, SIG_DFL);
    raise(sig);
}

void rig_install_safety(void)
{
    atexit(emergency_unkey);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGSEGV, on_signal);
    signal(SIGABRT, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGPIPE, SIG_IGN);
    /* Deliberately no direct RTS/DTR handle here: opening a tty on Linux
     * raises RTS and DTR, which would KEY an RTS-PTT radio. rigctld owns the
     * port; if it dies, the port closes and the lines drop with it. */
}
