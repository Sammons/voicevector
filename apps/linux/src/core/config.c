#include "core/config.h"
#include "core/log.h"
#include <glib/gstdio.h>
#include <string.h>
#include <linux/input-event-codes.h>

/* ----------------------------------------------------------- kinds */

bool vv_kind_supports_transcription(VvProviderKind k) { return k != VV_KIND_FIREWORKS && k != VV_KIND_CEREBRAS; }
bool vv_kind_supports_vocabulary(VvProviderKind k) { return k == VV_KIND_ELEVENLABS || k == VV_KIND_OPENAI_COMPATIBLE; }
bool vv_kind_supports_chat(VvProviderKind k) { return k != VV_KIND_ELEVENLABS; }
bool vv_kind_supports_model_listing(VvProviderKind k) { return k != VV_KIND_ELEVENLABS; }

const char *vv_kind_display_name(VvProviderKind k) {
    switch (k) {
    case VV_KIND_ELEVENLABS: return "ElevenLabs";
    case VV_KIND_FIREWORKS: return "Fireworks AI";
    case VV_KIND_CEREBRAS: return "Cerebras";
    case VV_KIND_VERCEL_GATEWAY: return "Vercel AI Gateway";
    default: return "OpenAI-compatible";
    }
}

const char *vv_kind_default_base_url(VvProviderKind k) {
    switch (k) {
    case VV_KIND_ELEVENLABS: return "https://api.elevenlabs.io";
    case VV_KIND_FIREWORKS: return "https://api.fireworks.ai/inference/v1";
    case VV_KIND_CEREBRAS: return "https://api.cerebras.ai/v1";
    case VV_KIND_VERCEL_GATEWAY: return "https://ai-gateway.vercel.sh/v1";
    default: return "http://localhost:11434/v1";
    }
}

static const char *WIRE_NAMES[VV_KIND_COUNT] = { "elevenLabs", "fireworks", "cerebras", "vercelGateway", "openAICompatible" };
const char *vv_kind_wire_name(VvProviderKind k) { return WIRE_NAMES[k]; }
VvProviderKind vv_kind_from_wire(const char *wire) {
    for (int i = 0; i < VV_KIND_COUNT; i++) if (wire && strcmp(WIRE_NAMES[i], wire) == 0) return (VvProviderKind)i;
    return VV_KIND_OPENAI_COMPATIBLE;
}

char *vv_uuid_new(void) { return g_uuid_string_random(); }
static bool valid_uuid(const char *s) { return s && g_uuid_string_is_valid(s); }

/* ------------------------------------------------------- providers */

VvProvider *vv_provider_preset(VvProviderKind kind) {
    VvProvider *p = g_new0(VvProvider, 1);
    p->id = vv_uuid_new();
    p->kind = kind;
    p->base_url = g_strdup(vv_kind_default_base_url(kind));
    p->stt_model = g_strdup("");
    p->chat_model = g_strdup("");
    switch (kind) {
    case VV_KIND_ELEVENLABS: p->name = g_strdup("ElevenLabs"); g_free(p->stt_model); p->stt_model = g_strdup("scribe_v2"); break;
    case VV_KIND_FIREWORKS: p->name = g_strdup("Fireworks"); g_free(p->chat_model); p->chat_model = g_strdup("accounts/fireworks/models/gpt-oss-20b"); break;
    case VV_KIND_CEREBRAS: p->name = g_strdup("Cerebras"); g_free(p->chat_model); p->chat_model = g_strdup("gpt-oss-120b"); break;
    case VV_KIND_VERCEL_GATEWAY:
        p->name = g_strdup("Vercel AI Gateway");
        g_free(p->stt_model); p->stt_model = g_strdup("openai/gpt-4o-mini-transcribe");
        g_free(p->chat_model); p->chat_model = g_strdup("openai/gpt-4o-mini");
        break;
    default: p->name = g_strdup("Self-hosted"); g_free(p->stt_model); p->stt_model = g_strdup("whisper-1"); break;
    }
    return p;
}

void vv_provider_free(VvProvider *p) {
    if (!p) return;
    g_free(p->id); g_free(p->name); g_free(p->base_url); g_free(p->stt_model); g_free(p->chat_model); g_free(p);
}

