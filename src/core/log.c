/* QSO-Zero - activity log. */
#include "core/log.h"

#include <errno.h>
#include <pthread.h>
#include <pwd.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#define LOG_MAX_BYTES (1L * 1024 * 1024)   /* start a new file past 1 MB */
#define LOG_KEEP 5                          /* qso-zero.log + .1 .. .4: at most ~5 MB */

static FILE *g_f;
static char g_path[256];
static long g_bytes;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

const char *log_path(void) { return g_path; }

static void mkdirs(const char *path)
{
    char p[256];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; ++s)
        if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
}

/* qso-zero.log -> .1 -> .2 ... ; the oldest (.LOG_KEEP-1) is deleted */
static void rotate(void)
{
    char a[280], b[280];
    snprintf(a, sizeof a, "%s.%d", g_path, LOG_KEEP - 1);
    remove(a);
    for (int i = LOG_KEEP - 2; i >= 1; --i) {
        snprintf(a, sizeof a, "%s.%d", g_path, i);
        snprintf(b, sizeof b, "%s.%d", g_path, i + 1);
        rename(a, b);
    }
    snprintf(b, sizeof b, "%s.1", g_path);
    rename(g_path, b);
}

int log_open(const char *path)
{
    if (path) snprintf(g_path, sizeof g_path, "%s", path);
    else {
        const char *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");
        if (!h || !*h) { struct passwd *pw = getpwuid(getuid()); h = pw ? pw->pw_dir : NULL; }   /* launcher may not set HOME */
        if (x && *x) snprintf(g_path, sizeof g_path, "%s/qso-zero/qso-zero.log", x);
        else snprintf(g_path, sizeof g_path, "%s/.cache/qso-zero/qso-zero.log", h && *h ? h : "/tmp");
    }
    mkdirs(g_path);
    rotate();                                   /* each run starts a fresh file; older runs are .1 .. .4 */
    g_f = fopen(g_path, "w");
    if (!g_f) { g_path[0] = 0; return -1; }
    g_bytes = 0;
    return 0;
}

void log_close(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_f) { fclose(g_f); g_f = NULL; }
    pthread_mutex_unlock(&g_lock);
}

void qlog(const char *area, const char *fmt, ...)
{
    if (!g_f) return;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    gmtime_r(&tv.tv_sec, &tm);
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_lock);
    if (g_f) {
        if (g_bytes > LOG_MAX_BYTES) {
            fclose(g_f);
            rotate();
            g_f = fopen(g_path, "w");
            g_bytes = 0;
        }
        if (g_f) {
            int n = fprintf(g_f, "%04d-%02d-%02d %02d:%02d:%02d.%03dZ [%s] %s\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                            tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000), area, msg);
            fflush(g_f);
            if (n > 0) g_bytes += n;
        }
    }
    pthread_mutex_unlock(&g_lock);
}
