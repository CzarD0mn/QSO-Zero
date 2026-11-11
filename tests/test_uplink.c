/* wsprnet uploader against local servers:
 *   A  HTTPS (trusted cert) up            -> uploads over HTTPS
 *   B  HTTPS cert for the wrong host      -> falls back to HTTP
 *   C  HTTPS port closed                  -> falls back to HTTP
 *   D  nothing reachable                  -> offline, spots dropped      */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "core/clock.h"
#include "net/uplink.h"

#define DIR "/tmp/nq_tls"
static int g_srv = -1, g_http_port, g_https_port, g_reqs;
static char g_last[1024];
static const char *rc(void) { return "KN4CRD"; }
static const char *rg(void) { return "EM73"; }
static int fails;
#define CHECK(c, ...) do { printf((c) ? "  ok    " : "  FAIL  "); printf(__VA_ARGS__); printf("\n"); if (!(c)) ++fails; } while (0)

static void *http_server(void *arg)
{
    (void)arg;
    for (;;) {
        int c = accept(g_srv, NULL, NULL);
        if (c < 0) return NULL;
        char buf[1024] = "";
        ssize_t n = recv(c, buf, sizeof buf - 1, 0);
        if (n > 0) { buf[n] = 0; snprintf(g_last, sizeof g_last, "%s", buf); ++g_reqs;
                     const char *ok = "HTTP/1.0 200 OK\r\n\r\n"; send(c, ok, strlen(ok), 0); }
        close(c);
    }
}

static int free_port(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(s, (struct sockaddr *)&a, sizeof a);
    socklen_t l = sizeof a;
    getsockname(s, (struct sockaddr *)&a, &l);
    close(s);
    return ntohs(a.sin_port);
}

static int lines(const char *path)
{
    FILE *f = fopen(path, "r");
    int n = 0, c;
    if (!f) return 0;
    while ((c = fgetc(f)) != EOF) n += c == '\n';
    fclose(f);
    return n;
}

static pid_t start_tls(const char *cert)
{
    char cmd[512];
    remove(DIR "/srv.out");                 /* don't mistake the previous server's "ready" */
    snprintf(cmd, sizeof cmd, "python3 tests/tls_server.py %d " DIR "/%s.crt " DIR "/%s.key " DIR "/https.log > " DIR "/srv.out 2>&1 & echo $!",
             g_https_port, cert, cert);
    FILE *p = popen(cmd, "r");
    int pid = 0;
    if (fscanf(p, "%d", &pid) != 1) pid = 0;
    pclose(p);
    for (int i = 0; i < 50; ++i) {          /* wait for "ready" */
        FILE *f = fopen(DIR "/srv.out", "r");
        char b[16] = "";
        if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; fclose(f); }
        if (!strncmp(b, "ready", 5)) break;
        usleep(100000);
    }
    return pid;
}

static void stop_tls(pid_t p) { if (p > 0) { kill(p, SIGTERM); usleep(300000); } }

static uplink_stats_t run_cycle(int nspots)
{
    uplink_spot_t s = {.utc = 1762862400, .dial_hz = 14095600, .audio_hz = 1501.2f, .snr = -25, .dbm = 37, .dt = 0.3f};
    strcpy(s.call, "G4JNT"); strcpy(s.grid, "IO90");
    for (int i = 0; i < nspots; ++i) uplink_add_wspr(&s);
    uplink_stats_t u;
    uplink_get(&u);
    double before = u.last_check;
    uplink_check_now();
    for (int i = 0; i < 300; ++i) { usleep(50000); uplink_get(&u); if (u.last_check > before) break; }
    usleep(100000);
    uplink_get(&u);
    return u;
}

int main(void)
{
    clk_init();
    /* throwaway CA + a cert for "localhost" + a cert for the wrong name */
    if (system("rm -rf " DIR " && mkdir -p " DIR " && cd " DIR " && "
               "openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.crt -days 2 -subj /CN=NQ-test-CA 2>/dev/null && "
               "for n in localhost wronghost; do "
               "openssl req -newkey rsa:2048 -nodes -keyout $n.key -out $n.csr -subj /CN=$n 2>/dev/null && "
               "printf 'subjectAltName=DNS:%s' $n > $n.ext && "
               "openssl x509 -req -in $n.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out $n.crt -days 2 -extfile $n.ext 2>/dev/null; done")) {
        printf("cannot create test certificates\n");
        return 1;
    }
    g_https_port = free_port();
    g_srv = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(g_srv, (struct sockaddr *)&a, sizeof a);
    socklen_t al = sizeof a;
    getsockname(g_srv, (struct sockaddr *)&a, &al);
    g_http_port = ntohs(a.sin_port);
    listen(g_srv, 8);
    pthread_t t;
    pthread_create(&t, NULL, http_server, NULL);

    uplink_set_ca_file(DIR "/ca.crt");
    pid_t tls = start_tls("localhost");
    uplink_start("localhost", g_https_port, g_http_port, rc, rg);

    printf("[A] HTTPS available with a valid certificate\n");
    uplink_stats_t u = run_cycle(2);
    CHECK(u.online && u.sent_https == 2 && u.sent_http == 0, "2 spots over HTTPS (https %d, http %d)", u.sent_https, u.sent_http);
    CHECK(lines(DIR "/https.log") == 2 && g_reqs == 0, "HTTPS server got both, HTTP server none");
    FILE *f = fopen(DIR "/https.log", "r");
    char req[512] = "";
    if (f) { if (!fgets(req, sizeof req, f)) req[0] = 0; fclose(f); }
    CHECK(strstr(req, "/post?function=wspr&rcall=KN4CRD&rgrid=EM73&rqrg=14.097101") && strstr(req, "&tcall=G4JNT&tgrid=IO90&dbm=37"),
          "same wsprnet request over TLS");
    stop_tls(tls);

    printf("[B] HTTPS certificate does not match the host\n");
    tls = start_tls("wronghost");
    u = run_cycle(2);
    CHECK(u.sent_https == 2 && u.sent_http == 2, "rejected the bad certificate, both sent over HTTP (http %d)", u.sent_http);
    CHECK(g_reqs == 2 && lines(DIR "/https.log") == 2, "no request leaked to the bad-cert server");
    CHECK(strstr(u.last_error, "certificate") != NULL, "failure was the certificate check: \"%s\"", u.last_error);
    stop_tls(tls);

    printf("[C] HTTPS port closed\n");
    u = run_cycle(3);
    CHECK(u.online && u.sent_http == 5 && g_reqs == 5, "fell back to HTTP for all 3 (http total %d)", u.sent_http);
    printf("        %.*s\n", (int)(strchr(g_last, '\r') - g_last), g_last);

    printf("[D] no internet at all\n");
    shutdown(g_srv, SHUT_RDWR);
    pthread_join(t, NULL);
    close(g_srv);
    u = run_cycle(4);
    CHECK(!u.online, "check reports offline");
    CHECK(u.dropped == 4 && u.sent_http == 5 && u.sent_https == 2, "4 spots dropped, nothing sent");
    uplink_stop();
    printf(fails ? "FAIL\n" : "PASS\n");
    return fails != 0;
}