VvJson *vv_provider_to_json(const VvProvider *p) {
    VvJson *o = vv_json_object();
    vv_json_object_set(o, "id", vv_json_string(p->id));
    vv_json_object_set(o, "kind", vv_json_string(vv_kind_wire_name(p->kind)));
    vv_json_object_set(o, "name", vv_json_string(p->name));
    vv_json_object_set(o, "baseURL", vv_json_string(p->base_url));
    vv_json_object_set(o, "sttModel", vv_json_string(p->stt_model));
    vv_json_object_set(o, "chatModel", vv_json_string(p->chat_model));
    return o;
}

VvProvider *vv_provider_from_json(const VvJson *j) {
    VvProvider *p = g_new0(VvProvider, 1);
    const char *id = vv_json_get_string(j, "id", NULL);
    p->id = valid_uuid(id) ? g_ascii_strdown(id, -1) : vv_uuid_new();
    p->kind = vv_kind_from_wire(vv_json_get_string(j, "kind", "openAICompatible"));
    p->name = g_strdup(vv_json_get_string(j, "name", ""));
    p->base_url = g_strdup(vv_json_get_string(j, "baseURL", vv_json_get_string(j, "baseUrl", "")));
    p->stt_model = g_strdup(vv_json_get_string(j, "sttModel", ""));
    p->chat_model = g_strdup(vv_json_get_string(j, "chatModel", ""));
    return p;
}

/* -------------------------------------------------------- profiles */

bool vv_hotkey_same(const VvHotkey *a, const VvHotkey *b) {
    return a->key_code == b->key_code && a->modifiers == b->modifiers && a->is_modifier_only == b->is_modifier_only;
}

static const char *MODE_WIRE[] = { "off", "light", "rich" };
const char *vv_cleanup_mode_wire(VvCleanupMode m) { return MODE_WIRE[m]; }
VvCleanupMode vv_cleanup_mode_from_wire(const char *w) {
    if (w && strcmp(w, "off") == 0) return VV_CLEANUP_OFF;
    if (w && strcmp(w, "light") == 0) return VV_CLEANUP_LIGHT;
    return VV_CLEANUP_RICH;
}

VvProfile *vv_profile_new(void) {
    VvProfile *p = g_new0(VvProfile, 1);
    p->id = vv_uuid_new();
    p->name = g_strdup("Default");
    p->hotkey.key_code = KEY_RIGHTALT;
    p->hotkey.modifiers = 0;
    p->hotkey.is_modifier_only = true;
    p->cleanup_enabled = true;
    p->custom_prompt = g_strdup("");
    p->vocabulary = g_strdup("");
    return p;
}

void vv_profile_free(VvProfile *p) {
    if (!p) return;
    g_free(p->id); g_free(p->name); g_free(p->cleanup_provider_id); g_free(p->custom_prompt);
    g_free(p->stt_provider_id); g_free(p->vocabulary); g_free(p->review_provider_id);
    g_free(p->router_provider_id); g_free(p);
}

static VvJson *hotkey_to_json(const VvHotkey *h) {
    VvJson *o = vv_json_object();
    vv_json_object_set(o, "keyCode", vv_json_number(h->key_code));
    vv_json_object_set(o, "modifiers", vv_json_number(h->modifiers));
    vv_json_object_set(o, "isModifierOnly", vv_json_bool(h->is_modifier_only));
    return o;
}

static void hotkey_from_json(VvHotkey *h, const VvJson *j) {
    if (!j) return;
    h->key_code = (int)vv_json_get_number(j, "keyCode", KEY_RIGHTALT);
    h->modifiers = (int)vv_json_get_number(j, "modifiers", 0);
    h->is_modifier_only = vv_json_get_bool(j, "isModifierOnly", true);
}

VvJson *vv_profile_to_json(const VvProfile *p) {
    VvJson *o = vv_json_object();
    vv_json_object_set(o, "id", vv_json_string(p->id));
    vv_json_object_set(o, "name", vv_json_string(p->name));
    vv_json_object_set(o, "hotkey", hotkey_to_json(&p->hotkey));
    bool enabled = p->has_cleanup_mode ? p->cleanup_mode != VV_CLEANUP_OFF : p->cleanup_enabled;
    vv_json_object_set(o, "cleanupEnabled", vv_json_bool(enabled));
    vv_json_object_set(o, "cleanupMode", p->has_cleanup_mode ? vv_json_string(vv_cleanup_mode_wire(p->cleanup_mode)) : vv_json_null());
    vv_json_object_set(o, "cleanupProviderID", vv_json_string(p->cleanup_provider_id));
    vv_json_object_set(o, "customPrompt", vv_json_string(p->custom_prompt));
    vv_json_object_set(o, "sttProviderID", vv_json_string(p->stt_provider_id));
    vv_json_object_set(o, "vocabulary", vv_json_string(p->vocabulary));
    vv_json_object_set(o, "reviewBeforePaste", vv_json_bool(p->review_before_paste));
    vv_json_object_set(o, "reviewProviderID", vv_json_string(p->review_provider_id));
    vv_json_object_set(o, "screenshotContext", vv_json_bool(p->screenshot_context));
    vv_json_object_set(o, "routerEnabled", vv_json_bool(p->router_enabled));
    vv_json_object_set(o, "routerProviderID", vv_json_string(p->router_provider_id));
    return o;
}

