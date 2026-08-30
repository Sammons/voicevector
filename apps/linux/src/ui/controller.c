#include "ui/controller.h"
#include "core/cleanup.h"
#include "core/providers.h"
#include "core/webhook.h"
#include "core/wav.h"
#include "core/log.h"
#include "platform/services.h"
#include "platform/peerservice.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

typedef struct {
    char *id, *audio_path, *folder;
    double duration;
    uint32_t tail_start_byte;
    char *profile_id;
    GPtrArray *segments;        /* char* transcripts of streamed segments (ordered) */
    GMutex segment_lock;
} Job;

typedef struct {
    VvEntry *entry;
    char *audio_path, *folder;
    char *profile_id;
    VvScreenshotSet *screenshots;   /* or NULL */
    int revisions;
    /* router verdict (docs/multi-machine.md); label NULL = normal paste */
    char *route_label;
    char *route_peer_fp;            /* remote target's fingerprint */
    guint32 route_window;
} Review;

typedef struct {
    char *active_profile_id;
    char *slot_id, *slot_audio, *slot_folder;
    VvScreenshotSet *pending_screenshots;
    Review *review;
    char *command_path;
    GPtrArray *segments; GMutex segment_lock;     /* live streamed segments */
    GBytes *chime_start, *chime_stop, *chime_error;
    guint clear_failure_source;
} Priv;

#define P(c) ((Priv *)(c)->priv)

static void changed(VvController *c) { if (c->on_change) c->on_change(c, c->user); }

static void set_state(VvController *c, VvState s, const char *detail) {
    c->state = s;
    g_free(c->detail); c->detail = g_strdup(detail ? detail : "");
    vv_hotkey_engine_set_recording_active(c->hotkey, s == VV_STATE_RECORDING);
    vv_hotkey_engine_set_review_active(c->hotkey, s == VV_STATE_REVIEWING);
    changed(c);
}

static gboolean clear_failure(gpointer data) {
    VvController *c = data;
    P(c)->clear_failure_source = 0;
    if (c->state == VV_STATE_FAILED) set_state(c, VV_STATE_IDLE, NULL);
    return G_SOURCE_REMOVE;
}

static void fail(VvController *c, const char *message) {
    vv_log_error("%s", message);
    if (c->config->play_sounds) vv_audio_play_wav(P(c)->chime_error);
    set_state(c, VV_STATE_FAILED, message);
    if (P(c)->clear_failure_source) g_source_remove(P(c)->clear_failure_source);
    P(c)->clear_failure_source = g_timeout_add_seconds(5, clear_failure, c);
}

static void bump_library(VvController *c) { c->library_generation++; changed(c); }

/* ---------------------------------------------------- hotkey glue */

static void on_hotkey_action(VvHotkeyAction a, const char *profile_id, gpointer user) {
    VvController *c = user;
    switch (a) {
    case VV_HK_ACTION_START: vv_controller_start(c, profile_id); break;
    case VV_HK_ACTION_COMMIT: vv_controller_finish(c); break;
    case VV_HK_ACTION_DISCARD: vv_controller_discard(c); break;
    }
}

static void on_hotkey_control(VvHotkeyControl ctl, gpointer user) {
    VvController *c = user;
    if (ctl == VV_HK_REVIEW_ACCEPT) vv_controller_accept_review(c);
    else if (ctl == VV_HK_REVIEW_DISCARD) vv_controller_discard_review(c);
    else vv_controller_discard(c);
}

static void apply_warm_policy(VvController *c) {
    vv_recorder_set_warm_policy(c->recorder, c->config->keep_mic_warm_after_recording, c->config->keep_mic_always_warm);
    vv_recorder_apply_warm_policy(c->recorder);
}

VvController *vv_controller_new(VvConfig *config, const char *config_path) {
    VvController *c = g_new0(VvController, 1);
    Priv *p = g_new0(Priv, 1);
    c->priv = p;
    g_mutex_init(&p->segment_lock);
    p->segments = g_ptr_array_new_with_free_func(g_free);
    p->chime_start = vv_wav_chime(true);
    p->chime_stop = vv_wav_chime(false);
    p->chime_error = vv_wav_chime(false);
    c->config = config;
    c->config_path = g_strdup(config_path);
    char *root = vv_config_expanded_library_path(config);
    c->library = vv_library_new(root);
    g_free(root);
    c->recorder = vv_recorder_new();
    c->hotkey = vv_hotkey_engine_new(on_hotkey_action, on_hotkey_control, c);
    vv_hotkey_engine_configure(c->hotkey, config);
    c->detail = g_strdup("");
    apply_warm_policy(c);
    return c;
}

void vv_controller_free(VvController *c) {
    if (!c) return;
    Priv *p = P(c);
    vv_hotkey_engine_free(c->hotkey);
    vv_recorder_free(c->recorder);
    vv_library_free(c->library);
    g_ptr_array_unref(p->segments);
    g_bytes_unref(p->chime_start); g_bytes_unref(p->chime_stop); g_bytes_unref(p->chime_error);
    g_free(p->active_profile_id); g_free(p->slot_id); g_free(p->slot_audio); g_free(p->slot_folder); g_free(p->command_path);
    vv_screenshot_set_unref(p->pending_screenshots);
    g_free(p);
    g_free(c->config_path); g_free(c->detail); g_free(c->review_draft); g_free(c->review_route);
    g_free(c);
}

