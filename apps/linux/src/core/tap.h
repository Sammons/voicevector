/* Pure hotkey-gesture logic — behavior-identical to the macOS and Windows
 * apps (docs/config-schema.md "Hotkey semantics"). Recording starts on the
 * very first key-down; a stray single tap is discarded after the tap window. */
#pragma once
#include <stdbool.h>

typedef enum { VV_TAP_DOUBLE, VV_TAP_SINGLE } VvTapStartMode;
typedef enum { VV_ACT_START_RECORDING, VV_ACT_COMMIT, VV_ACT_DISCARD } VvTapAct;
typedef enum { VV_PHASE_IDLE, VV_PHASE_PRESSED, VV_PHASE_AWAITING_SECOND_TAP, VV_PHASE_LATCHED, VV_PHASE_DRAINING } VvTapPhase;

typedef struct {
    VvTapStartMode start_mode;
    double hold_threshold;   /* 0.35 */
    double tap_window;       /* 0.40 */
    VvTapPhase phase;
    double pressed_since;
    int tap;
    double deadline;
} VvTap;

void vv_tap_init(VvTap *t, VvTapStartMode mode);
bool vv_tap_is_active(const VvTap *t);
/* Deadline for the pending second tap, or -1 when none. */
double vv_tap_pending_deadline(const VvTap *t);

/* Each returns the number of actions written to `out` (0 or 1). */
int vv_tap_key_down(VvTap *t, double now, VvTapAct *out);
int vv_tap_key_up(VvTap *t, double now, VvTapAct *out);
int vv_tap_expire(VvTap *t, double now, VvTapAct *out);
int vv_tap_cancel(VvTap *t, VvTapAct *out);
