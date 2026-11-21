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
    snprintf(p, sizeof p, "rm -rf %s", home);
    if (system(p)) {}
    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}
