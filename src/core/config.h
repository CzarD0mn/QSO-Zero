/* QSO-Zero - configuration (simple key = value file). */
#ifndef QSOZERO_CONFIG_H
#define QSOZERO_CONFIG_H

typedef struct {
    char callsign[16];
    char grid[8];            /* 4 or 6 chars; GPS overrides when auto_grid */
    int  auto_grid;
    char radio[16];          /* qmx | qmxplus | g90 | x6100 | x6200 | ft891 */
    char rig_device[128];
    int  rig_baud;           /* 0 = profile default */
    char ptt[8];             /* cat | rts | dtr | "" = profile default */
    char ptt_device[128];
    int  spawn_rigctld;
    char audio_in[96];       /* "" = auto-detect from radio profile */
    char audio_out[96];
    float tx_level;          /* 0..1 audio drive */
    char gps_device[96];
    int  gps_baud;
    int  gps_latency_ms;
    char display[64];        /* auto | /dev/fbN */
    char keyboard[96];       /* auto | /dev/input/eventN */
    char jsc_path[160];
    int  wspr_dbm;
    int  wspr_tx_pct;        /* % of slots to beacon */
    int  js8_submode;        /* 0 normal 1 fast 2 turbo 3 slow */
    int  rx_offset;          /* audio Hz */
    int  tx_offset;
    char log_path[160];
    int  js8_autoreply;      /* answer SNR?/GRID?/INFO?/STATUS?/HEARING?/AGN?/MSG */
    int  js8_hb_ack;         /* acknowledge heartbeats (needs js8_autoreply) */
    char js8_info[80];       /* reply to INFO? (blank = don't answer) */
    char js8_status[80];     /* reply to STATUS? (blank = don't answer) */
    char inbox_path[160];    /* JS8 MSG inbox */
    int  wspr_rx;            /* decode WSPR in receive slots */
    int  wsprnet_upload;     /* upload WSPR spots when online (checked every 5 min) */
    char wsprnet_host[64];
} config_t;

void config_defaults(config_t *c);
int  config_load(config_t *c, const char *path);   /* missing file is not an error */
int  config_save(const config_t *c, const char *path);
const char *config_default_path(void);

#endif