void vv_controller_set_observer(VvController *c, VvStateFn fn, gpointer user) { c->on_change = fn; c->user = user; }

void vv_controller_save_config(VvController *c) {
    vv_config_save(c->config, c->config_path);
    vv_hotkey_engine_configure(c->hotkey, c->config);
    apply_warm_policy(c);
    changed(c);
}

void vv_controller_reload_library(VvController *c) {
    vv_library_free(c->library);
    char *root = vv_config_expanded_library_path(c->config);
    c->library = vv_library_new(root);
    g_free(root);
    bump_library(c);
}

bool vv_controller_is_busy(const VvController *c) { return c->state == VV_STATE_RECORDING || c->state == VV_STATE_PROCESSING; }

/* -------------------------------------------------- recording */

typedef struct { VvController *c; GBytes *wav; int index; VvProvider *stt; char *api_key; GPtrArray *vocabulary; } SegmentTask;

static gpointer segment_thread(gpointer data) {
    SegmentTask *t = data;
    char *text = NULL, *error = NULL;
    char *name = g_strdup_printf("segment%d.wav", t->index);
    if (!vv_provider_transcribe(t->stt, t->api_key, t->wav, name, t->vocabulary, &text, &error)) {
        vv_log_error("Segment %d transcription failed: %s", t->index, error);
        g_free(error);
    }
    g_free(name);
    Priv *p = P(t->c);
    g_mutex_lock(&p->segment_lock);
    while ((int)p->segments->len <= t->index) g_ptr_array_add(p->segments, NULL);
    g_free(g_ptr_array_index(p->segments, t->index));
    g_ptr_array_index(p->segments, t->index) = text ? text : g_strdup("\x01");   /* \x01 = failed marker */
    g_mutex_unlock(&p->segment_lock);
    g_bytes_unref(t->wav); g_free(t->api_key); g_ptr_array_unref(t->vocabulary); g_free(t);
    return NULL;
}

typedef struct { VvController *c; VvProvider *stt; char *api_key; GPtrArray *vocabulary; } SegmentContext;

static void on_segment(GBytes *wav, int index, gpointer user) {
    SegmentContext *ctx = user;
    SegmentTask *t = g_new0(SegmentTask, 1);
    t->c = ctx->c; t->wav = g_bytes_ref(wav); t->index = index; t->stt = ctx->stt;
    t->api_key = g_strdup(ctx->api_key); t->vocabulary = g_ptr_array_ref(ctx->vocabulary);
    GThread *th = g_thread_new("vv-segment", segment_thread, t);
    g_thread_unref(th);
}

static SegmentContext *segment_ctx;   /* one recording at a time */

static void start_command_recording(VvController *c);

void vv_controller_start(VvController *c, const char *profile_id) {
    if (vv_controller_is_busy(c)) return;
    if (c->state == VV_STATE_REVIEWING) { start_command_recording(c); return; }
    Priv *p = P(c);
    g_free(p->active_profile_id);
    VvProfile *first = g_ptr_array_index(c->config->profiles, 0);
    p->active_profile_id = g_strdup(profile_id ? profile_id : first->id);
    VvProfile *profile = vv_config_find_profile(c->config, p->active_profile_id);

    vv_screenshot_set_unref(p->pending_screenshots); p->pending_screenshots = NULL;
    if (profile && profile->screenshot_context) p->pending_screenshots = vv_screenshots();

    g_free(p->slot_id); g_free(p->slot_audio); g_free(p->slot_folder);
    vv_library_new_slot(c->library, c->config->active_folder, &p->slot_id, &p->slot_audio);
    if (p->pending_screenshots) vv_library_save_screenshots(c->library, p->slot_id, c->config->active_folder, p->pending_screenshots);
    p->slot_folder = g_strdup(c->config->active_folder);

    g_mutex_lock(&p->segment_lock); g_ptr_array_set_size(p->segments, 0); g_mutex_unlock(&p->segment_lock);
    VvEffective eff = vv_effective(profile, c->config);
    bool chunk = c->config->chunked_transcription && eff.stt && !(eff.enabled && vv_single_pass_eligible(eff.stt, eff.provider, eff.config.mode));
    if (segment_ctx) { g_free(segment_ctx->api_key); g_ptr_array_unref(segment_ctx->vocabulary); g_free(segment_ctx); segment_ctx = NULL; }
    if (chunk) {
        segment_ctx = g_new0(SegmentContext, 1);
        segment_ctx->c = c; segment_ctx->stt = eff.stt;
        segment_ctx->api_key = vv_secret_get(eff.stt->id);
        segment_ctx->vocabulary = vv_parse_vocabulary(eff.config.vocabulary);
    }
    vv_effective_clear(&eff);

    set_state(c, VV_STATE_RECORDING, "Recording…");
    char *error = NULL;
    if (!vv_recorder_start(c->recorder, p->slot_audio, chunk, chunk ? on_segment : NULL, segment_ctx, &error)) {
        char *msg = g_strdup_printf("Could not start recording: %s", error);
        fail(c, msg); g_free(msg); g_free(error);
        return;
    }
    if (c->config->play_sounds) vv_audio_play_wav(p->chime_start);
}

/* ------------------------------------------------- pipeline job */

