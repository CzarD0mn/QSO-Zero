/* QRZ / Wavelog uploads against a fake server on 127.0.0.1:
 *   - each QSO sent once; duplicates and rejections recorded; the .adi untouched
 *   - nothing re-sent on the next pass; --retry resends only rejected QSOs
 *   - server down: nothing recorded, QSOs stay pending
 *   - Wavelog JSON body, duplicate (HTTP 400 abort), bad key (401)
 *   - URL handling, QRZ key never over plain HTTP to another host
 *   - "mark" = only QSOs logged from now on are sent                        */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "net/logbook.h"

#define DIR "/tmp/qz_logbook"
static int fails, g_srv = -1, g_port, g_reqs, g_wl_bad_key;
static char g_last[16384];
#define CHECK(c, ...) do { int ok_ = (c); printf(ok_ ? "  ok    " : "  FAIL  "); printf(__VA_ARGS__); printf("\n"); if (!ok_) ++fails; } while (0)

static void reply(int c, int code, const char *body)
{
    char h[256];
    int n = snprintf(h, sizeof h, "HTTP/1.1 %d X\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n\r\n", code, strlen(body));
    send(c, h, n, 0);
    send(c, body, strlen(body), 0);
}

static void *server(void *arg)
{
    (void)arg;
    for (;;) {
        int c = accept(g_srv, NULL, NULL);
        if (c < 0) return NULL;
        char buf[16384];
        int got = 0, need = -1;
        while (got < (int)sizeof buf - 1) {
            int r = (int)recv(c, buf + got, sizeof buf - 1 - got, 0);
            if (r <= 0) break;
            got += r;
            buf[got] = 0;
            char *e = strstr(buf, "\r\n\r\n");
            if (e && need < 0) { char *cl = strcasestr(buf, "Content-Length:"); need = (int)(e + 4 - buf) + (cl ? atoi(cl + 15) : 0); }
            if (need >= 0 && got >= need) break;
        }
        buf[got] = 0;
        snprintf(g_last, sizeof g_last, "%s", buf);
        ++g_reqs;
        const char *body = strstr(buf, "\r\n\r\n");
        body = body ? body + 4 : "";
        if (!strncmp(buf, "POST /api ", 10)) {                       /* QRZ */
            if (!strstr(body, "KEY=TEST-KEY")) reply(c, 200, "RESULT=FAIL&REASON=invalid api key");
            else if (strstr(body, "ACTION=STATUS")) reply(c, 200, "RESULT=OK&DATA=BOOK_NAME%3DTest%26OWNER%3DK1ABC");
            else if (strstr(body, "K1AAA")) reply(c, 200, "RESULT=OK&LOGID=1001&COUNT=1");
            else if (strstr(body, "K2BBB")) reply(c, 200, "RESULT=FAIL&REASON=Unable to add QSO to database: duplicate&COUNT=0");
            else reply(c, 200, "RESULT=FAIL&REASON=wrong band for frequency&COUNT=0");
        } else if (!strncmp(buf, "POST /wl/index.php/api/qso ", 27)) {    /* Wavelog */
            if (g_wl_bad_key) reply(c, 401, "{\"status\":\"failed\",\"reason\":\"missing api key\"}");
            else if (strstr(body, "K1AAA")) reply(c, 201, "{\"status\":\"created\",\"type\":\"adif\",\"adif_count\":1,\"adif_errors\":0,\"messages\":[]}");
            else if (strstr(body, "K2BBB")) reply(c, 400, "{\"status\":\"abort\",\"adif_errors\":1,\"messages\":[\"\",\"Date/Time: 2026-11-27 12:00:00 Callsign: K2BBB Band: 20m Duplicate for K1ABC\"]}");
            else reply(c, 400, "{\"status\":\"abort\",\"adif_errors\":1,\"messages\":[\"Wrong station callsign\"]}");
        } else if (!strncmp(buf, "GET /wl/index.php/api/station_info/wl-key ", 42)) {
            reply(c, 200, "[{\"station_id\":\"1\",\"station_profile_name\":\"Home\",\"station_gridsquare\":\"FN31\",\"station_callsign\":\"K1ABC\",\"station_active\":\"1\"},"
                          "{\"station_id\":\"7\",\"station_profile_name\":\"Portable\",\"station_gridsquare\":\"FN42\",\"station_callsign\":\"K1ABC/P\",\"station_active\":null}]");
        } else reply(c, 404, "not found");
        close(c);
    }
}

