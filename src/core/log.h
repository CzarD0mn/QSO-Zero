/* QSO-Zero - activity log for diagnosing problems.
 * ~/.cache/qso-zero/qso-zero.log; the 4 before it are kept as .1 (newest)
 * .. .4, each at most 1 MB, so the logs never take more than ~5 MB.
 * Lines: "2026-11-21 19:40:01.123Z [area] text". Nothing is written until
 * log_open() is called, so tests stay quiet. */
#ifndef QSOZERO_LOG_H
#define QSOZERO_LOG_H

int  log_open(const char *path);      /* NULL = the default path; 0 on success */
void log_close(void);
const char *log_path(void);
void qlog(const char *area, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