VvProfile *vv_profile_from_json(const VvJson *j) {
    VvProfile *p = vv_profile_new();
    const char *id = vv_json_get_string(j, "id", NULL);
    if (valid_uuid(id)) { g_free(p->id); p->id = g_ascii_strdown(id, -1); }
    g_free(p->name); p->name = g_strdup(vv_json_get_string(j, "name", "Default"));
    hotkey_from_json(&p->hotkey, vv_json_get_object(j, "hotkey"));
    p->cleanup_enabled = vv_json_get_bool(j, "cleanupEnabled", true);
    const char *mode = vv_json_get_string(j, "cleanupMode", NULL);
    if (mode && *mode) { p->has_cleanup_mode = true; p->cleanup_mode = vv_cleanup_mode_from_wire(mode); }
    const char *cp = vv_json_get_string(j, "cleanupProviderID", NULL);
    if (valid_uuid(cp)) p->cleanup_provider_id = g_ascii_strdown(cp, -1);
    g_free(p->custom_prompt); p->custom_prompt = g_strdup(vv_json_get_string(j, "customPrompt", ""));
    const char *sp = vv_json_get_string(j, "sttProviderID", NULL);
    if (valid_uuid(sp)) p->stt_provider_id = g_ascii_strdown(sp, -1);
    g_free(p->vocabulary); p->vocabulary = g_strdup(vv_json_get_string(j, "vocabulary", ""));
    p->review_before_paste = vv_json_get_bool(j, "reviewBeforePaste", false);
    const char *rp = vv_json_get_string(j, "reviewProviderID", NULL);
    if (valid_uuid(rp)) p->review_provider_id = g_ascii_strdown(rp, -1);
    p->screenshot_context = vv_json_get_bool(j, "screenshotContext", false);
    p->router_enabled = vv_json_get_bool(j, "routerEnabled", false);
    const char *rt = vv_json_get_string(j, "routerProviderID", NULL);
    if (valid_uuid(rt)) p->router_provider_id = g_ascii_strdown(rt, -1);
    return p;
}

/* ---------------------------------------------------------- config */

static void webhook_free(gpointer w) { VvWebhook *h = w; g_free(h->url); g_free(h); }

VvPeer *vv_peer_ref_new(void) {
    VvPeer *p = g_new0(VvPeer, 1);
    p->name = g_strdup(""); p->fingerprint = g_strdup(""); p->address = g_strdup("");
    return p;
}

void vv_peer_ref_free(VvPeer *p) {
    if (!p) return;
    g_free(p->name); g_free(p->fingerprint); g_free(p->address); g_free(p);
}

const char *vv_multi_machine_name(const VvMultiMachine *mm) {
    return mm->machine_name && *mm->machine_name ? mm->machine_name : g_get_host_name();
}

VvConfig *vv_config_new(void) {
    VvConfig *c = g_new0(VvConfig, 1);
    c->version = 1;
    c->profiles = g_ptr_array_new_with_free_func((GDestroyNotify)vv_profile_free);
    g_ptr_array_add(c->profiles, vv_profile_new());
    c->tap_start_mode = VV_TAP_DOUBLE;
    c->providers = g_ptr_array_new_with_free_func((GDestroyNotify)vv_provider_free);
    c->cleanup.mode = VV_CLEANUP_RICH;
    c->cleanup.vocabulary = g_strdup("");
    c->cleanup.custom_prompt = g_strdup("");
    c->play_sounds = true;
    c->chunked_transcription = true;
    c->auto_paste = true;
    c->active_folder = g_strdup("Inbox");
    c->folder_webhooks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, webhook_free);
    c->library_path = g_strdup("~/Documents/VoiceVector");
    c->keep_mic_warm_after_recording = true;
    c->keep_mic_always_warm = false;
    c->hotkey_backend = VV_HOTKEY_BACKEND_AUTO;
    c->multi_machine.machine_name = g_strdup("");
    c->multi_machine.port = 47800;
    c->multi_machine.peers = g_ptr_array_new_with_free_func((GDestroyNotify)vv_peer_ref_free);
    return c;
}

