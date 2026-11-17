/* Keyboard handling with real evdev records fed through a FIFO:
 * short ESC = K_ESC (on release), held ESC = K_ESC_HOLD after ESC_HOLD_S,
 * the Cardputer's ` key behaves as ESC, Shift+` types '~'. */
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "ui/input.h"

static int wfd, fails;
#define CHECK(c, ...) do { printf((c) ? "  ok    " : "  FAIL  "); printf(__VA_ARGS__); printf("\n"); if (!(c)) ++fails; } while (0)

static void key(int code, int value)
{
    struct input_event e = {.type = EV_KEY, .code = (unsigned short)code, .value = value};
    if (write(wfd, &e, sizeof e) != sizeof e) perror("write");
}

/* collect events for `ms` milliseconds */
static int collect(int ms, keyev_t *out, int max)
{
    int n = 0;
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        keyev_t ev;
        if (input_poll(&ev, 10) && n < max) out[n++] = ev;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 >= ms) return n;
    }
}

int main(void)
{
    const char *path = "/tmp/qz_kbd_fifo";
    unlink(path);
    mkfifo(path, 0600);
    if (input_open(path)) { printf("cannot open fifo\n"); return 1; }
    wfd = open(path, O_WRONLY);
    keyev_t ev[16];
    int n;

    key(KEY_ESC, 1); usleep(300000); key(KEY_ESC, 0);
    n = collect(200, ev, 16);
    CHECK(n == 2 && ev[0].key == K_ESC_HOLDING && ev[1].key == K_ESC, "short ESC: HOLDING then ESC on release (%d events)", n);

    key(KEY_ESC, 1);
    n = 0;
    for (int i = 0; i < 6; ++i) { n += collect(200, ev + n, 16 - n); key(KEY_ESC, 2); }  /* held, autorepeating */
    key(KEY_ESC, 0);
    n += collect(200, ev + n, 16 - n);
    CHECK(n == 2 && ev[0].key == K_ESC_HOLDING && ev[1].key == K_ESC_HOLD, "held ESC 1.2 s: HOLDING then HOLD, nothing on release (%d events)", n);

    key(KEY_GRAVE, 1); usleep(100000); key(KEY_GRAVE, 0);
    n = collect(200, ev, 16);
    CHECK(n == 2 && ev[1].key == K_ESC, "Cardputer ` key acts as ESC");

    key(KEY_LEFTSHIFT, 1); key(KEY_GRAVE, 1); key(KEY_GRAVE, 0); key(KEY_LEFTSHIFT, 0);
    n = collect(150, ev, 16);
    CHECK(n == 1 && ev[0].key == K_CHAR && ev[0].ch == '~', "Shift+` types '~'");

    key(KEY_LEFTCTRL, 1); key(KEY_X, 1); key(KEY_X, 0); key(KEY_LEFTCTRL, 0);
    n = collect(150, ev, 16);
    CHECK(n == 1 && ev[0].key == K_CHAR && ev[0].ch == 'x' && (ev[0].mods & MOD_CTRL), "Ctrl+X (halt TX) still arrives");

    input_close();
    close(wfd);
    unlink(path);
    printf(fails ? "FAIL\n" : "PASS\n");
    return fails != 0;
}
