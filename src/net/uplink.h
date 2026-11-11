/* NeonQRP - wsprnet uploader with an internet check every 5 minutes.
 * Spots collected since the last check are uploaded if the check finds a
 * connection, and dropped if it doesn't (no backlog when operating offline).
 *
 * Transport: HTTPS first (certificate verified against the system CA store,
 * via libssl loaded at runtime); if HTTPS fails for any reason, the same
 * request is retried over plain HTTP. */
#ifndef NEONQRP_UPLINK_H
#define NEONQRP_UPLINK_H

#include <stdint.h>

#define UPLINK_INTERVAL_S 300

typedef struct {
    double  utc;          /* start of the 2-minute cycle */
    int64_t dial_hz;
    float   audio_hz;
    int     snr, drift, dbm;
    float   dt;
    char    call[13], grid[7];
} uplink_spot_t;

typedef struct {
    int    online;        /* result of the last check */
    double last_check;    /* UTC */
    int    sent_https, sent_http, dropped, failed;
    int    tls_available; /* libssl could be loaded */
    char   last_error[96];
} uplink_stats_t;

/* rcall/rgrid are read on every upload so callsign/grid edits apply.
 * Ports: 443 / 80 for wsprnet.org. */
void uplink_start(const char *host, int https_port, int http_port,
                  const char *(*rcall)(void), const char *(*rgrid)(void));
void uplink_stop(void);
void uplink_add_wspr(const uplink_spot_t *s);
void uplink_check_now(void);           /* force a check + upload (e.g. /net) */
void uplink_get(uplink_stats_t *out);

/* tests only: trust this CA file instead of the system store */
void uplink_set_ca_file(const char *path);

#endif
