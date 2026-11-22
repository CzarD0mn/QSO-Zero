/* Keyboard handling with real evdev records fed through a FIFO:
 * short ESC = K_ESC (on release), held ESC = K_ESC_HOLD after ESC_HOLD_S,
 * and the Cardputer Zero's Sym layer from its stock keymap file
 * (tests/data/tca8418_keypad_m5stack_keymap.map, copied from a device). */
#include <stdlib.h>
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
    setenv("QSOZERO_KEYMAP", "tests/data/tca8418_keypad_m5stack_keymap.map", 1);
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

    /* --- Cardputer Zero keypad: the FIFO has no device name, and the keymap
     * file is loaded, so it counts as the built-in keypad --- */
    CHECK(input_keymap_from_file() && input_device_builtin(0), "keymap file read; device treated as the Zero keypad");
    static const struct { int code; char ch; } sym[] = {
        {26, '!'}, {27, '@'}, {39, '#'}, {40, '$'}, {41, '%'}, {43, '^'}, {51, '&'}, {52, '*'},
        {53, '('}, {94, ')'}, {55, '~'}, {69, '`'}, {70, '_'}, {71, '-'}, {72, '+'}, {73, '='},
        {74, '['}, {75, ']'}, {76, '{'}, {77, '}'}, {79, ';'}, {80, ':'}, {81, '\''}, {82, '"'},
        {83, '<'}, {85, '>'}, {86, '\\'}, {89, '|'}, {90, ','}, {91, '.'}, {92, '/'}, {93, '?'},
    };
    int okc = 0;
    char bad[64] = "";
    for (size_t i = 0; i < sizeof sym / sizeof *sym; ++i) {
        key(sym[i].code, 1); key(sym[i].code, 0);
        n = collect(30, ev, 16);
        if (n == 1 && ev[0].key == K_CHAR && ev[0].ch == sym[i].ch) ++okc;
        else snprintf(bad + strlen(bad), sizeof bad - strlen(bad), " %d", sym[i].code);
    }
    CHECK(okc == 32, "all 32 Sym symbols type what the keymap says (%d/32)%s", okc, bad);
    key(43, 1); key(43, 0); key(53, 1); key(53, 0); key(92, 1); key(92, 0);
    n = collect(80, ev, 16);
    CHECK(n == 3 && ev[0].ch == '^' && ev[1].ch == '(' && ev[2].ch == '/', "Sym+6 = ^, Sym+9 = (, slash = / (were \\ and / before)");
    key(KEY_LEFTSHIFT, 1); key(70, 1); key(70, 0); key(KEY_A, 1); key(KEY_A, 0); key(KEY_LEFTSHIFT, 0);
    key(KEY_6, 1); key(KEY_6, 0);
    n = collect(80, ev, 16);
    CHECK(n == 3 && ev[0].ch == '_' && ev[1].ch == 'A' && ev[2].ch == '6', "Shift leaves Sym symbols alone, still capitalises letters; digits plain");
    key(41, 1); usleep(50000); key(41, 0);
    n = collect(150, ev, 16);
    CHECK(n == 1 && ev[0].key == K_CHAR && ev[0].ch == '%', "code 41 is Sym '%%', not an ESC alias any more");

    /* --- a USB keyboard on the same Zero: ordinary US layout --- */
    input_set_builtin(0, 0);
    key(26, 1); key(26, 0); key(43, 1); key(43, 0); key(KEY_SLASH, 1); key(KEY_SLASH, 0);
    key(KEY_LEFTSHIFT, 1); key(KEY_GRAVE, 1); key(KEY_GRAVE, 0); key(KEY_LEFTSHIFT, 0);
    n = collect(100, ev, 16);
    CHECK(n == 4 && ev[0].ch == '[' && ev[1].ch == '\\' && ev[2].ch == '/' && ev[3].ch == '~', "USB keyboard: [ \\ / and Shift+` = ~");
    input_set_builtin(0, 1);

    /* --- no keymap file: the built-in table is used --- */
    CHECK(input_load_keymap("/nonexistent") == 0, "missing keymap file -> fallback table");
    key(43, 1); key(43, 0); key(92, 1); key(92, 0);
    n = collect(60, ev, 16);
    CHECK(n == 2 && ev[0].ch == '^' && ev[1].ch == '/', "fallback table gives the same symbols");

    key(KEY_LEFTCTRL, 1); key(KEY_X, 1); key(KEY_X, 0); key(KEY_LEFTCTRL, 0);
    n = collect(150, ev, 16);
    CHECK(n == 1 && ev[0].key == K_CHAR && ev[0].ch == 'x' && (ev[0].mods & MOD_CTRL), "Ctrl+X (halt TX) still arrives");

    /* --- hold to scroll: the keypad sends no autorepeat, so we make it --- */
    input_set_repeat(400, 60);
    key(KEY_DOWN, 1); key(KEY_DOWN, 0);
    n = collect(700, ev, 16);
    CHECK(n == 1 && ev[0].key == K_DOWN, "tap down: one step (%d)", n);
    keyev_t big[64];
    key(KEY_DOWN, 1);
    n = collect(350, big, 64);
    CHECK(n == 1, "held down 350 ms: still one step, under the 400 ms delay (%d)", n);
    n += collect(650, big + n, 64 - n);
    int alld = 1;
    for (int i = 0; i < n; ++i) alld &= big[i].key == K_DOWN;
    CHECK(alld && n >= 9 && n <= 13, "held down 1 s: 1 + ~10 repeats at 60 ms (%d)", n);
    key(KEY_DOWN, 0);
    n = collect(300, big, 64);
    CHECK(n == 0, "released: repeats stop (%d)", n);
    key(KEY_UP, 1); n = collect(200, big, 64); key(KEY_ENTER, 1); n += collect(500, big + n, 64 - n);
    key(KEY_ENTER, 0); key(KEY_UP, 0); collect(100, big, 64);
    CHECK(n == 2 && big[0].key == K_UP && big[1].key == K_ENTER, "another key while holding up stops the repeat (%d)", n);
    key(KEY_PAGEDOWN, 1); n = collect(900, big, 64); key(KEY_PAGEDOWN, 0); collect(100, big + n, 64 - n);
    CHECK(n == 1, "PgDn (band step) never repeats (%d)", n);
    /* a keyboard with its own autorepeat: ours stays out of the way */
    key(KEY_DOWN, 1); n = collect(300, big, 64);
    for (int i = 0; i < 8; ++i) { key(KEY_DOWN, 2); n += collect(100, big + n, 64 - n); }
    key(KEY_DOWN, 0); n += collect(200, big + n, 64 - n);
    CHECK(n == 9, "kernel autorepeat: 1 + 8 of its repeats, none of ours (%d)", n);

    input_close();
    close(wfd);
    unlink(path);
    printf(fails ? "FAIL\n" : "PASS\n");
    return fails != 0;
}
