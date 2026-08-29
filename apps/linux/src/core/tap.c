#include "core/tap.h"

void vv_tap_init(VvTap *t, VvTapStartMode mode) {
    t->start_mode = mode;
    t->hold_threshold = 0.35;
    t->tap_window = 0.40;
    t->phase = VV_PHASE_IDLE;
    t->pressed_since = 0;
    t->tap = 0;
    t->deadline = 0;
}

bool vv_tap_is_active(const VvTap *t) { return t->phase != VV_PHASE_IDLE; }
double vv_tap_pending_deadline(const VvTap *t) { return t->phase == VV_PHASE_AWAITING_SECOND_TAP ? t->deadline : -1; }
static int taps_required(const VvTap *t) { return t->start_mode == VV_TAP_DOUBLE ? 2 : 1; }

int vv_tap_key_down(VvTap *t, double now, VvTapAct *out) {
    switch (t->phase) {
    case VV_PHASE_IDLE:
        t->phase = VV_PHASE_PRESSED; t->pressed_since = now; t->tap = 1;
        *out = VV_ACT_START_RECORDING; return 1;
    case VV_PHASE_AWAITING_SECOND_TAP:
        t->phase = VV_PHASE_PRESSED; t->pressed_since = now; t->tap = 2;
        return 0;
    case VV_PHASE_LATCHED:
        t->phase = VV_PHASE_DRAINING;
        *out = VV_ACT_COMMIT; return 1;
    default: return 0;
    }
}

int vv_tap_key_up(VvTap *t, double now, VvTapAct *out) {
    switch (t->phase) {
    case VV_PHASE_PRESSED:
        if (now - t->pressed_since >= t->hold_threshold) {
            t->phase = VV_PHASE_IDLE;
            *out = VV_ACT_COMMIT; return 1;   /* hold-to-talk */
        }
        if (t->tap >= taps_required(t)) { t->phase = VV_PHASE_LATCHED; return 0; }
        t->phase = VV_PHASE_AWAITING_SECOND_TAP;
        t->deadline = now + t->tap_window;
        return 0;
    case VV_PHASE_DRAINING:
        t->phase = VV_PHASE_IDLE; return 0;
    default: return 0;
    }
}

int vv_tap_expire(VvTap *t, double now, VvTapAct *out) {
    if (t->phase == VV_PHASE_AWAITING_SECOND_TAP && now >= t->deadline) {
        t->phase = VV_PHASE_IDLE;
        *out = VV_ACT_DISCARD; return 1;
    }
    return 0;
}

int vv_tap_cancel(VvTap *t, VvTapAct *out) {
    bool was_active = vv_tap_is_active(t);
    t->phase = VV_PHASE_IDLE;
    if (was_active) { *out = VV_ACT_DISCARD; return 1; }
    return 0;
}