typedef struct {
    VvController *c;
    Job job;
    VvConfig *config;          /* borrowed; config isn't mutated while busy */
    VvEntry *entry;
    char *fail_message;
    bool review;
    VvProfile *profile;
    VvScreenshotSet *screenshots;
    GPtrArray *attachments;   /* captioned parts for chat calls, or NULL */
    /* results */
    bool stt_ok;
} Pipeline;

static void notify(VvController *c, const char *message) { vv_log_error("%s", message); }

static gpointer pipeline_thread(gpointer data) {
    Pipeline *pl = data;
    VvController *c = pl->c;
    VvConfig *config = pl->config;
    VvProfile *profile = pl->profile;
    VvEntry *entry = pl->entry;
    VvEffective eff = vv_effective(profile, config);
    VvProvider *stt = eff.stt;
    if (!stt) {
        g_free(entry->status); entry->status = g_strdup("error: no transcription provider configured");
        vv_library_save(c->library, entry);
        pl->fail_message = g_strdup("No transcription provider configured — open Settings.");
        vv_effective_clear(&eff);
        return NULL;
    }
    g_free(entry->stt_label); entry->stt_label = g_strdup_printf("%s/%s", stt->name, stt->stt_model);
    char *stt_key = vv_secret_get(stt->id);
    bool single_passed = false;

    /* 0. Single-pass. */
    if (eff.enabled && vv_single_pass_eligible(stt, eff.provider, eff.config.mode)) {
        char *data = NULL; gsize n = 0;
        if (g_file_get_contents(pl->job.audio_path, &data, &n, NULL)) {
            GBytes *wav = g_bytes_new_take(data, n);
            char *system = vv_cleanup_system_prompt(&eff.config);
            char *reply = NULL, *error = NULL;
            if (vv_provider_chat_with_audio(stt, stt_key, system, wav, pl->attachments, &reply, &error)) {
                char *cleaned = vv_post_process(reply, "");
                if (*cleaned) {
                    g_free(entry->raw); entry->raw = g_strdup(cleaned);
                    g_free(entry->cleaned); entry->cleaned = cleaned;
                    g_free(entry->stt_label); entry->stt_label = g_strdup_printf("%s/%s (single-pass)", stt->name, stt->chat_model);
                    g_free(entry->cleanup_label); entry->cleanup_label = g_strdup("single-pass");
                    single_passed = true;
                } else g_free(cleaned);
            } else { vv_log_error("Single-pass failed, falling back: %s", error); g_free(error); }
            g_free(reply); g_free(system); g_bytes_unref(wav);
        }
    }

    if (!single_passed) {
        /* 1. Transcribe — streamed segments first, tail/full file as needed. */
        GPtrArray *vocabulary = vv_parse_vocabulary(eff.config.vocabulary);
        GString *raw = g_string_new(NULL);
        bool ok = false;
        Priv *p = P(c);
        g_mutex_lock(&p->segment_lock);
        guint nseg = p->segments->len;
        bool all_ok = nseg > 0;
        for (guint i = 0; i < nseg; i++) { const char *s = g_ptr_array_index(p->segments, i); if (!s || strcmp(s, "\x01") == 0) all_ok = false; }
        if (all_ok) for (guint i = 0; i < nseg; i++) { if (i) g_string_append_c(raw, ' '); g_string_append(raw, g_strstrip((char *)g_ptr_array_index(p->segments, i))); }
        g_mutex_unlock(&p->segment_lock);
        if (all_ok) {
            /* Tail after the last flushed segment. */
            gsize size = 0; char *probe = NULL;
            if (g_file_get_contents(pl->job.audio_path, &probe, &size, NULL)) g_free(probe);
            uint32_t data_bytes = size > 44 ? (uint32_t)(size - 44) : 0;
            if (data_bytes > pl->job.tail_start_byte + 2 * VV_WAV_SAMPLE_RATE / 2) {
                GBytes *tail = vv_wav_slice(pl->job.audio_path, pl->job.tail_start_byte, data_bytes, VV_WAV_SAMPLE_RATE);
                char *text = NULL, *error = NULL;
                if (tail && vv_provider_transcribe(stt, stt_key, tail, "tail.wav", vocabulary, &text, &error)) {
                    if (raw->len && *g_strstrip(text)) g_string_append_c(raw, ' ');
                    g_string_append(raw, g_strstrip(text));
                    ok = true;
                } else { vv_log_error("Tail transcription failed: %s", error ? error : "?"); g_free(error); }
                g_free(text); if (tail) g_bytes_unref(tail);
            } else ok = true;
            if (ok) { g_free(entry->stt_label); entry->stt_label = g_strdup_printf("%s/%s (streamed %u parts)", stt->name, stt->stt_model, nseg + 1); }
        }
        if (!ok) {
            g_string_set_size(raw, 0);
            char *data = NULL; gsize n = 0;
            if (g_file_get_contents(pl->job.audio_path, &data, &n, NULL)) {
                GBytes *wav = g_bytes_new_take(data, n);
                char *text = NULL, *error = NULL;
                char *name = g_strconcat(pl->job.id, ".wav", NULL);
                if (vv_provider_transcribe(stt, stt_key, wav, name, vocabulary, &text, &error)) { g_string_append(raw, text); ok = true; }
                else {
                    g_free(entry->status); entry->status = g_strdup_printf("error: %s", error);
                    vv_library_save(c->library, entry);
                    pl->fail_message = g_strdup_printf("Transcription failed: %s", error);
                    g_free(error);
                }
                g_free(text); g_free(name); g_bytes_unref(wav);
            }
        }
        g_ptr_array_unref(vocabulary);
        if (!ok) { g_string_free(raw, TRUE); g_free(stt_key); vv_effective_clear(&eff); return NULL; }
        g_free(entry->raw); entry->raw = g_strstrip(g_string_free(raw, FALSE));
        if (!*entry->raw) {
            g_free(entry->status); entry->status = g_strdup("error: empty transcript");
            vv_library_save(c->library, entry);
            pl->fail_message = g_strdup("The recording produced an empty transcript.");
            g_free(stt_key); vv_effective_clear(&eff); return NULL;
        }

        /* 2. Clean up. */
        g_free(entry->cleaned); entry->cleaned = g_strdup(entry->raw);
        if (!eff.enabled) {
            if (profile) { g_free(entry->cleanup_label); entry->cleanup_label = g_strdup_printf("skipped — %s hotkey", profile->name); }
        } else if (eff.provider && vv_kind_supports_chat(eff.provider->kind) && *eff.provider->chat_model) {
            g_free(entry->cleanup_label); entry->cleanup_label = g_strdup_printf("%s/%s", eff.provider->name, eff.provider->chat_model);
            char *key = vv_secret_get(eff.provider->id);
            char *system = vv_cleanup_system_prompt(&eff.config);
            char *user = vv_wrap_transcript(entry->raw);
            char *reply = NULL, *error = NULL;
            bool cok;
            if (pl->screenshots) {
                char *with = g_strconcat(system, "\n", vv_screenshot_note(), NULL);
                cok = vv_provider_chat(eff.provider, key, with, user, pl->attachments, &reply, &error);
                g_free(with);
                if (!cok) { g_free(error); error = NULL; cok = vv_provider_chat(eff.provider, key, system, user, NULL, &reply, &error); }
            } else cok = vv_provider_chat(eff.provider, key, system, user, NULL, &reply, &error);
            if (cok) { g_free(entry->cleaned); entry->cleaned = vv_post_process(reply, entry->raw); }
            else {
                char *label = g_strconcat(entry->cleanup_label, " (failed — raw used)", NULL);
                g_free(entry->cleanup_label); entry->cleanup_label = label;
                vv_log_error("Cleanup failed, using raw transcript: %s", error);
            }
            g_free(reply); g_free(error); g_free(user); g_free(system); g_free(key);
        } else {
            g_free(entry->cleanup_label); entry->cleanup_label = g_strdup("not run — no cleanup provider selected");
        }
    }
    g_free(stt_key);
    vv_effective_clear(&eff);
    vv_library_save(c->library, entry);
    pl->stt_ok = true;
    return NULL;
}

