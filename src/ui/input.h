/* QSO-Zero - keyboard input via Linux evdev (Cardputer keyboard + USB). */
#ifndef QSOZERO_INPUT_H
#define QSOZERO_INPUT_H

enum {
    K_NONE = 0, K_CHAR, K_ENTER, K_BKSP, K_TAB, K_ESC, K_UP, K_DOWN, K_LEFT, K_RIGHT,
    K_PGUP, K_PGDN, K_DEL, K_HOME, K_END, K_F1, K_F2, K_F3, K_F4, K_F5,
    K_ESC_HOLDING,   /* ESC went down: show "Hold ESC to exit" */
    K_ESC_HOLD       /* ESC held for ESC_HOLD_S: exit to the launcher */
};

/* CardputerZero launcher convention: short ESC = back, hold ESC = exit.
 * A short press is delivered as K_ESC on release. */
#define ESC_HOLD_S 1.0
#define MOD_SHIFT 1
#define MOD_CTRL  2
#define MOD_ALT   4
#define MOD_FN    8

typedef struct { int key; int ch; int mods; } keyev_t;

int  input_open(const char *dev);      /* "auto" = every keyboard-like device */
void input_close(void);
/* waits up to timeout_ms; returns 1 and fills ev when a key arrives */
int  input_poll(keyev_t *ev, int timeout_ms);

#endif
