/* QSO-Zero - optional upload of logged QSOs to QRZ Logbook and Wavelog.
 *
 * Settings live in their own file, ~/.config/qso-zero-logbook.conf (mode
 * 0600, written by 'qso-zero-setup --logbook'); without it nothing is sent.
 * The ADIF log itself is only ever read, never changed or deleted. What has
 * been sent is remembered per service in <log folder>/uploads/<service>.txt,
 * one line per QSO, so each QSO goes once and QSOs logged while offline go
 * when the internet is back. */
#ifndef QSOZERO_LOGBOOK_H
#define QSOZERO_LOGBOOK_H

#include <stddef.h>

enum { LB_QRZ, LB_WAVELOG, LB_N };

typedef struct {
    char qrz_key[64];
    int  qrz_auto;            /* upload after each QSO (default 1) */
    char qrz_url[160];        /* default https://logbook.qrz.com/api */
    char wl_url[160];         /* Wavelog base URL, e.g. https://log.example.com */
    char wl_key[96];
    char wl_station[16];      /* station_profile_id */
    int  wl_auto;
} lb_cfg_t;

typedef struct {
    int  configured[LB_N], auto_on[LB_N];
    int  in_log;              /* QSOs in the ADIF log (last pass) */
    int  pending[LB_N];       /* not yet sent (last pass) */
    int  sent[LB_N], dupes[LB_N], rejected[LB_N];   /* since start */
    char last_error[LB_N][128];
    double last_pass;         /* UTC, 0 = none yet */
} lb_stats_t;

/* outcome of one QSO upload */
enum { LB_SENT, LB_DUPE, LB_REJECTED, LB_STOP };

const char *lb_name(int svc);                                  /* "QRZ" / "Wavelog" */
const char *logbook_conf_path(void);                           /* ~/.config/qso-zero-logbook.conf */
int  lb_cfg_load(lb_cfg_t *c, const char *path);              /* missing file: nothing configured */
int  lb_configured(const lb_cfg_t *c, int svc);
void lb_sent_path(const char *adi_path, int svc, char *out, size_t n);
void lb_wavelog_url(const char *base, const char *api, char *out, size_t n);

/* Send every QSO of the log not yet recorded as sent. retry: also resend
 * QSOs the service rejected before. verbose: print a line per QSO (CLI).
 * Returns the number of QSOs still pending. */
int  lb_upload(const lb_cfg_t *c, int svc, const char *adi_path, int retry, int verbose, lb_stats_t *st);

/* background uploader in the app: a pass at start, after each logged QSO
 * (logbook_kick) and every 5 minutes; the settings file is re-read each pass */
void logbook_start(const char *conf_path, const char *adi_path);
void logbook_kick(void);
void logbook_stop(void);
void logbook_get(lb_stats_t *out);

/* qso-zero --logbook status|test|stations|upload|mark|count [qrz|wavelog|all]
 *   upload  sends what is waiting, and retries QSOs rejected before
 *   mark    records every QSO now in the log as already uploaded
 *   count   prints how many QSOs are not sent yet */
int  logbook_cli(int argc, char **argv, const char *conf_path, const char *adi_path);

#endif