static void deliver(VvController *c, VvEntry *entry, const char *audio_path, const char *folder);
static void start_router(VvController *c, Review *r, VvProfile *profile);

static gboolean pipeline_done(gpointer data) {
    Pipeline *pl = data;
    VvController *c = pl->c;
    bump_library(c);
    if (!pl->stt_ok) {
        fail(c, pl->fail_message ? pl->fail_message : "Dictation failed");
    } else if (pl->review && pl->profile) {
        Priv *p = P(c);
        Review *r = g_new0(Review, 1);
        r->entry = pl->entry; pl->entry = NULL;
        r->audio_path = g_strdup(pl->job.audio_path); r->folder = g_strdup(pl->job.folder);
        r->profile_id = g_strdup(pl->profile->id);
        r->screenshots = vv_screenshot_set_ref(pl->screenshots);
        p->review = r;
        g_free(c->review_draft); c->review_draft = g_strdup(r->entry->cleaned);
        g_free(c->review_route); c->review_route = NULL;
        if (pl->profile->router_enabled) {
            c->review_route = g_strdup("Routing…");
            start_router(c, r, pl->profile);
        }
        set_state(c, VV_STATE_REVIEWING, NULL);
    } else {
        deliver(c, pl->entry, pl->job.audio_path, pl->job.folder);
    }
    vv_entry_free(pl->entry);
    vv_screenshot_set_unref(pl->screenshots);
    if (pl->attachments) g_ptr_array_unref(pl->attachments);
    g_free(pl->fail_message); g_free(pl->job.id); g_free(pl->job.audio_path); g_free(pl->job.folder); g_free(pl->job.profile_id);
    g_free(pl);
    return G_SOURCE_REMOVE;
}

static gpointer pipeline_runner(gpointer data) {
    pipeline_thread(data);
    g_idle_add(pipeline_done, data);
    return NULL;
}

static void run_pipeline(VvController *c, const char *id, const char *audio_path, const char *folder, double duration,
                         uint32_t tail_start_byte, const char *profile_id, VvScreenshotSet *screenshots) {
    Pipeline *pl = g_new0(Pipeline, 1);
    pl->c = c; pl->config = c->config;
    pl->job.id = g_strdup(id); pl->job.audio_path = g_strdup(audio_path); pl->job.folder = g_strdup(folder);
    pl->job.duration = duration; pl->job.tail_start_byte = tail_start_byte; pl->job.profile_id = g_strdup(profile_id);
    pl->profile = vv_config_find_profile(c->config, profile_id);
    pl->review = pl->profile && pl->profile->review_before_paste && profile_id != NULL;
    pl->screenshots = vv_screenshot_set_ref(screenshots);
    pl->attachments = screenshots ? vv_screenshot_set_attachments(screenshots) : NULL;
    pl->entry = vv_entry_new();
    vv_entry_attach(pl->entry, screenshots);
    g_free(pl->entry->id); pl->entry->id = g_strdup(id);
    g_free(pl->entry->folder); pl->entry->folder = g_strdup(folder);
    pl->entry->date_unix = g_get_real_time() / G_USEC_PER_SEC;
    pl->entry->duration = duration;
    set_state(c, VV_STATE_PROCESSING, "Transcribing…");
    GThread *t = g_thread_new("vv-pipeline", pipeline_runner, pl);
    g_thread_unref(t);
}

