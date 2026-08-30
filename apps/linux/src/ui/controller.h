/* One dictation at a time: hotkey → record → transcribe → clean → (review) →
 * paste → save → webhook. Mirrors the macOS/Windows pipelines. Runs the
 * network work on a worker thread; all state changes land on the main loop. */
#pragma once
#include <glib.h>
#include <stdbool.h>
#include "core/config.h"
#include "core/library.h"
#include "platform/audio.h"
#include "platform/hotkey.h"

typedef enum { VV_STATE_IDLE, VV_STATE_RECORDING, VV_STATE_PROCESSING, VV_STATE_REVIEWING, VV_STATE_FAILED } VvState;

typedef struct VvController VvController;
typedef void (*VvStateFn)(VvController *c, gpointer user);

struct VvController {
    VvConfig *config;
    char *config_path;
    VvLibrary *library;
    VvRecorder *recorder;
    VvHotkeyEngine *hotkey;
    VvState state;
    char *detail;                 /* "Transcribing…" etc. */
    char *review_draft;           /* non-NULL while a review session is active */
    char *review_route;           /* router verdict for the staging card, or NULL */
    guint library_generation;
    VvStateFn on_change; gpointer user;
    /* private */
    gpointer priv;
};

VvController *vv_controller_new(VvConfig *config, const char *config_path);
void vv_controller_free(VvController *c);
void vv_controller_set_observer(VvController *c, VvStateFn fn, gpointer user);
void vv_controller_save_config(VvController *c);       /* persists + reconfigures hotkeys/warm policy */
void vv_controller_reload_library(VvController *c);

void vv_controller_start(VvController *c, const char *profile_id);
void vv_controller_finish(VvController *c);
void vv_controller_discard(VvController *c);
void vv_controller_accept_review(VvController *c);
void vv_controller_discard_review(VvController *c);
void vv_controller_retry(VvController *c, VvEntry *entry);
/* Inbound routed text from a paired machine: paste + save. done() gets the
 * outcome. Main thread. */
void vv_controller_receive_routed(VvController *c, const char *text,
                                  void (*done)(bool ok, const char *error, gpointer token), gpointer token);
bool vv_controller_is_busy(const VvController *c);
