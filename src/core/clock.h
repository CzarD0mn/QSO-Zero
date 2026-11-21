/* QSO-Zero - disciplined UTC clock. The app never touches the system clock;
 * it keeps an offset learned from GPS (or trusts NTP/system time). */
#ifndef QSOZERO_CLOCK_H
#define QSOZERO_CLOCK_H

typedef enum { CLK_SYSTEM = 0, CLK_GPS = 1 } clk_source_t;

void   clk_init(void);
double clk_now(void);                 /* corrected UTC, seconds since epoch */
double clk_mono(void);                /* monotonic seconds */
void   clk_gps_sample(double gps_utc, double mono_at_second); /* from GPS */
double clk_offset(void);              /* corrected - system, seconds */
clk_source_t clk_source(void);
int    clk_locked(void);              /* GPS lock good enough for TX */
double clk_age(void);                 /* seconds since last GPS sample */

/* Seconds into the current T/R period of length period_s, and the UTC
 * start of that period. */
double clk_period_pos(double period_s, double *period_start);

#endif