void vv_controller_finish(VvController *c) {
    Priv *p = P(c);
    if (p->review && p->command_path) {
        /* command recording */
        double duration = vv_recorder_stop(c->recorder);
        char *path = p->command_path; p->command_path = NULL;
        if (c->config->play_sounds) vv_audio_play_wav(p->chime_stop);
        if (duration < 0.5) { g_unlink(path); g_free(path); set_state(c, VV_STATE_REVIEWING, NULL); return; }
        extern void vv_controller_apply_command(VvController *c, char *path);
        vv_controller_apply_command(c, path);
        return;
    }
    if (c->state != VV_STATE_RECORDING || !p->slot_id) return;
    double duration = vv_recorder_stop(c->recorder);
    uint32_t tail = vv_recorder_tail_start_byte(c->recorder);
    if (c->config->play_sounds) vv_audio_play_wav(p->chime_stop);
    if (duration < 0.5) {
        g_unlink(p->slot_audio);
        g_free(p->slot_id); p->slot_id = NULL;
        set_state(c, VV_STATE_IDLE, NULL);
        return;
    }
    char *id = p->slot_id, *audio = p->slot_audio, *folder = p->slot_folder;
    p->slot_id = p->slot_audio = p->slot_folder = NULL;
    run_pipeline(c, id, audio, folder, duration, tail, p->active_profile_id, p->pending_screenshots);
    g_free(id); g_free(audio); g_free(folder);
}

void vv_controller_discard(VvController *c) {
    if (c->state != VV_STATE_RECORDING) return;
    Priv *p = P(c);
    vv_recorder_discard(c->recorder);
    if (p->review && p->command_path) { g_free(p->command_path); p->command_path = NULL; set_state(c, VV_STATE_REVIEWING, NULL); return; }
    if (p->slot_id) vv_library_delete_screenshots(c->library, p->slot_id, p->slot_folder ? p->slot_folder : c->config->active_folder);
    vv_screenshot_set_unref(p->pending_screenshots); p->pending_screenshots = NULL;
    g_free(p->slot_id); p->slot_id = NULL;
    set_state(c, VV_STATE_IDLE, NULL);
}

/* ------------------------------------------------------ deliver */

typedef struct { VvEntry *entry; char *audio_path; VvWebhook hook; } HookTask;
static gpointer hook_thread(gpointer data) {
    HookTask *t = data;
    vv_webhook_send(t->entry, t->audio_path, &t->hook);
    vv_entry_free(t->entry); g_free(t->audio_path); g_free(t->hook.url); g_free(t);
    return NULL;
}

static void deliver(VvController *c, VvEntry *entry, const char *audio_path, const char *folder) {
    set_state(c, VV_STATE_PROCESSING, "Pasting…");
    char *reason = NULL;
    VvPasteOutcome outcome = vv_paste_insert(entry->cleaned, c->config->auto_paste, &reason);
    if (outcome == VV_PASTE_COPIED_ONLY) { char *m = g_strdup_printf("Transcript copied — %s. Press Ctrl+V to insert it.", reason ? reason : ""); notify(c, m); g_free(m); }
    g_free(reason);
    set_state(c, VV_STATE_IDLE, NULL);
    VvWebhook *hook = g_hash_table_lookup(c->config->folder_webhooks, folder);
    if (hook && hook->enabled) {
        HookTask *t = g_new0(HookTask, 1);
        t->entry = vv_library_parse(vv_library_render(entry), entry->id, entry->folder);
        t->audio_path = g_strdup(audio_path);
        t->hook = *hook; t->hook.url = g_strdup(hook->url);
        GThread *th = g_thread_new("vv-webhook", hook_thread, t);
        g_thread_unref(th);
    }
}

/* ------------------------------------------------------- review */

static void start_command_recording(VvController *c) {
    Priv *p = P(c);
    if (!p->review || c->state != VV_STATE_REVIEWING) return;
    char *path = g_build_filename(g_get_tmp_dir(), "vv-command-XXXXXX.wav", NULL);
    int fd = g_mkstemp(path);
    if (fd >= 0) close(fd);
    p->command_path = path;
    set_state(c, VV_STATE_RECORDING, "Listening for a change…");
    char *error = NULL;
    if (!vv_recorder_start(c->recorder, path, false, NULL, NULL, &error)) {
        g_free(p->command_path); p->command_path = NULL;
        char *m = g_strdup_printf("Could not record the change: %s", error); notify(c, m); g_free(m); g_free(error);
        set_state(c, VV_STATE_REVIEWING, NULL);
        return;
    }
    if (c->config->play_sounds) vv_audio_play_wav(p->chime_start);
}

typedef struct { VvController *c; char *path; char *draft; char *revised; char *error; } CommandTask;