void vv_config_free(VvConfig *c) {
    if (!c) return;
    g_ptr_array_unref(c->profiles);
    g_ptr_array_unref(c->providers);
    g_free(c->multi_machine.machine_name);
    g_ptr_array_unref(c->multi_machine.peers);
    g_free(c->stt_provider_id);
    g_free(c->cleanup.provider_id); g_free(c->cleanup.vocabulary); g_free(c->cleanup.custom_prompt);
    g_free(c->active_folder);
    g_hash_table_unref(c->folder_webhooks);
    g_free(c->library_path);
    g_free(c);
}

static const char *BACKEND_WIRE[] = { "auto", "portal", "raw" };

VvJson *vv_config_to_json(const VvConfig *c) {
    VvJson *o = vv_json_object();
    vv_json_object_set(o, "version", vv_json_number(c->version));
    vv_json_object_set(o, "wizardCompleted", vv_json_bool(c->wizard_completed));
    VvJson *profiles = vv_json_array();
    for (guint i = 0; i < c->profiles->len; i++) vv_json_array_add(profiles, vv_profile_to_json(g_ptr_array_index(c->profiles, i)));
    vv_json_object_set(o, "dictationProfiles", profiles);
    vv_json_object_set(o, "tapStartMode", vv_json_string(c->tap_start_mode == VV_TAP_DOUBLE ? "doubleTap" : "singleTap"));
    VvJson *providers = vv_json_array();
    for (guint i = 0; i < c->providers->len; i++) vv_json_array_add(providers, vv_provider_to_json(g_ptr_array_index(c->providers, i)));
    vv_json_object_set(o, "providers", providers);
    vv_json_object_set(o, "sttProviderID", vv_json_string(c->stt_provider_id));
    VvJson *cleanup = vv_json_object();
    vv_json_object_set(cleanup, "mode", vv_json_string(vv_cleanup_mode_wire(c->cleanup.mode)));
    vv_json_object_set(cleanup, "providerID", vv_json_string(c->cleanup.provider_id));
    vv_json_object_set(cleanup, "vocabulary", vv_json_string(c->cleanup.vocabulary));
    vv_json_object_set(cleanup, "customPrompt", vv_json_string(c->cleanup.custom_prompt));
    vv_json_object_set(o, "cleanup", cleanup);
    vv_json_object_set(o, "playSounds", vv_json_bool(c->play_sounds));
    vv_json_object_set(o, "chunkedTranscription", vv_json_bool(c->chunked_transcription));
    vv_json_object_set(o, "autoPaste", vv_json_bool(c->auto_paste));
    vv_json_object_set(o, "activeFolder", vv_json_string(c->active_folder));
    VvJson *hooks = vv_json_object();
    GHashTableIter it; gpointer key, value;
    g_hash_table_iter_init(&it, c->folder_webhooks);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        VvWebhook *h = value;
        VvJson *hj = vv_json_object();
        vv_json_object_set(hj, "url", vv_json_string(h->url ? h->url : ""));
        vv_json_object_set(hj, "includeAudio", vv_json_bool(h->include_audio));
        vv_json_object_set(hj, "enabled", vv_json_bool(h->enabled));
        vv_json_object_set(hooks, key, hj);
    }
    vv_json_object_set(o, "folderWebhooks", hooks);
    vv_json_object_set(o, "libraryPath", vv_json_string(c->library_path));
    vv_json_object_set(o, "keepMicWarmAfterRecording", vv_json_bool(c->keep_mic_warm_after_recording));
    vv_json_object_set(o, "keepMicAlwaysWarm", vv_json_bool(c->keep_mic_always_warm));
    vv_json_object_set(o, "hotkeyBackend", vv_json_string(BACKEND_WIRE[c->hotkey_backend]));
    VvJson *mm = vv_json_object();
    vv_json_object_set(mm, "enabled", vv_json_bool(c->multi_machine.enabled));
    vv_json_object_set(mm, "machineName", vv_json_string(c->multi_machine.machine_name));
    vv_json_object_set(mm, "port", vv_json_number(c->multi_machine.port));
    VvJson *peers = vv_json_array();
    for (guint i = 0; i < c->multi_machine.peers->len; i++) {
        VvPeer *p = g_ptr_array_index(c->multi_machine.peers, i);
        VvJson *pj = vv_json_object();
        vv_json_object_set(pj, "name", vv_json_string(p->name));
        vv_json_object_set(pj, "fingerprint", vv_json_string(p->fingerprint));
        vv_json_object_set(pj, "address", vv_json_string(p->address));
        vv_json_object_set(pj, "allowScreens", vv_json_bool(p->allow_screens));
        vv_json_object_set(pj, "allowDeliver", vv_json_bool(p->allow_deliver));
        vv_json_array_add(peers, pj);
    }
    vv_json_object_set(mm, "peers", peers);
    vv_json_object_set(o, "multiMachine", mm);
    return o;
}

