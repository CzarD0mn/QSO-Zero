#include "input.h"
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAXDEV 8
static int g_fd[MAXDEV], g_n;
static int g_mods;
static int g_esc_code = -1, g_esc_fired;     /* key currently acting as ESC */
static double g_esc_t0;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

#define BITS_PER_LONG (sizeof(long) * 8)
#define TESTBIT(b, a) ((a[(b) / BITS_PER_LONG] >> ((b) % BITS_PER_LONG)) & 1)

static int is_keyboard(int fd)
{
    unsigned long ev[EV_MAX / BITS_PER_LONG + 1] = {0}, keys[KEY_MAX / BITS_PER_LONG + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof ev), ev) < 0 || !TESTBIT(EV_KEY, ev)) return 0;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0) return 0;
    return TESTBIT(KEY_A, keys) && TESTBIT(KEY_ENTER, keys);
}

int input_open(const char *dev)
{
    g_n = 0;
    if (dev && strcmp(dev, "auto")) {
        int fd = open(dev, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) g_fd[g_n++] = fd;
        return g_n ? 0 : -1;
    }
    DIR *d = opendir("/dev/input");
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) && g_n < MAXDEV) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char p[300];
        snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
        int fd = open(p, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (is_keyboard(fd)) g_fd[g_n++] = fd;
        else close(fd);
    }
    closedir(d);
    return g_n ? 0 : -1;
}

void input_close(void)
{
    for (int i = 0; i < g_n; ++i) close(g_fd[i]);
    g_n = 0;
}

/* US layout */
static const char k_lo[KEY_SLASH + 1] = {
    [KEY_1] = '1', [KEY_2] = '2', [KEY_3] = '3', [KEY_4] = '4', [KEY_5] = '5', [KEY_6] = '6',
    [KEY_7] = '7', [KEY_8] = '8', [KEY_9] = '9', [KEY_0] = '0', [KEY_MINUS] = '-', [KEY_EQUAL] = '=',
    [KEY_Q] = 'q', [KEY_W] = 'w', [KEY_E] = 'e', [KEY_R] = 'r', [KEY_T] = 't', [KEY_Y] = 'y',
    [KEY_U] = 'u', [KEY_I] = 'i', [KEY_O] = 'o', [KEY_P] = 'p', [KEY_LEFTBRACE] = '[', [KEY_RIGHTBRACE] = ']',
    [KEY_A] = 'a', [KEY_S] = 's', [KEY_D] = 'd', [KEY_F] = 'f', [KEY_G] = 'g', [KEY_H] = 'h',
    [KEY_J] = 'j', [KEY_K] = 'k', [KEY_L] = 'l', [KEY_SEMICOLON] = ';', [KEY_APOSTROPHE] = '\'',
    [KEY_GRAVE] = '`', [KEY_BACKSLASH] = '\\', [KEY_Z] = 'z', [KEY_X] = 'x', [KEY_C] = 'c',
    [KEY_V] = 'v', [KEY_B] = 'b', [KEY_N] = 'n', [KEY_M] = 'm', [KEY_COMMA] = ',', [KEY_DOT] = '.',
    [KEY_SLASH] = '/',
};
static const char k_hi[KEY_SLASH + 1] = {
    [KEY_1] = '!', [KEY_2] = '@', [KEY_3] = '#', [KEY_4] = '$', [KEY_5] = '%', [KEY_6] = '^',
    [KEY_7] = '&', [KEY_8] = '*', [KEY_9] = '(', [KEY_0] = ')', [KEY_MINUS] = '_', [KEY_EQUAL] = '+',
    [KEY_LEFTBRACE] = '{', [KEY_RIGHTBRACE] = '}', [KEY_SEMICOLON] = ':', [KEY_APOSTROPHE] = '"',
    [KEY_GRAVE] = '~', [KEY_BACKSLASH] = '|', [KEY_COMMA] = '<', [KEY_DOT] = '>', [KEY_SLASH] = '?',
};