static int listen_any(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(s, (struct sockaddr *)&a, sizeof a);
    socklen_t l = sizeof a;
    getsockname(s, (struct sockaddr *)&a, &l);
    g_port = ntohs(a.sin_port);
    listen(s, 8);
    return s;
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

static char *slurp(const char *path)
{
    static char b[4][65536];
    static int k;
    char *o = b[k++ & 3];
    o[0] = 0;
    FILE *f = fopen(path, "r");
    if (f) { size_t n = fread(o, 1, sizeof b[0] - 1, f); o[n] = 0; fclose(f); }
    return o;
}

static int lines(const char *path, const char *word)
{
    int n = 0;
    for (const char *p = slurp(path); (p = strstr(p, word)); p += strlen(word)) ++n;
    return n;
}

#define REC(call, t) "<call:5>" call " <gridsquare:4>FN42 <mode:3>FT8 <rst_sent:3>-10 <rst_rcvd:3>-12 <qso_date:8>20261127 " \
                     "<time_on:6>" t " <freq:9>14.075000 <band:3>20m <station_callsign:5>K1ABC <my_gridsquare:4>FN31 <eor>\n"

int main(void)
{
    printf("logbook uploads (QRZ / Wavelog)\n");
    if (system("rm -rf " DIR " && mkdir -p " DIR)) return 1;
    g_srv = listen_any();
    pthread_t t;
    pthread_create(&t, NULL, server, NULL);
    const char *adi = DIR "/qso-zero.adi", *conf = DIR "/logbook.conf";
    const char *log3 = "QSO-Zero ADIF export\n<adif_ver:5>3.1.4 <programid:7>QSO-Zero <eoh>\n"
                       REC("K1AAA", "120000") REC("K2BBB", "120100") REC("K3CCC", "120200")
                       "<call:5>K4DDD <mode:3>FT8 <qso_date:8>20261127";            /* still being written: ignored */
    put(adi, log3);
    char c[512];
    snprintf(c, sizeof c, "# test\nqrz_api_key = TEST-KEY\nqrz_url = http://127.0.0.1:%d/api\n"
                          "wavelog_url = http://127.0.0.1:%d/wl/\nwavelog_api_key = wl-key\nwavelog_station_id = 1\nwavelog_upload = 0\n", g_port, g_port);
    put(conf, c);

    lb_cfg_t cfg;
    CHECK(lb_cfg_load(&cfg, conf) == 0 && lb_configured(&cfg, LB_QRZ) && lb_configured(&cfg, LB_WAVELOG), "settings file read, both services set up");
    CHECK(cfg.qrz_auto == 1 && cfg.wl_auto == 0, "automatic upload on by default, off when set to 0");
    char sp[256];
    lb_sent_path(adi, LB_QRZ, sp, sizeof sp);
    CHECK(!strcmp(sp, DIR "/uploads/qrz.txt"), "QRZ record kept next to the log: %s", sp);

    /* --- QRZ --- */
    lb_stats_t st = {0};
    int r0 = g_reqs, left = lb_upload(&cfg, LB_QRZ, adi, 0, 0, &st);
    CHECK(g_reqs - r0 == 3 && left == 0, "QRZ: 3 complete QSOs sent, the unfinished record skipped (%d requests)", g_reqs - r0);
    CHECK(st.sent[LB_QRZ] == 1 && st.dupes[LB_QRZ] == 1 && st.rejected[LB_QRZ] == 1, "QRZ: 1 sent, 1 duplicate, 1 rejected");
    CHECK(lines(sp, "\tsent\t") == 1 && lines(sp, "\tdupe\t") == 1 && lines(sp, "\trejected\t") == 1, "QRZ: all three recorded in uploads/qrz.txt");
    CHECK(strstr(slurp(sp), "logid 1001") != NULL, "QRZ: logid kept");
    CHECK(!strcmp(slurp(adi), log3), "the .adi log is unchanged");
    r0 = g_reqs;
    lb_upload(&cfg, LB_QRZ, adi, 0, 0, &st);
    CHECK(g_reqs == r0, "QRZ: next pass sends nothing again");
    lb_upload(&cfg, LB_QRZ, adi, 1, 0, &st);
    CHECK(g_reqs - r0 == 1 && strstr(g_last, "K3CCC"), "QRZ: retry resends only the rejected QSO");
    CHECK(strstr(g_last, "User-Agent: QSO-Zero/") && strstr(g_last, "KEY=TEST-KEY&ACTION=INSERT&ADIF=%3Ccall%3A5%3EK3CCC"),
          "QRZ: identifiable User-Agent, KEY / ACTION / URL-encoded ADIF");

    /* a new QSO is picked up; the server being down leaves it pending */
    FILE *f = fopen(adi, "w");
    fprintf(f, "QSO-Zero ADIF export\n<adif_ver:5>3.1.4 <programid:7>QSO-Zero <eoh>\n" REC("K1AAA", "120000") REC("K2BBB", "120100") REC("K3CCC", "120200") REC("K1AAA", "130000"));
    fclose(f);
    lb_cfg_t down = cfg;
    snprintf(down.qrz_url, sizeof down.qrz_url, "http://127.0.0.1:1/api");
    lb_stats_t sd = {0};
    left = lb_upload(&down, LB_QRZ, adi, 0, 0, &sd);
    CHECK(left == 1 && sd.last_error[LB_QRZ][0] && lines(sp, "130000") == 0, "server unreachable: QSO stays pending, nothing recorded (%s)", sd.last_error[LB_QRZ]);
    r0 = g_reqs;
    left = lb_upload(&cfg, LB_QRZ, adi, 0, 0, &st);
    CHECK(left == 0 && g_reqs - r0 == 1 && lines(sp, "130000") == 1, "back online: the waiting QSO is sent");

    /* wrong key: stop, record nothing */
    lb_cfg_t bad = cfg;
    snprintf(bad.qrz_key, sizeof bad.qrz_key, "WRONG");
    put(DIR "/b.adi", "<eoh>\n" REC("K1AAA", "140000") REC("K1AAA", "140100"));
    lb_stats_t sb = {0};
    r0 = g_reqs;
    left = lb_upload(&bad, LB_QRZ, DIR "/b.adi", 0, 0, &sb);
    CHECK(left == 2 && g_reqs - r0 == 1 && strstr(sb.last_error[LB_QRZ], "API key"), "bad QRZ key: stops after one try, both QSOs wait (%s)", sb.last_error[LB_QRZ]);

    /* QRZ key never sent over plain HTTP except to this machine */
    snprintf(c, sizeof c, "qrz_api_key = X\nqrz_url = http://example.com/api\n");
    put(DIR "/evil.conf", c);
    lb_cfg_t ev;
    lb_cfg_load(&ev, DIR "/evil.conf");
    CHECK(!strcmp(ev.qrz_url, "https://logbook.qrz.com/api"), "qrz_url over plain HTTP to another host is ignored");

    /* --- Wavelog --- */
    char wp[256];
    lb_sent_path(adi, LB_WAVELOG, wp, sizeof wp);
    lb_stats_t sw = {0};
    r0 = g_reqs;
    left = lb_upload(&cfg, LB_WAVELOG, adi, 0, 0, &sw);
    CHECK(g_reqs - r0 == 4 && left == 0, "Wavelog: 4 QSOs sent");
    CHECK(sw.sent[LB_WAVELOG] == 2 && sw.dupes[LB_WAVELOG] == 1 && sw.rejected[LB_WAVELOG] == 1, "Wavelog: 2 created, 1 duplicate (HTTP 400), 1 rejected");
    CHECK(strstr(slurp(wp), "Wrong station callsign") != NULL, "Wavelog: the rejection reason is recorded");
    CHECK(strstr(g_last, "Content-Type: application/json") && strstr(g_last, "\"station_profile_id\":\"1\",\"type\":\"adif\",\"string\":\"<call:5>K1AAA")
          && strstr(g_last, "\"key\":\"wl-key\""), "Wavelog: JSON body with key, station_profile_id, type adif, ADIF string");
    g_wl_bad_key = 1;
    put(DIR "/w.adi", "<eoh>\n" REC("K1AAA", "150000"));
    lb_stats_t s2 = {0};
    left = lb_upload(&cfg, LB_WAVELOG, DIR "/w.adi", 0, 0, &s2);
    CHECK(left == 1 && strstr(s2.last_error[LB_WAVELOG], "missing api key"), "Wavelog: HTTP 401 stops and keeps the QSO waiting (%s)", s2.last_error[LB_WAVELOG]);
    g_wl_bad_key = 0;

    char u[256];
    lb_wavelog_url("https://log.example.com", "qso", u, sizeof u);
    CHECK(!strcmp(u, "https://log.example.com/index.php/api/qso"), "URL: %s", u);
    lb_wavelog_url("https://ex.org/wavelog/index.php/", "qso", u, sizeof u);
    CHECK(!strcmp(u, "https://ex.org/wavelog/index.php/api/qso"), "URL with index.php: %s", u);
    lb_wavelog_url("https://ex.org/index.php/api/qso", "station_info/", u, sizeof u);
    CHECK(!strcmp(u, "https://ex.org/index.php/api/station_info/"), "full API URL pasted: %s", u);

    /* command line */
    fflush(stdout);
    char *tv[] = {"test", "wavelog"};
    CHECK(logbook_cli(2, tv, conf, adi) == 0, "--logbook test wavelog: key OK, station 1 found");
    char *tq[] = {"test", "qrz"};
    CHECK(logbook_cli(2, tq, conf, adi) == 0, "--logbook test qrz: key OK");
    snprintf(c, sizeof c, "wavelog_url = http://127.0.0.1:%d/wl\nwavelog_api_key = wl-key\nwavelog_station_id = 3\n", g_port);
    put(DIR "/w3.conf", c);
    CHECK(logbook_cli(2, tv, DIR "/w3.conf", adi) != 0, "--logbook test: station 3 not one of the key's -> error");

    put(DIR "/m.adi", "<eoh>\n" REC("K1AAA", "160000") REC("K1AAA", "160100"));
    char *mk[] = {"mark", "qrz"};
    logbook_cli(2, mk, conf, DIR "/m.adi");
    r0 = g_reqs;
    lb_stats_t sm = {0};
    lb_upload(&cfg, LB_QRZ, DIR "/m.adi", 0, 0, &sm);
    CHECK(g_reqs == r0, "mark: QSOs already in the log are not sent");
    f = fopen(DIR "/m.adi", "a");
    fputs(REC("K1AAA", "170000"), f);
    fclose(f);
    lb_upload(&cfg, LB_QRZ, DIR "/m.adi", 0, 0, &sm);
    CHECK(g_reqs - r0 == 1 && strstr(g_last, "170000"), "mark: a QSO logged afterwards is sent");

    /* the app's background uploader: a pass at start, then one per logged QSO */
    if (system("rm -rf " DIR "/bg && mkdir -p " DIR "/bg")) return 1;
    snprintf(c, sizeof c, "qrz_api_key = TEST-KEY\nqrz_url = http://127.0.0.1:%d/api\n", g_port);
    put(DIR "/bg/lb.conf", c);
    put(DIR "/bg/qso-zero.adi", "<eoh>\n" REC("K1AAA", "180000"));
    r0 = g_reqs;
    logbook_start(DIR "/bg/lb.conf", DIR "/bg/qso-zero.adi");
    lb_stats_t sg = {0};
    for (int i = 0; i < 50 && !sg.last_pass; ++i) { usleep(100000); logbook_get(&sg); }
    CHECK(g_reqs - r0 == 1 && sg.configured[LB_QRZ] && !sg.configured[LB_WAVELOG] && sg.sent[LB_QRZ] == 1, "background: first pass sends the waiting QSO");
    f = fopen(DIR "/bg/qso-zero.adi", "a");
    fputs(REC("K1AAA", "181500"), f);
    fclose(f);
    double lp = sg.last_pass;
    logbook_kick();
    for (int i = 0; i < 50 && sg.last_pass == lp; ++i) { usleep(100000); logbook_get(&sg); }
    CHECK(g_reqs - r0 == 2 && strstr(g_last, "181500") && sg.sent[LB_QRZ] == 2 && sg.pending[LB_QRZ] == 0,
          "background: a newly logged QSO goes straight away (kick)");
    logbook_stop();

    lb_cfg_t none;
    CHECK(lb_cfg_load(&none, DIR "/missing.conf") != 0 && !lb_configured(&none, LB_QRZ) && !lb_configured(&none, LB_WAVELOG),
          "no settings file: nothing set up, nothing sent");

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
