#include "input.h"
#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAXDEV 8
static int g_fd[MAXDEV], g_n;
static int g_builtin[MAXDEV];                 /* the Cardputer Zero's own keypad (TCA8418) */
static char g_names[MAXDEV][64];

/* Cardputer Zero Sym layer. The TCA8418 keypad driver sends each Sym symbol
 * as its own key code, and the stock console keymap
 * /usr/share/keymaps/tca8418_keypad_m5stack_keymap.map turns those codes
 * into characters. Apps that read evdev directly must do the same, or e.g.
 * Sym+6 (code 43, '^') reads as US '\'. Loaded from that file when present;
 * this table (the stock file's 32 entries) is the fallback. Applied only to
 * the built-in keypad: a USB keyboard's code 26 is still '['. */
#define ZERO_KEYMAP "/usr/share/keymaps/tca8418_keypad_m5stack_keymap.map"
static char g_sym[KEY_MAX + 1];
static int g_keymap_from_file;
static const struct { unsigned char code; char ch; } k_zero_sym[] = {
    {26, '!'}, {27, '@'}, {39, '#'}, {40, '$'}, {41, '%'}, {43, '^'}, {51, '&'}, {52, '*'},
    {53, '('}, {94, ')'}, {55, '~'}, {69, '`'}, {70, '_'}, {71, '-'}, {72, '+'}, {73, '='},
    {74, '['}, {75, ']'}, {76, '{'}, {77, '}'}, {79, ';'}, {80, ':'}, {81, '\''}, {82, '"'},
    {83, '<'}, {85, '>'}, {86, '\\'}, {89, '|'}, {90, ','}, {91, '.'}, {92, '/'}, {93, '?'},
};
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

static const struct { const char *name; char ch; } k_keysyms[] = {
    {"exclam", '!'}, {"at", '@'}, {"numbersign", '#'}, {"dollar", '$'}, {"percent", '%'},
    {"asciicircum", '^'}, {"ampersand", '&'}, {"asterisk", '*'}, {"parenleft", '('}, {"parenright", ')'},
    {"asciitilde", '~'}, {"grave", '`'}, {"underscore", '_'}, {"minus", '-'}, {"plus", '+'}, {"equal", '='},
    {"bracketleft", '['}, {"bracketright", ']'}, {"braceleft", '{'}, {"braceright", '}'},
    {"semicolon", ';'}, {"colon", ':'}, {"apostrophe", '\''}, {"quotedbl", '"'}, {"less", '<'},
    {"greater", '>'}, {"backslash", '\\'}, {"bar", '|'}, {"comma", ','}, {"period", '.'},
    {"slash", '/'}, {"question", '?'}, {"space", ' '},
    {"zero", '0'}, {"one", '1'}, {"two", '2'}, {"three", '3'}, {"four", '4'},
    {"five", '5'}, {"six", '6'}, {"seven", '7'}, {"eight", '8'}, {"nine", '9'},
};

static char keysym_char(const char *name)
{
    if (name[0] && !name[1] && isgraph((unsigned char)name[0])) return name[0];
    for (size_t i = 0; i < sizeof k_keysyms / sizeof *k_keysyms; ++i)
        if (!strcmp(k_keysyms[i].name, name)) return k_keysyms[i].ch;
    return 0;
}

static const char k_lo[KEY_SLASH + 1];

/* "keycode 43 = asciicircum   # ..." -> g_sym[43] = '^'. Lines with a
 * modifier prefix (shift/altgr/...) and codes whose character is just the
 * normal US one are left to the regular handling (so Shift keeps working). */
