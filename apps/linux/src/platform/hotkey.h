/* Global hotkeys with two backends:
 *  - portal: XDG GlobalShortcuts (sudo-free; combos only; Activated/Deactivated
 *    give press+release, so TapStateMachine works unchanged);
 *  - raw: evdev (/dev/input) — any key incl. modifier-only; needs the `input`
 *    group. Raw input cannot swallow keys, so review accept/discard and cancel
 *    are combos (Ctrl+Alt+Enter / Ctrl+Alt+Esc) on both backends.
 * Runs entirely on the GLib main loop. */
#pragma once
#include <glib.h>
#include <stdbool.h>
#include "core/config.h"
#include "core/tap.h"

typedef enum { VV_HK_ACTION_START, VV_HK_ACTION_COMMIT, VV_HK_ACTION_DISCARD } VvHotkeyAction;
typedef enum { VV_HK_REVIEW_ACCEPT, VV_HK_REVIEW_DISCARD, VV_HK_CANCEL } VvHotkeyControl;

typedef struct VvHotkeyEngine VvHotkeyEngine;

typedef void (*VvHotkeyActionFn)(VvHotkeyAction action, const char *profile_id, gpointer user);
typedef void (*VvHotkeyControlFn)(VvHotkeyControl control, gpointer user);
typedef void (*VvHotkeyCaptureFn)(const VvHotkey *spec, gpointer user);

VvHotkeyEngine *vv_hotkey_engine_new(VvHotkeyActionFn on_action, VvHotkeyControlFn on_control, gpointer user);
void vv_hotkey_engine_free(VvHotkeyEngine *e);
/* (Re)binds every profile's hotkey with the backend chosen from config. */
void vv_hotkey_engine_configure(VvHotkeyEngine *e, const VvConfig *config);
void vv_hotkey_engine_set_recording_active(VvHotkeyEngine *e, bool active);
void vv_hotkey_engine_set_review_active(VvHotkeyEngine *e, bool active);
/* Which backend is in use right now, for the status/permissions UI. */
const char *vv_hotkey_engine_backend_name(const VvHotkeyEngine *e);
char *vv_hotkey_engine_status(const VvHotkeyEngine *e);   /* human-readable */
/* Raw mode only: next key press is reported to `fn` instead of acted on. */
void vv_hotkey_engine_capture(VvHotkeyEngine *e, VvHotkeyCaptureFn fn, gpointer user);
void vv_hotkey_engine_cancel_capture(VvHotkeyEngine *e);

bool vv_raw_input_available(void);
/* Portal trigger string for a spec, e.g. "<Control><Alt>space" (NULL for modifier-only). */
char *vv_hotkey_portal_trigger(const VvHotkey *h);
char *vv_hotkey_describe(const VvHotkey *h);
const char *vv_keycode_name(int evdev_code);