static gpointer command_thread(gpointer data) {
    CommandTask *t = data;
    VvController *c = t->c;
    Priv *p = P(c);
    Review *r = p->review;
    VvProfile *profile = vv_config_find_profile(c->config, r->profile_id);
    VvEffective eff = vv_effective(profile, c->config);
    VvProvider *reviewer = NULL;
    if (profile && profile->review_provider_id) reviewer = vv_config_find_provider(c->config, profile->review_provider_id);
    if (!reviewer && eff.provider && vv_kind_supports_chat(eff.provider->kind) && *eff.provider->chat_model) reviewer = eff.provider;
    if (!reviewer) for (guint i = 0; i < c->config->providers->len; i++) { VvProvider *pp = g_ptr_array_index(c->config->providers, i); if (vv_kind_supports_chat(pp->kind) && *pp->chat_model) { reviewer = pp; break; } }
    if (!eff.stt) t->error = g_strdup("No transcription provider — pick one in Settings.");
    else if (!reviewer) t->error = g_strdup("No review model — pick a cleanup or review model for this hotkey.");
    else {
        char *data = NULL; gsize n = 0;
        g_file_get_contents(t->path, &data, &n, NULL);
        GBytes *wav = g_bytes_new_take(data, n);
        GPtrArray *vocabulary = vv_parse_vocabulary(eff.config.vocabulary);
        char *key = vv_secret_get(eff.stt->id);
        char *instruction = NULL;
        if (vv_provider_transcribe(eff.stt, key, wav, "command.wav", vocabulary, &instruction, &t->error) && *g_strstrip(instruction)) {
            char *rkey = vv_secret_get(reviewer->id);
            char *system = vv_review_system_prompt(eff.config.vocabulary);
            char *user = vv_review_message(t->draft, instruction);
            char *reply = NULL;
            GPtrArray *att = r->screenshots ? vv_screenshot_set_attachments(r->screenshots) : NULL;
            bool ok = att ? vv_provider_chat(reviewer, rkey, system, user, att, &reply, &t->error) : false;
            if (att) g_ptr_array_unref(att);
            if (!ok) { g_free(t->error); t->error = NULL; ok = vv_provider_chat(reviewer, rkey, system, user, NULL, &reply, &t->error); }
            if (ok) {
                t->revised = vv_post_process(reply, t->draft);
                r->revisions++;
                char *base = g_strdup(r->entry->cleanup_label);
                char *cut = strstr(base, " · review "); if (cut) *cut = '\0';
                g_free(r->entry->cleanup_label);
                r->entry->cleanup_label = g_strdup_printf("%s · review %s/%s ×%d", base, reviewer->name, reviewer->chat_model, r->revisions);
                g_free(base);
            }
            g_free(reply); g_free(user); g_free(system); g_free(rkey);
        }
        g_free(instruction); g_free(key); g_ptr_array_unref(vocabulary); g_bytes_unref(wav);
    }
    vv_effective_clear(&eff);
    g_unlink(t->path);
    return NULL;
}

static gboolean command_done(gpointer data) {
    CommandTask *t = data;
    VvController *c = t->c;
    if (t->revised) { g_free(c->review_draft); c->review_draft = t->revised; t->revised = NULL; }
    else if (t->error) { char *m = g_strdup_printf("Revision failed: %s", t->error); notify(c, m); g_free(m); if (c->config->play_sounds) vv_audio_play_wav(P(c)->chime_error); }
    set_state(c, VV_STATE_REVIEWING, NULL);
    g_free(t->path); g_free(t->draft); g_free(t->error); g_free(t);
    return G_SOURCE_REMOVE;
}

static gpointer command_runner(gpointer data) { command_thread(data); g_idle_add(command_done, data); return NULL; }

void vv_controller_apply_command(VvController *c, char *path) {
    CommandTask *t = g_new0(CommandTask, 1);
    t->c = c; t->path = path; t->draft = g_strdup(c->review_draft ? c->review_draft : "");
    set_state(c, VV_STATE_PROCESSING, "Hearing the change…");
    GThread *th = g_thread_new("vv-command", command_runner, t);
    g_thread_unref(th);
}

static void review_free(Review *r) {
    vv_entry_free(r->entry); g_free(r->audio_path); g_free(r->folder); g_free(r->profile_id);
    vv_screenshot_set_unref(r->screenshots);
    g_free(r->route_label); g_free(r->route_peer_fp);
    g_free(r);
}

/* Remote delivery of an accepted, routed draft (worker thread + idle). */
typedef struct { VvController *c; Review *r; char *error; } RouteDeliverTask;

static gboolean route_deliver_done(gpointer data) {
    RouteDeliverTask *t = data;
    VvController *c = t->c;
    Review *r = t->r;
    if (t->error) {
        char *m = g_strdup_printf("Could not deliver to %s: %s — pasting here instead.",
                                  r->route_label, t->error);
        notify(c, m); g_free(m);
        deliver(c, r->entry, r->audio_path, r->folder);
    } else {
        set_state(c, VV_STATE_IDLE, NULL);
        VvWebhook *hook = g_hash_table_lookup(c->config->folder_webhooks, r->folder);
        if (hook && hook->enabled) {
            HookTask *ht = g_new0(HookTask, 1);
            ht->entry = vv_library_parse(vv_library_render(r->entry), r->entry->id, r->entry->folder);
            ht->audio_path = g_strdup(r->audio_path);
            ht->hook = *hook; ht->hook.url = g_strdup(hook->url);
            GThread *th = g_thread_new("vv-webhook", hook_thread, ht);
            g_thread_unref(th);
        }
    }
    review_free(r);
    g_free(t->error); g_free(t);
    return G_SOURCE_REMOVE;
}

