/* NeonQRP - NMEA GPS reader (M5Stack Cap LoRa-1262 / LoRa868: ATGM336H,
 * UART 115200 8N1). Feeds the disciplined clock and the Maidenhead grid. */
#ifndef NEONQRP_GPS_H
#define NEONQRP_GPS_H

typedef struct {
    int    fix;          /* 0 none, 1 2D/3D */
    int    sats;
    double lat, lon;
    char   grid[7];      /* 6-char Maidenhead */
    double last_utc;     /* last GPS time seen */
    int    time_valid;
} gps_state_t;

/* Start the reader thread. latency_ms is the typical delay between the UTC
 * second edge and the first byte of the NMEA burst (ATGM336H ~ 30-80 ms). */
int  gps_start(const char *dev, int baud, int latency_ms);
void gps_stop(void);
void gps_get(gps_state_t *out);

/* Exposed for tests: feed one NMEA line with its burst-start timestamp. */
void gps_feed_line(const char *line, double mono_burst_start);
void latlon_to_grid(double lat, double lon, char out[7]);

#endif
