#include "clock.h"
#include <math.h>
#include <pthread.h>
#include <time.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
/* corrected_utc = mono + g_base ; g_base learned from GPS, else system */
static double g_base;
static int    g_have_gps;
static double g_last_gps_mono = -1e9;
static double g_samples[16];
static int    g_ns, g_si;

double clk_mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double sys_utc(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void clk_init(void)
{
    pthread_mutex_lock(&g_lock);
    g_base = sys_utc() - clk_mono();
    g_have_gps = 0;
    g_ns = g_si = 0;
    pthread_mutex_unlock(&g_lock);
}

static int cmpd(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

#include <stdlib.h>
void clk_gps_sample(double gps_utc, double mono_at_second)
{
    double base = gps_utc - mono_at_second;
    pthread_mutex_lock(&g_lock);
    g_samples[g_si] = base;
    g_si = (g_si + 1) % 16;
    if (g_ns < 16) ++g_ns;
    double tmp[16];
    for (int i = 0; i < g_ns; ++i) tmp[i] = g_samples[i];
    qsort(tmp, g_ns, sizeof(double), cmpd);
    double med = tmp[g_ns / 2];
    /* median of recent samples rejects late/early NMEA bursts; slew gently */
    if (!g_have_gps || fabs(med - g_base) > 0.5) g_base = med;
    else g_base += 0.2 * (med - g_base);
    g_have_gps = 1;
    g_last_gps_mono = mono_at_second;
    pthread_mutex_unlock(&g_lock);
}

double clk_now(void)
{
    pthread_mutex_lock(&g_lock);
    double b = g_have_gps ? g_base : sys_utc() - clk_mono();
    pthread_mutex_unlock(&g_lock);
    return clk_mono() + b;
}

double clk_offset(void) { return clk_now() - sys_utc(); }
clk_source_t clk_source(void) { return g_have_gps ? CLK_GPS : CLK_SYSTEM; }
double clk_age(void) { return clk_mono() - g_last_gps_mono; }
int clk_locked(void) { return g_have_gps && g_ns >= 3 && clk_age() < 120.0; }

double clk_period_pos(double period_s, double *start)
{
    double t = clk_now();
    double s = floor(t / period_s) * period_s;
    if (start) *start = s;
    return t - s;
}