int input_load_keymap(const char *path)
{
    memset(g_sym, 0, sizeof g_sym);
    g_keymap_from_file = 0;
    FILE *f = path ? fopen(path, "r") : NULL;
    int n = 0;
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *h = strchr(line, '#');
            if (h) *h = 0;
            char *p = line;
            while (isspace((unsigned char)*p)) ++p;
            if (!strncmp(p, "plain", 5)) p += 5;
            while (isspace((unsigned char)*p)) ++p;
            int code;
            char name[64];
            if (sscanf(p, "keycode %d = %63s", &code, name) != 2 || code < 0 || code > KEY_MAX) continue;
            char c = keysym_char(name);
            if (!c) continue;
            if (code <= KEY_SLASH && k_lo[code] && k_lo[code] == tolower((unsigned char)c)) continue;   /* plain US key */
            g_sym[code] = c;
            ++n;
        }
        fclose(f);
    }
    if (n) { g_keymap_from_file = 1; return n; }
    for (size_t i = 0; i < sizeof k_zero_sym / sizeof *k_zero_sym; ++i) g_sym[k_zero_sym[i].code] = k_zero_sym[i].ch;
    return 0;
}

static int is_builtin_name(const char *name)
{
    char l[64];
    size_t i = 0;
    for (; name[i] && i < sizeof l - 1; ++i) l[i] = (char)tolower((unsigned char)name[i]);
    l[i] = 0;
    return strstr(l, "tca8418") || strstr(l, "m5stack") || strstr(l, "cardputer");
}

static void add_dev(int fd)
{
    g_names[g_n][0] = 0;
    if (ioctl(fd, EVIOCGNAME(sizeof g_names[0]), g_names[g_n]) < 0) g_names[g_n][0] = 0;
    /* a device with no name (or a test FIFO) on a Cardputer Zero image is its keypad */
    g_builtin[g_n] = g_names[g_n][0] ? is_builtin_name(g_names[g_n]) : g_keymap_from_file;
    g_fd[g_n++] = fd;
}

int input_device_count(void) { return g_n; }
const char *input_device_name(int i) { return i >= 0 && i < g_n ? (g_names[i][0] ? g_names[i] : "?") : ""; }
int input_device_builtin(int i) { return i >= 0 && i < g_n ? g_builtin[i] : 0; }
void input_set_builtin(int i, int on) { if (i >= 0 && i < g_n) g_builtin[i] = on; }
int input_keymap_from_file(void) { return g_keymap_from_file; }

int input_open(const char *dev)
{
    const char *km = getenv("QSOZERO_KEYMAP");
    input_load_keymap(km ? km : ZERO_KEYMAP);
    g_n = 0;
    if (dev && strcmp(dev, "auto")) {
        int fd = open(dev, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) add_dev(fd);
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
        if (is_keyboard(fd)) add_dev(fd);
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

/* US layout (USB keyboards, and the Zero keypad's letters and digits) */
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

static int translate(int code, int builtin, keyev_t *ev)
{
    ev->mods = g_mods;
    ev->ch = 0;
    if (builtin && code >= 0 && code <= KEY_MAX && g_sym[code]) {   /* Zero Sym symbol: already resolved */
        ev->key = K_CHAR;
        ev->ch = g_sym[code];
        return 1;
    }
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
            /* ESC: short press = back, hold = exit (the Zero has a real ESC key;
             * code 41 there is the Sym '%', so ` is no longer an ESC alias) */
            if (e.code == g_esc_code) {
                if (e.value == 0) {
                    int fired = g_esc_fired;
                    g_esc_code = -1;
                    if (!fired) { *ev = (keyev_t){.key = K_ESC, .mods = g_mods}; return 1; }
                }
                continue;                               /* autorepeat while held */
            }
            if (e.value == 1 && e.code == KEY_ESC) {
                g_esc_code = e.code;
                g_esc_t0 = now_s();
                g_esc_fired = 0;
                *ev = (keyev_t){.key = K_ESC_HOLDING, .mods = g_mods};
                return 1;
            }
            if (e.value == 0) continue;                 /* release */
            if (translate(e.code, g_builtin[i], ev)) return 1;   /* press or autorepeat */
        }
    }
    return esc_hold_due(ev);
}
