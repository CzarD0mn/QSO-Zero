#include "config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

void config_defaults(config_t *c)
{
    memset(c, 0, sizeof *c);
    strcpy(c->callsign, "N0CALL");
    strcpy(c->grid, "FN42");
    c->auto_grid = 1;
    strcpy(c->radio, "qmx");
    c->spawn_rigctld = 1;
    c->tx_level = 0.5f;
    strcpy(c->gps_device, "/dev/serial0");
    c->gps_baud = 115200;
    c->gps_latency_ms = 50;
    strcpy(c->display, "auto");
    strcpy(c->keyboard, "auto");
    strcpy(c->jsc_path, "/usr/share/qso-zero/js8.jsc");
    c->wspr_dbm = 37;
    c->wspr_tx_pct = 20;
    c->js8_submode = 0;
    c->rx_offset = 1500;
    c->tx_offset = 1500;
    const char *home = getenv("HOME");
    if (!home || !*home) { struct passwd *pw = getpwuid(getuid()); home = pw && pw->pw_dir ? pw->pw_dir : "/tmp"; }
    snprintf(c->log_path, sizeof c->log_path, "%s/qso-zero/qso-zero.adi", home);   /* ADIF log; folder made on demand */
    c->js8_autoreply = 0;
    c->js8_hb_ack = 0;
    snprintf(c->inbox_path, sizeof c->inbox_path, "%s/qso-zero-inbox.txt", home);
    c->wspr_rx = 1;
    c->auto_qsy = 1;
    c->audio_in_level = c->audio_out_level = -1;
    c->wsprnet_upload = 1;
    strcpy(c->wsprnet_host, "wsprnet.org");
}

const char *config_default_path(void)
{
    static char p[256];
    const char *home = getenv("HOME");
    snprintf(p, sizeof p, "%s/.config/qso-zero.conf", home ? home : "/root");
    return p;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) ++s;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    return s;
}

#define STR(k, f) else if (!strcmp(key, k)) snprintf(c->f, sizeof c->f, "%s", val)
#define INT(k, f) else if (!strcmp(key, k)) c->f = atoi(val)
#define FLT(k, f) else if (!strcmp(key, k)) c->f = (float)atof(val)

int config_load(config_t *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        /* strip inline comments: '#' at line start or preceded by whitespace */
        for (char *h = line; *h; ++h)
            if (*h == '#' && (h == line || isspace((unsigned char)h[-1]))) { *h = 0; break; }
        char *s = trim(line);
        if (!*s || *s == ';' || *s == '[') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s), *val = trim(eq + 1);
        if (0) {}
        STR("callsign", callsign); STR("grid", grid); INT("auto_grid", auto_grid);
        STR("radio", radio); STR("rig_device", rig_device); INT("rig_baud", rig_baud);
        STR("ptt", ptt); STR("ptt_device", ptt_device); INT("spawn_rigctld", spawn_rigctld);
        STR("audio_in", audio_in); STR("audio_out", audio_out); FLT("tx_level", tx_level);
        STR("gps_device", gps_device); INT("gps_baud", gps_baud); INT("gps_latency_ms", gps_latency_ms);
        STR("display", display); STR("keyboard", keyboard); STR("jsc_path", jsc_path);
        INT("wspr_dbm", wspr_dbm); INT("wspr_tx_pct", wspr_tx_pct); INT("js8_submode", js8_submode);
        INT("auto_qsy", auto_qsy); INT("audio_in_level", audio_in_level); INT("audio_out_level", audio_out_level); STR("bands", bands);
        INT("rx_offset", rx_offset); INT("tx_offset", tx_offset); STR("log_path", log_path);
        INT("ghostnet", ghostnet); STR("cq_mod", cq_mod); INT("cq_only", cq_only); INT("js8_autoreply", js8_autoreply); INT("js8_hb_ack", js8_hb_ack);
        STR("js8_info", js8_info); STR("js8_status", js8_status); STR("inbox_path", inbox_path);
        INT("wspr_rx", wspr_rx); INT("wsprnet_upload", wsprnet_upload); STR("wsprnet_host", wsprnet_host);
    }
    fclose(f);
    for (char *p = c->callsign; *p; ++p) *p = (char)toupper((unsigned char)*p);
    return 0;
}

int config_save(const config_t *c, const char *path)
{
    char dir[256];
    snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = 0; mkdir(dir, 0755); }
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# QSO-Zero configuration\n");
    fprintf(f, "callsign = %s\ngrid = %s\nauto_grid = %d\n", c->callsign, c->grid, c->auto_grid);
    fprintf(f, "\n# radio: qmx | qmxplus | g90 | x6100 | x6200 | ft891\nradio = %s\n", c->radio);
    fprintf(f, "rig_device = %s\nrig_baud = %d\nptt = %s\nptt_device = %s\nspawn_rigctld = %d\n",
            c->rig_device, c->rig_baud, c->ptt, c->ptt_device, c->spawn_rigctld);
    fprintf(f, "\n# blank = auto-detect from the radio profile\naudio_in = %s\naudio_out = %s\ntx_level = %.2f\n",
            c->audio_in, c->audio_out, c->tx_level);
    fprintf(f, "\ngps_device = %s\ngps_baud = %d\ngps_latency_ms = %d\n", c->gps_device, c->gps_baud, c->gps_latency_ms);
    fprintf(f, "\ndisplay = %s\nkeyboard = %s\njsc_path = %s\n", c->display, c->keyboard, c->jsc_path);
    fprintf(f, "\nwspr_dbm = %d\nwspr_tx_pct = %d\njs8_submode = %d\nrx_offset = %d\ntx_offset = %d\n",
            c->wspr_dbm, c->wspr_tx_pct, c->js8_submode, c->rx_offset, c->tx_offset);
    {   /* only a custom log location is saved; the default stays ~/qso-zero/qso-zero.adi */
        config_t def;
        config_defaults(&def);
        if (strcmp(c->log_path, def.log_path)) fprintf(f, "log_path = %s\n", c->log_path);
    }
    fprintf(f, "\n# FT8 CQ modifier (CQ POTA ...): 1-4 letters or 3 digits, blank for plain CQ\ncq_mod = %s\n", c->cq_mod);
    fprintf(f, "\n# FT8/FT4: show only CQ calls in the decode list (/cqonly)\ncq_only = %d\n", c->cq_only);
    fprintf(f, "\n# GHOSTNET: JS8 on 3.575 / 7.107 / 14.107 MHz USB-D instead of the usual JS8 frequencies\nghostnet = %d\n", c->ghostnet);
    fprintf(f, "\n# JS8 automatic replies (only to messages addressed to you)\njs8_autoreply = %d\njs8_hb_ack = %d\n"
               "js8_info = %s\njs8_status = %s\ninbox_path = %s\nwspr_rx = %d\n",
            c->js8_autoreply, c->js8_hb_ack, c->js8_info, c->js8_status, c->inbox_path, c->wspr_rx);
    fprintf(f, "wsprnet_upload = %d\nwsprnet_host = %s\n", c->wsprnet_upload, c->wsprnet_host);
    fprintf(f, "\n# sound card levels in %% (-1 = leave as they are); set in Setup\naudio_in_level = %d\naudio_out_level = %d\n",
            c->audio_in_level, c->audio_out_level);
    fprintf(f, "\n# auto_qsy: tune the radio to each mode's standard frequency automatically\nauto_qsy = %d\nbands = %s\n",
            c->auto_qsy, c->bands);
    fclose(f);
    return 0;
}