VvConfig *vv_config_from_json(const VvJson *j) {
    VvConfig *c = vv_config_new();
    if (!j || j->type != VV_JSON_OBJECT) return c;
    c->version = (int)vv_json_get_number(j, "version", 1);
    c->wizard_completed = vv_json_get_bool(j, "wizardCompleted", false);

    VvJson *profiles = vv_json_get_array(j, "dictationProfiles");
    if (profiles && vv_json_array_length(profiles) > 0) {
        g_ptr_array_set_size(c->profiles, 0);
        for (guint i = 0; i < vv_json_array_length(profiles); i++) {
            VvJson *pj = vv_json_array_get(profiles, i);
            if (pj && pj->type == VV_JSON_OBJECT) g_ptr_array_add(c->profiles, vv_profile_from_json(pj));
        }
        if (c->profiles->len == 0) g_ptr_array_add(c->profiles, vv_profile_new());
    } else {
        /* Legacy single-hotkey config. */
        VvProfile *p0 = g_ptr_array_index(c->profiles, 0);
        hotkey_from_json(&p0->hotkey, vv_json_get_object(j, "hotkey"));
    }

    c->tap_start_mode = strcmp(vv_json_get_string(j, "tapStartMode", "doubleTap"), "singleTap") == 0 ? VV_TAP_SINGLE : VV_TAP_DOUBLE;
    VvJson *providers = vv_json_get_array(j, "providers");
    for (guint i = 0; i < vv_json_array_length(providers); i++) {
        VvJson *pj = vv_json_array_get(providers, i);
        if (pj && pj->type == VV_JSON_OBJECT) g_ptr_array_add(c->providers, vv_provider_from_json(pj));
    }
    const char *stt = vv_json_get_string(j, "sttProviderID", NULL);
    if (valid_uuid(stt)) c->stt_provider_id = g_ascii_strdown(stt, -1);
    VvJson *cleanup = vv_json_get_object(j, "cleanup");
    if (cleanup) {
        c->cleanup.mode = vv_cleanup_mode_from_wire(vv_json_get_string(cleanup, "mode", "rich"));
        const char *cp = vv_json_get_string(cleanup, "providerID", NULL);
        if (valid_uuid(cp)) c->cleanup.provider_id = g_ascii_strdown(cp, -1);
        g_free(c->cleanup.vocabulary); c->cleanup.vocabulary = g_strdup(vv_json_get_string(cleanup, "vocabulary", ""));
        g_free(c->cleanup.custom_prompt); c->cleanup.custom_prompt = g_strdup(vv_json_get_string(cleanup, "customPrompt", ""));
    }
    c->play_sounds = vv_json_get_bool(j, "playSounds", true);
    c->chunked_transcription = vv_json_get_bool(j, "chunkedTranscription", true);
    c->auto_paste = vv_json_get_bool(j, "autoPaste", true);
    g_free(c->active_folder); c->active_folder = g_strdup(vv_json_get_string(j, "activeFolder", "Inbox"));
    VvJson *hooks = vv_json_get_object(j, "folderWebhooks");
    if (hooks) {
        for (guint i = 0; i < hooks->keys->len; i++) {
            VvJson *hj = g_ptr_array_index(hooks->values, i);
            if (!hj || hj->type != VV_JSON_OBJECT) continue;
            VvWebhook *h = g_new0(VvWebhook, 1);
            h->url = g_strdup(vv_json_get_string(hj, "url", ""));
            h->include_audio = vv_json_get_bool(hj, "includeAudio", false);
            h->enabled = vv_json_get_bool(hj, "enabled", false);
            g_hash_table_insert(c->folder_webhooks, g_strdup(g_ptr_array_index(hooks->keys, i)), h);
        }
    }
    g_free(c->library_path); c->library_path = g_strdup(vv_json_get_string(j, "libraryPath", "~/Documents/VoiceVector"));
    c->keep_mic_warm_after_recording = vv_json_get_bool(j, "keepMicWarmAfterRecording", true);
    c->keep_mic_always_warm = vv_json_get_bool(j, "keepMicAlwaysWarm", false);
    const char *backend = vv_json_get_string(j, "hotkeyBackend", "auto");
    c->hotkey_backend = strcmp(backend, "portal") == 0 ? VV_HOTKEY_BACKEND_PORTAL
                      : strcmp(backend, "raw") == 0 ? VV_HOTKEY_BACKEND_RAW : VV_HOTKEY_BACKEND_AUTO;
    VvJson *mm = vv_json_get_object(j, "multiMachine");
    if (mm) {
        c->multi_machine.enabled = vv_json_get_bool(mm, "enabled", false);
        g_free(c->multi_machine.machine_name);
        c->multi_machine.machine_name = g_strdup(vv_json_get_string(mm, "machineName", ""));
        c->multi_machine.port = (int)vv_json_get_number(mm, "port", 47800);
        VvJson *peers = vv_json_get_array(mm, "peers");
        for (guint i = 0; peers && i < vv_json_array_length(peers); i++) {
            VvJson *pj = vv_json_array_get(peers, i);
            if (!pj || pj->type != VV_JSON_OBJECT) continue;
            VvPeer *p = vv_peer_ref_new();
            g_free(p->name); p->name = g_strdup(vv_json_get_string(pj, "name", ""));
            g_free(p->fingerprint); p->fingerprint = g_strdup(vv_json_get_string(pj, "fingerprint", ""));
            g_free(p->address); p->address = g_strdup(vv_json_get_string(pj, "address", ""));
            p->allow_screens = vv_json_get_bool(pj, "allowScreens", false);
            p->allow_deliver = vv_json_get_bool(pj, "allowDeliver", false);
            g_ptr_array_add(c->multi_machine.peers, p);
        }
    }
    return c;
}

