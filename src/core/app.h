/* QSO-Zero - application state shared by the engine threads and the UI. */
#ifndef QSOZERO_APP_H
#define QSOZERO_APP_H

#include <pthread.h>
#include <stdint.h>
#include "config.h"
#include "devices.h"
#include "battery.h"
#include "rig/radios.h"

typedef enum { MODE_FT8 = 0, MODE_JS8, MODE_WSPR, MODE_COUNT } op_mode_t;

#define LINE_CQ      0x01
#define LINE_TOME    0x02
#define LINE_TX      0x04
#define LINE_PARTIAL 0x08
#define LINE_HB      0x10
#define LINE_SYS     0x20

typedef struct {
    double    utc;
    int       snr;
    float     dt, freq;
    op_mode_t mode;
    uint8_t   flags;
    int       slot_odd;       /* FT8: decoded in an odd 15 s slot */
    char      text[200];
} rxline_t;

#define WF_BINS 320           /* 0..3000 Hz in 9.375 Hz bins */
#define WF_ROWS 120
#define WF_FMAX 3000.0f
#define MAX_LINES 200
#define MAX_TXQ 32

typedef enum { TXK_NONE, TXK_FT8, TXK_JS8, TXK_WSPR, TXK_TUNE } tx_kind_t;

typedef struct {
    tx_kind_t kind;
    char      text[40];       /* FT8 message, or 12-char JS8 frame */
    int       bits;           /* JS8 frame flags */
    int       slot_odd;       /* FT8: -1 any, 0 even, 1 odd */
} txitem_t;

typedef enum { SEQ_IDLE, SEQ_CQ, SEQ_REPLYING, SEQ_REPORT, SEQ_RREPORT, SEQ_RR73, SEQ_73 } seq_state_t;

typedef struct {
    seq_state_t st;
    char dx[16], dxgrid[8];
    int  snr_sent, snr_rcvd, tries, slot_odd;
    char last_tx[40];
} ft8seq_t;

#define MAX_HEARD 32
typedef struct { char call[16]; double utc; int snr; double last_hb_ack; } heard_t;

typedef struct app {
    pthread_mutex_t lock;
    config_t   cfg;
    const radio_profile_t *radio;

    op_mode_t  mode;
    int        js8_sub;
    int        band;          /* index into the band table */
    int64_t    dial_hz;
    int        rx_off, tx_off;
    unsigned   band_mask;     /* bands this radio covers (and config allows) */
    unsigned   band_rejected; /* bands the radio refused this session */
    int64_t    ghost_hz[11];
    int        aprs_msgno;
    batt_state_t batt;        /* battery, polled every 30 s */    /* APRS message number for the {n} ack id */  /* GHOSTNET: where the operator last tuned on each band (0 = net default) */

    /* audio ring (12 kHz) with UTC mapping */
    int16_t   *ring;
    int        ring_len;
    uint64_t   ring_total;
    double     ring_utc0;     /* UTC of sample #0 (monotone index) */
    int        have_utc0;

    /* waterfall */
    uint8_t    wf[WF_ROWS][WF_BINS];
    int        wf_head;
    float      wf_floor;

    /* receive log */
    rxline_t   lines[MAX_LINES];
    int        nlines;
    int        sel;           /* selected line (-1 none) */
    int        scroll;

    /* transmit */
    txitem_t   txq[MAX_TXQ];
    int        ntxq;
    int        tx_active;     /* 1 while audio is playing */
    float      tx_progress;
    int        tx_enabled;    /* master TX enable (Ctrl+X clears) */
    int        wspr_beacon;   /* WSPR: beacon running */
    double     wspr_tx_slot;  /* start of the last slot we transmitted WSPR in */
    double     tune_until;
    ft8seq_t   seq;

    /* JS8 automation */
    heard_t    heard[MAX_HEARD];
    int        nheard;
    char       js8_last_tx[160];
    int        autoreplies;   /* count sent this session */

    /* status */
    int        rig_ok, audio_ok, gps_ok;
    volatile int rig_busy;    /* reconnecting to the radio in the background */
    char       audio_in_dev[96], audio_out_dev[96];   /* what is actually open */
    char       status[96];
    double     status_until;
    float      swr, power;
    int        decoding;
    double     last_decode_ms;

    /* UI */
    char       input[160];
    int        input_len;
    int        large;         /* large (HDMI) layout */
    int        screen;        /* 0 main, 1 menu, 2 tools, 3 help, 4 device picker */
    int        pick_kind, pick_n, pick_sel;
    dev_opt_t  pick[DEV_MAX];
    int        menu_sel;
    double     esc_t0;        /* >0 while ESC is held (exit countdown) */
    volatile int dirty;
    volatile int quit;
} app_t;