static gpointer route_deliver_thread(gpointer data) {
    RouteDeliverTask *t = data;
    VvPeer *peer = NULL;
    for (guint i = 0; i < t->c->config->multi_machine.peers->len; i++) {
        VvPeer *p2 = g_ptr_array_index(t->c->config->multi_machine.peers, i);
        if (g_strcmp0(p2->fingerprint, t->r->route_peer_fp) == 0) { peer = p2; break; }
    }
    t->error = peer ? vv_peer_service_deliver(peer, t->r->entry->cleaned, t->r->route_window)
                    : g_strdup("peer is gone");
    g_idle_add(route_deliver_done, t);
    return NULL;
}

static void end_review(VvController *c, bool paste) {
    Priv *p = P(c);
    Review *r = p->review;
    if (!r || c->state != VV_STATE_REVIEWING) return;
    p->review = NULL;
    if (c->review_draft) { g_free(r->entry->cleaned); r->entry->cleaned = g_strdup(c->review_draft); }
    g_free(c->review_draft); c->review_draft = NULL;
    g_free(c->review_route); c->review_route = NULL;
    if (!paste) { g_free(r->entry->status); r->entry->status = g_strdup("complete (not pasted)"); }
    if (paste && r->route_label && r->route_peer_fp) {
        char *label = g_strconcat(r->entry->cleanup_label, " · routed to ", r->route_label, NULL);
        g_free(r->entry->cleanup_label); r->entry->cleanup_label = label;
    }
    vv_library_save(c->library, r->entry);
    bump_library(c);
    if (paste && r->route_label && r->route_peer_fp) {
        char *m = g_strdup_printf("Sending to %s…", r->route_label);
        set_state(c, VV_STATE_PROCESSING, m);
        g_free(m);
        RouteDeliverTask *t = g_new0(RouteDeliverTask, 1);
        t->c = c; t->r = r;
        GThread *th = g_thread_new("vv-route-deliver", route_deliver_thread, t);
        g_thread_unref(th);
        return;
    }
    if (paste) deliver(c, r->entry, r->audio_path, r->folder); else set_state(c, VV_STATE_IDLE, NULL);
    review_free(r);
}

void vv_controller_receive_routed(VvController *c, const char *text,
                                  void (*done)(bool ok, const char *error, gpointer token), gpointer token) {
    if (vv_controller_is_busy(c)) { done(false, "busy dictating", token); return; }
    char *reason = NULL;
    VvPasteOutcome outcome = vv_paste_insert(text, c->config->auto_paste, &reason);
    char *id = NULL, *audio = NULL;
    vv_library_new_slot(c->library, c->config->active_folder, &id, &audio);
    VvEntry *entry = vv_entry_new();
    g_free(entry->id); entry->id = g_strdup(id);
    g_free(entry->folder); entry->folder = g_strdup(c->config->active_folder);
    entry->date_unix = g_get_real_time() / G_USEC_PER_SEC;
    g_free(entry->stt_label); entry->stt_label = g_strdup("routed from a paired machine");
    g_free(entry->cleaned); entry->cleaned = g_strdup(text);
    g_free(entry->raw); entry->raw = g_strdup(text);
    if (outcome == VV_PASTE_COPIED_ONLY) {
        g_free(entry->status); entry->status = g_strdup("complete (copied only)");
        char *m = g_strdup_printf("Routed text copied — %s. Press Ctrl+V to insert it.", reason ? reason : "");
        notify(c, m); g_free(m);
    }
    vv_library_save(c->library, entry);
    bump_library(c);
    vv_entry_free(entry);
    g_free(reason); g_free(id); g_free(audio);
    done(true, NULL, token);
}

/* ---------------------------------------------------- AI routing */

typedef struct {
    VvController *c;
    Review *r;                     /* checked against p->review before use */
    char *draft;
    char *router_provider_id;      /* or NULL */
    char *review_provider_id;
    char *cleanup_provider_id;     /* effective fallback chain */
    VvScreenshotSet *screens;
    /* results */
    char *route_label, *route_peer_fp;
    guint32 route_window;
    char *route_display;
} RouterTask;

static gboolean router_done(gpointer data) {
    RouterTask *t = data;
    VvController *c = t->c;
    Priv *p = P(c);
    if (p->review == t->r && c->state == VV_STATE_REVIEWING) {
        g_free(c->review_route);
        c->review_route = t->route_display; t->route_display = NULL;
        g_free(t->r->route_label); t->r->route_label = t->route_label; t->route_label = NULL;
        g_free(t->r->route_peer_fp); t->r->route_peer_fp = t->route_peer_fp; t->route_peer_fp = NULL;
        t->r->route_window = t->route_window;
        set_state(c, VV_STATE_REVIEWING, NULL);
    }
    g_free(t->draft); g_free(t->router_provider_id); g_free(t->review_provider_id);
    g_free(t->cleanup_provider_id);
    vv_screenshot_set_unref(t->screens);
    g_free(t->route_label); g_free(t->route_peer_fp); g_free(t->route_display);
    g_free(t);
    return G_SOURCE_REMOVE;
}