char *vv_config_expanded_library_path(const VvConfig *c) {
    const char *p = c->library_path;
    if (p[0] == '~' && (p[1] == '/' || p[1] == '\0')) return g_build_filename(g_get_home_dir(), p + 1, NULL);
    return g_strdup(p);
}

const VvHotkey *vv_config_primary_hotkey(const VvConfig *c) {
    return &((VvProfile *)g_ptr_array_index(c->profiles, 0))->hotkey;
}

VvProvider *vv_config_find_provider(const VvConfig *c, const char *id) {
    if (!id) return NULL;
    for (guint i = 0; i < c->providers->len; i++) {
        VvProvider *p = g_ptr_array_index(c->providers, i);
        if (g_ascii_strcasecmp(p->id, id) == 0) return p;
    }
    return NULL;
}

VvProfile *vv_config_find_profile(const VvConfig *c, const char *id) {
    if (!id) return NULL;
    for (guint i = 0; i < c->profiles->len; i++) {
        VvProfile *p = g_ptr_array_index(c->profiles, i);
        if (g_ascii_strcasecmp(p->id, id) == 0) return p;
    }
    return NULL;
}

char *vv_config_default_path(void) { return g_build_filename(g_get_user_config_dir(), "voicevector", "config.json", NULL); }

VvConfig *vv_config_load(const char *path) {
    char *text = NULL;
    if (!g_file_get_contents(path, &text, NULL, NULL)) return vv_config_new();
    char *error = NULL;
    VvJson *j = vv_json_parse(text, &error);
    g_free(text);
    if (!j) { vv_log_error("config.json unreadable (%s) — using defaults", error ? error : "?"); g_free(error); return vv_config_new(); }
    VvConfig *c = vv_config_from_json(j);
    vv_json_free(j);
    return c;
}

bool vv_config_save(const VvConfig *c, const char *path) {
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    VvJson *j = vv_config_to_json(c);
    char *text = vv_json_write(j, 2);
    vv_json_free(j);
    GError *err = NULL;
    bool ok = g_file_set_contents(path, text, -1, &err);
    if (!ok) { vv_log_error("Failed to save config: %s", err->message); g_error_free(err); }
    g_free(text);
    return ok;
}