static int translate(int code, keyev_t *ev)
{
    ev->mods = g_mods;
    ev->ch = 0;
    switch (code) {
    case KEY_ENTER: case KEY_KPENTER: ev->key = K_ENTER; return 1;
    case KEY_BACKSPACE: ev->key = K_BKSP; return 1;
    case KEY_TAB: ev->key = K_TAB; return 1;
    case KEY_ESC: ev->key = K_ESC; return 1;
    case KEY_UP: ev->key = K_UP; return 1;
    case KEY_DOWN: ev->key = K_DOWN; return 1;
    case KEY_LEFT: ev->key = K_LEFT; return 1;
    case KEY_RIGHT: ev->key = K_RIGHT; return 1;
    case KEY_PAGEUP: ev->key = K_PGUP; return 1;
    case KEY_PAGEDOWN: ev->key = K_PGDN; return 1;
    case KEY_DELETE: ev->key = K_DEL; return 1;
    case KEY_HOME: ev->key = K_HOME; return 1;
    case KEY_END: ev->key = K_END; return 1;
    case KEY_F1: ev->key = K_F1; return 1;
    case KEY_F2: ev->key = K_F2; return 1;
    case KEY_SPACE: ev->key = K_CHAR; ev->ch = ' '; return 1;
    }
    if (code >= 0 && code <= KEY_SLASH && k_lo[code]) {
        int sh = g_mods & MOD_SHIFT;
        char c = sh && k_hi[code] ? k_hi[code] : k_lo[code];
        if (sh && c >= 'a' && c <= 'z') c -= 32;
        /* The Cardputer's top-left key is ` / ESC: plain ` acts as ESC */
        if (code == KEY_GRAVE && !sh && !(g_mods & MOD_CTRL)) { ev->key = K_ESC; return 1; }
        ev->key = K_CHAR;
        ev->ch = (g_mods & MOD_CTRL) && c >= 'A' && c <= 'Z' ? c + 32 : c;
        return 1;
    }
    return 0;
}

static int esc_hold_due(keyev_t *ev)
{
    if (g_esc_code >= 0 && !g_esc_fired && now_s() - g_esc_t0 >= ESC_HOLD_S) {
        g_esc_fired = 1;
        *ev = (keyev_t){.key = K_ESC_HOLD, .mods = g_mods};
        return 1;
    }
    return 0;
}

int input_poll(keyev_t *ev, int timeout_ms)
{
    struct pollfd p[MAXDEV];
    if (esc_hold_due(ev)) return 1;
    if (g_esc_code >= 0 && timeout_ms > 20) timeout_ms = 20;   /* watch the hold timer */
    for (int i = 0; i < g_n; ++i) p[i] = (struct pollfd){.fd = g_fd[i], .events = POLLIN};
    if (g_n == 0) { usleep(timeout_ms * 1000); return 0; }
    if (poll(p, g_n, timeout_ms) <= 0) return esc_hold_due(ev);
    for (int i = 0; i < g_n; ++i) {
        if (!(p[i].revents & POLLIN)) continue;
        struct input_event e;
        while (read(g_fd[i], &e, sizeof e) == sizeof e) {
            if (e.type != EV_KEY) continue;
            int m = 0;
            switch (e.code) {
            case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: m = MOD_SHIFT; break;
            case KEY_LEFTCTRL: case KEY_RIGHTCTRL: m = MOD_CTRL; break;
            case KEY_LEFTALT: case KEY_RIGHTALT: m = MOD_ALT; break;
            case KEY_FN: m = MOD_FN; break;
            }
            if (m) { if (e.value) g_mods |= m; else g_mods &= ~m; continue; }
            /* ESC (or the Cardputer's plain ` key): short press = back, hold = exit */
            if (e.code == g_esc_code) {
                if (e.value == 0) {
                    int fired = g_esc_fired;
                    g_esc_code = -1;
                    if (!fired) { *ev = (keyev_t){.key = K_ESC, .mods = g_mods}; return 1; }
                }
                continue;                               /* autorepeat while held */
            }
            if (e.value == 1 && (e.code == KEY_ESC || (e.code == KEY_GRAVE && !(g_mods & (MOD_SHIFT | MOD_CTRL))))) {
                g_esc_code = e.code;
                g_esc_t0 = now_s();
                g_esc_fired = 0;
                *ev = (keyev_t){.key = K_ESC_HOLDING, .mods = g_mods};
                return 1;
            }
            if (e.value == 0) continue;                 /* release */
            if (translate(e.code, ev)) return 1;        /* press or autorepeat */
        }
    }
    return esc_hold_due(ev);
}
