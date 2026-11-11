#include "gps.h"
#include "core/clock.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static pthread_t       g_thr;
static volatile int    g_run;
static int             g_fd = -1;
static double          g_latency = 0.05;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static gps_state_t     g_st;
static int             g_burst_used;   /* only one time sample per burst */

void latlon_to_grid(double lat, double lon, char g[7])
{
    double x = lon + 180.0, y = lat + 90.0;
    if (x < 0) x = 0;
    if (x >= 360) x = 359.9999;
    if (y < 0) y = 0;
    if (y >= 180) y = 179.9999;
    g[0] = 'A' + (int)(x / 20); g[1] = 'A' + (int)(y / 10);
    g[2] = '0' + (int)fmod(x / 2, 10); g[3] = '0' + (int)fmod(y, 10);
    g[4] = 'a' + (int)fmod(x * 12, 24); g[5] = 'a' + (int)fmod(y * 24, 24);
    g[6] = 0;
}

static int nmea_checksum_ok(const char *s)
{
    if (*s != '$') return 0;
    const char *star = strchr(s, '*');
    if (!star) return 0;
    unsigned cs = 0;
    for (const char *p = s + 1; p < star; ++p) cs ^= (unsigned char)*p;
    return (unsigned)strtoul(star + 1, NULL, 16) == cs;
}

/* split comma fields in place */
static int fields(char *s, char **f, int max)
{
    int n = 0;
    f[n++] = s;
    for (char *p = s; *p && n < max; ++p)
        if (*p == ',' || *p == '*') { *p = 0; f[n++] = p + 1; }
    return n;
}

static double nmea_deg(const char *v, const char *hemi)
{
    if (!*v) return NAN;
    double d = atof(v);
    int deg = (int)(d / 100);
    double r = deg + (d - deg * 100) / 60.0;
    if (*hemi == 'S' || *hemi == 'W') r = -r;
    return r;
}

static time_t utc_mktime(int y, int mo, int d, int h, int mi, int s)
{
    struct tm tm = {0};
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s;
    return timegm(&tm);
}

void gps_feed_line(const char *line, double burst_start)
{
    if (!nmea_checksum_ok(line)) return;
    char buf[128];
    snprintf(buf, sizeof buf, "%s", line + 1);
    char *f[24];
    int n = fields(buf, f, 24);
    const char *type = f[0] + 2;   /* skip talker GP/GN/BD */
    pthread_mutex_lock(&g_lock);
    if (!strcmp(type, "RMC") && n >= 10) {
        /* hhmmss.ss, A/V, lat, N, lon, E, sog, cog, ddmmyy */
        int valid = f[2][0] == 'A';
        if (strlen(f[1]) >= 6 && strlen(f[9]) == 6) {
            int hh = (f[1][0] - '0') * 10 + f[1][1] - '0', mm = (f[1][2] - '0') * 10 + f[1][3] - '0';
            int ss = (f[1][4] - '0') * 10 + f[1][5] - '0';
            double frac = strlen(f[1]) > 6 ? atof(f[1] + 6) : 0.0;
            int dd = (f[9][0] - '0') * 10 + f[9][1] - '0', mo = (f[9][2] - '0') * 10 + f[9][3] - '0';
            int yy = 2000 + (f[9][4] - '0') * 10 + f[9][5] - '0';
            double utc = (double)utc_mktime(yy, mo, dd, hh, mm, ss) + frac;
            g_st.last_utc = utc;
            g_st.time_valid = valid;
            /* time is usable once the receiver reports a valid fix */
            if (valid && !g_burst_used && burst_start > 0) {
                g_burst_used = 1;
                pthread_mutex_unlock(&g_lock);
                clk_gps_sample(utc, burst_start - g_latency);
                pthread_mutex_lock(&g_lock);
            }
        }
        if (valid) {
            double la = nmea_deg(f[3], f[4]), lo = nmea_deg(f[5], f[6]);
            if (!isnan(la) && !isnan(lo)) {
                g_st.lat = la; g_st.lon = lo; g_st.fix = 1;
                latlon_to_grid(la, lo, g_st.grid);
            }
        } else g_st.fix = 0;
    } else if (!strcmp(type, "GGA") && n >= 8) {
        g_st.sats = atoi(f[7]);
    }
    pthread_mutex_unlock(&g_lock);
}

static int open_serial(const char *dev, int baud)
{
    int fd = open(dev, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct termios t;
    tcgetattr(fd, &t);
    cfmakeraw(&t);
    speed_t sp = baud == 9600 ? B9600 : baud == 38400 ? B38400 : baud == 57600 ? B57600 : B115200;
    cfsetispeed(&t, sp);
    cfsetospeed(&t, sp);
    t.c_cflag |= CLOCAL | CREAD;
    tcsetattr(fd, TCSANOW, &t);
    tcflush(fd, TCIFLUSH);
    return fd;
}

static void *gps_thread(void *arg)
{
    (void)arg;
    char line[160];
    size_t ll = 0;
    double burst = 0, last_byte = 0;
    while (g_run) {
        struct pollfd p = {.fd = g_fd, .events = POLLIN};
        if (poll(&p, 1, 200) <= 0) continue;
        char buf[256];
        ssize_t r = read(g_fd, buf, sizeof buf);
        double now = clk_mono();
        if (r <= 0) continue;
        /* A gap of >300 ms means this read starts a new once-per-second burst. */
        if (now - last_byte > 0.3) { burst = now; g_burst_used = 0; }
        last_byte = now;
        for (ssize_t i = 0; i < r; ++i) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (ll) { line[ll] = 0; gps_feed_line(line, burst); ll = 0; }
            } else if (ll < sizeof line - 1) line[ll++] = c;
        }
    }
    return NULL;
}

int gps_start(const char *dev, int baud, int latency_ms)
{
    g_latency = latency_ms / 1000.0;
    g_fd = open_serial(dev, baud);
    if (g_fd < 0) return -1;
    g_run = 1;
    if (pthread_create(&g_thr, NULL, gps_thread, NULL)) { close(g_fd); g_fd = -1; return -1; }
    return 0;
}

void gps_stop(void)
{
    if (g_fd < 0) return;
    g_run = 0;
    pthread_join(g_thr, NULL);
    close(g_fd);
    g_fd = -1;
}

void gps_get(gps_state_t *out)
{
    pthread_mutex_lock(&g_lock);
    *out = g_st;
    pthread_mutex_unlock(&g_lock);
}
