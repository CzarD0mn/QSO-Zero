/* Sound card level control: which mixer control is used, and graceful
 * errors when there is no such card. */
#include <stdio.h>
#include <string.h>
#include "audio/mixer.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok    " __VA_ARGS__); else { printf("  FAIL  " __VA_ARGS__); ++fails; } printf("\n"); } while (0)

int main(void)
{
    printf("[1] picking the control\n");
    /* C-Media USB codec (X6100/X6200 internal, Digirig): "Speaker" playback,
     * "Mic" has both a capture volume and a playback (monitor) volume */
    const char *cm[] = {"Speaker", "Mic", "Auto Gain Control"};
    int cm_c[] = {0, 1, 0}, cm_p[] = {1, 1, 0};
    CHECK(mixer_pick(cm, cm_c, 3, 1) == 1, "C-Media: in level = Mic");
    CHECK(mixer_pick(cm, cm_p, 3, 0) == 0, "C-Media: out level = Speaker (not the Mic monitor)");
    /* QMX-style codec with PCM / Capture */
    const char *q[] = {"PCM", "Capture"};
    int q_c[] = {0, 1}, q_p[] = {1, 0};
    CHECK(mixer_pick(q, q_c, 2, 1) == 1 && mixer_pick(q, q_p, 2, 0) == 0, "PCM / Capture codec");
    /* only a Mic monitor for playback: don't use it as the TX level */
    const char *m[] = {"Mic"};
    int m_p[] = {1};
    CHECK(mixer_pick(m, m_p, 1, 0) == -1, "a Mic monitor is never used as the out level");
    /* unknown names: first one with a volume */
    const char *u[] = {"Foo", "Bar"};
    int u_c[] = {0, 1};
    CHECK(mixer_pick(u, u_c, 2, 1) == 1, "unknown names: first with a capture volume");
    CHECK(mixer_pick(u, (int[]){0, 0}, 2, 1) == -1, "no volume controls -> none");

    printf("[2] no such card\n");
    mixer_level_t lv;
    int rc = mixer_get("plughw:CARD=NoSuchCard,DEV=0", 1, &lv);
    CHECK(rc != 0 && !lv.ok && lv.error[0], "error, not a crash: \"%s\"", lv.error);
    rc = mixer_set("", 0, 50, &lv);
    CHECK(rc != 0 && !lv.ok, "no device: \"%s\"", lv.error);

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}