typedef struct { const char *name; int64_t ft8, js8, wspr, lo, hi; } band_t;
const band_t *band_table(int *count);

const char *mode_name(op_mode_t m);
int  grid_distance_km(const char *g1, const char *g2);
int  mode_period(const app_t *a);

int  app_init(app_t *a, const char *config_path, int hw);
void app_shutdown(app_t *a);
void app_set_mode(app_t *a, op_mode_t m);
void app_set_band(app_t *a, int band);      /* explicit band choice: always tunes */
void app_step_band(app_t *a, int dir);
/* Setup pickers: change the radio, CAT port or sound cards and reconnect
 * without restarting. NULL leaves that setting as it is. */
void app_use_radio(app_t *a, const char *id);
void app_poll_battery(app_t *a);              /* read now (also done every 30 s) */
/* FT8 CQ modifier: "" or 1-4 letters A-Z or exactly 3 digits (the WSJT-X
 * 77-bit rule). Returns 0 if accepted. */
int  app_set_cq_mod(app_t *a, const char *mod);
int  cq_mod_valid(const char *mod);
/* JS8 -> APRS through the @APRSIS group (relayed to APRS-IS by JS8Call
 * stations with APRS spotting on). Return 0 if queued. */
int  app_aprs_position(app_t *a);
int  app_aprs_message(app_t *a, const char *to, const char *text);   /* to = CALL[-SSID] */
int  app_aprs_sms(app_t *a, const char *number, const char *text);   /* number or alias */
int  app_aprs_email(app_t *a, const char *addr, const char *text);
/* GHOSTNET JS8 frequencies. ghostnet_freq() is 0 for bands without one. */
int64_t ghostnet_freq(int band);
void app_set_ghostnet(app_t *a, int on);          /* on: JS8 on the nearest GHOSTNET band */
void app_ghostnet_band(app_t *a, int band);       /* JS8 + GHOSTNET default frequency on this band */
void app_use_rig_device(app_t *a, const char *dev);
void app_use_audio(app_t *a, const char *in, const char *out);
void app_rescan_audio(app_t *a);
void app_rig_port(const app_t *a, char *out, int outsz);   /* the CAT port in use (saved or auto) */             /* look for sound devices again and reconnect */      /* next/previous band the radio covers */
int  band_from_freq(int64_t hz);            /* band index, -1 outside the ham bands */
unsigned app_bands(const app_t *a);         /* usable band bits */
void app_status(app_t *a, const char *fmt, ...);
void app_add_line(app_t *a, const rxline_t *l);

/* audio callback (12 kHz) */
void app_audio_in(const int16_t *pcm, int n, double utc_first, void *user);

/* user actions */
void app_submit_input(app_t *a);
void app_reply_selected(app_t *a);
void app_call_cq(app_t *a);
void app_send_heartbeat(app_t *a);
void app_halt_tx(app_t *a);
void app_tune(app_t *a, double seconds);
void app_toggle_wspr(app_t *a);

/* engine threads */
int  app_start_threads(app_t *a);
void app_stop_threads(app_t *a);

/* scheduler.c */
void seq_on_decode(app_t *a, const rxline_t *l);
void seq_start_reply(app_t *a, const rxline_t *l);
void seq_start_cq(app_t *a);
void seq_reset(app_t *a);
void seq_after_cycle(app_t *a, int rx_slot_odd);
void js8_assemble(app_t *a, const char frame[13], int bits, int snr, float freq, float dt, double utc);
/* js8auto.c (lock held) */
void js8_heard(app_t *a, const char *call, int snr, double utc);
void js8_auto_on_message(app_t *a, const rxline_t *l);
void app_queue_js8_locked(app_t *a, const char *text);

void adif_log(app_t *a, const char *call, const char *grid, const char *mode, int sent, int rcvd);

#endif