static gpointer router_thread(gpointer data) {
    RouterTask *t = data;
    VvController *c = t->c;
    VvConfig *cfg = c->config;
    const char *ids[] = { t->router_provider_id, t->review_provider_id, t->cleanup_provider_id };
    VvProvider *provider = NULL;
    for (int i = 0; i < 3 && !provider; i++)
        if (ids[i]) { VvProvider *p2 = vv_config_find_provider(cfg, ids[i]);
                      if (p2 && vv_kind_supports_chat(p2->kind) && *p2->chat_model) provider = p2; }
    for (guint i = 0; !provider && i < cfg->providers->len; i++) {
        VvProvider *p2 = g_ptr_array_index(cfg->providers, i);
        if (vv_kind_supports_chat(p2->kind) && *p2->chat_model) provider = p2;
    }
    if (!provider) { g_idle_add(router_done, t); return NULL; }
    const char *machine_name = vv_multi_machine_name(&cfg->multi_machine);
    GPtrArray *contexts = g_ptr_array_new_with_free_func((GDestroyNotify)vv_machine_context_free);
    g_ptr_array_add(contexts, vv_peer_service_local_context(machine_name, t->screens));
    for (guint i = 0; i < cfg->multi_machine.peers->len; i++) {
        VvPeer *peer = g_ptr_array_index(cfg->multi_machine.peers, i);
        VvMachineContext *ctx = vv_peer_service_fetch_context(peer);
        if (ctx) g_ptr_array_add(contexts, ctx);
    }
    GPtrArray *machines = g_ptr_array_new_with_free_func((GDestroyNotify)vv_router_machine_free);
    GPtrArray *attachments = g_ptr_array_new_with_free_func((GDestroyNotify)vv_screenshot_free);
    for (guint i = 0; i < contexts->len; i++) {
        VvMachineContext *ctx = g_ptr_array_index(contexts, i);
        g_ptr_array_add(machines, vv_router_machine_new(ctx->machine, ctx->is_local, ctx->window_lines));
        for (guint k = 0; k < ctx->screens->len; k++) {
            VvScreenshot *shot = g_ptr_array_index(ctx->screens, k);
            g_ptr_array_add(attachments, vv_screenshot_new(shot->jpeg, g_strdup(shot->caption)));
        }
    }
    char *user = vv_router_message(t->draft, machines);
    char *key = vv_secret_get(provider->id);
    char *reply = NULL, *error = NULL;
    bool ok = vv_provider_chat(provider, key, vv_router_prompt(), user, attachments, &reply, &error);
    if (!ok) { g_free(error); error = NULL; ok = vv_provider_chat(provider, key, vv_router_prompt(), user, NULL, &reply, &error); }
    if (ok) {
        char *machine = NULL; guint32 window = 0;
        if (vv_router_parse(reply, &machine, &window)) {
            for (guint i = 0; i < contexts->len; i++) {
                VvMachineContext *ctx = g_ptr_array_index(contexts, i);
                if (g_strcmp0(ctx->machine, machine) != 0 || ctx->is_local) continue;
                for (guint k = 0; k < cfg->multi_machine.peers->len; k++) {
                    VvPeer *peer = g_ptr_array_index(cfg->multi_machine.peers, k);
                    if (g_strcmp0(peer->name, ctx->machine) == 0) {
                        t->route_peer_fp = g_strdup(peer->fingerprint);
                        t->route_label = g_strdup(ctx->machine);
                        t->route_window = window;
                        t->route_display = g_strdup_printf("→ %s", ctx->machine);
                        break;
                    }
                }
            }
        }
        g_free(machine);
    } else {
        vv_log_error("Router failed: %s", error ? error : "?");
    }
    g_free(error); g_free(reply); g_free(key); g_free(user);
    g_ptr_array_unref(attachments); g_ptr_array_unref(machines); g_ptr_array_unref(contexts);
    g_idle_add(router_done, t);
    return NULL;
}

static void start_router(VvController *c, Review *r, VvProfile *profile) {
    VvEffective eff = vv_effective(profile, c->config);
    RouterTask *t = g_new0(RouterTask, 1);
    t->c = c; t->r = r;
    t->draft = g_strdup(r->entry->cleaned);
    t->router_provider_id = g_strdup(profile->router_provider_id);
    t->review_provider_id = g_strdup(profile->review_provider_id);
    t->cleanup_provider_id = eff.provider ? g_strdup(eff.provider->id) : NULL;
    t->screens = vv_screenshot_set_ref(r->screenshots);
    vv_effective_clear(&eff);
    GThread *th = g_thread_new("vv-router", router_thread, t);
    g_thread_unref(th);
}

void vv_controller_accept_review(VvController *c) { end_review(c, true); }
void vv_controller_discard_review(VvController *c) { end_review(c, false); }

void vv_controller_retry(VvController *c, VvEntry *entry) {
    if (vv_controller_is_busy(c)) return;
    char *audio = vv_library_audio_path(c->library, entry);
    if (g_file_test(audio, G_FILE_TEST_EXISTS)) {
        /* Reuse the screenshots saved with the entry; the screen has moved on. */
        VvScreenshotSet *saved = vv_library_load_screenshots(c->library, entry);
        run_pipeline(c, entry->id, audio, entry->folder, entry->duration, 0, NULL, saved);
        vv_screenshot_set_unref(saved);
    }
    g_free(audio);
}
