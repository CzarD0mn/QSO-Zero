/* Activity log: file location, line format, previous run kept as .1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/log.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok    " __VA_ARGS__); else { printf("  FAIL  " __VA_ARGS__); ++fails; } printf("\n"); } while (0)

static int file_has(const char *path, const char *text)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[600];
    int found = 0;
    while (fgets(line, sizeof line, f)) if (strstr(line, text)) found = 1;
    fclose(f);
    return found;
}

int main(void)
{
    char home[] = "/tmp/qsoz-logXXXXXX", p[300], p1[310];
    if (!mkdtemp(home)) return 1;
    setenv("HOME", home, 1);
    unsetenv("XDG_CACHE_HOME");
    qlog("test", "before open: ignored");
    CHECK(log_open(NULL) == 0, "opened %s", log_path());
    snprintf(p, sizeof p, "%s/.cache/qso-zero/qso-zero.log", home);
    CHECK(!strcmp(log_path(), p), "default location ~/.cache/qso-zero/qso-zero.log");
    qlog("audio", "input %s open", "plughw:CARD=Device,DEV=0");
    CHECK(file_has(p, "Z [audio] input plughw:CARD=Device,DEV=0 open"), "timestamped [area] lines, written straight away");
    CHECK(!file_has(p, "before open"), "nothing logged before log_open");
    log_close();
    log_open(NULL);
    qlog("start", "second run");
    snprintf(p1, sizeof p1, "%s.1", p);
    CHECK(file_has(p1, "[audio] input") && file_has(p, "second run") && !file_has(p, "[audio] input"), "previous run kept as qso-zero.log.1");
    log_close();

    /* seven runs: only the newest five logs are kept, newest = no suffix, then .1 .. .4 */
    for (int r = 3; r <= 7; ++r) { log_open(NULL); qlog("start", "run %d", r); log_close(); }
    char q[320], want[32];
    int ok = 1;
    for (int k = 0; k <= 4; ++k) {
        if (k) snprintf(q, sizeof q, "%s.%d", p, k); else snprintf(q, sizeof q, "%s", p);
        snprintf(want, sizeof want, "run %d", 7 - k);
        ok &= file_has(q, want);
    }
    snprintf(q, sizeof q, "%s.5", p);
    FILE *f5 = fopen(q, "r");
    CHECK(ok && !f5, "7 runs -> qso-zero.log (run 7) + .1 .. .4 (runs 6..3), nothing older");
    if (f5) fclose(f5);

    /* one long run: past 1 MB the file rotates, still at most 5 files */
    log_open(NULL);
    char big[400];
    memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0;
    for (int i = 0; i < 12000; ++i) qlog("audio", "%s", big);      /* ~4.9 MB */
    qlog("start", "end of long run");
    log_close();
    long total = 0, biggest = 0;
    int files = 0;
    for (int k = 0; k <= 5; ++k) {
        if (k) snprintf(q, sizeof q, "%s.%d", p, k); else snprintf(q, sizeof q, "%s", p);
        FILE *f = fopen(q, "r");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        ++files; total += sz; if (sz > biggest) biggest = sz;
    }
    CHECK(files == 5 && biggest < 1100000 && total < 5600000 && file_has(p, "end of long run"),
          "1 MB cap: %d files, largest %ld KB, total %ld KB", files, biggest / 1024, total / 1024);
    snprintf(p, sizeof p, "rm -rf %s", home);
    if (system(p)) {}
    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}
