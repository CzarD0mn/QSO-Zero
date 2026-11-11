/* NeonQRP - Hamlib rig control via rigctld (spawned and supervised by us). */
#ifndef NEONQRP_RIGCTL_H
#define NEONQRP_RIGCTL_H

#include "radios.h"
#include <stdint.h>

typedef struct {
    const radio_profile_t *profile;
    char   serial[128];
    int    baud;          /* 0 = profile default */
    ptt_method_t ptt;
    char   ptt_device[128]; /* for RTS/DTR when not on the CAT port */
    int    port;          /* rigctld TCP port, default 4532 */
    int    spawn;         /* 1 = we start rigctld, 0 = connect to an existing one */
} rig_config_t;

int  rig_open(const rig_config_t *cfg);
void rig_close(void);
int  rig_connected(void);
const char *rig_error(void);

int  rig_set_freq(int64_t hz);
int64_t rig_get_freq(void);
int  rig_set_mode(const char *mode, int passband);
int  rig_ptt(int on);          /* safe to call repeatedly */
int  rig_ptt_state(void);
int  rig_get_level(const char *level, float *val);  /* SWR, RFPOWER, STRENGTH */
int  rig_set_level(const char *level, float val);

/* Installs atexit + signal handlers that drop PTT if the app dies. */
void rig_install_safety(void);

#endif
