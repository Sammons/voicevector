/* Mirrors docs/config-schema.md. JSON mapping is explicit and tolerant by
 * construction: missing keys keep defaults, unknown keys are ignored, and a
 * legacy top-level "hotkey" migrates into dictationProfiles[0]. */
#pragma once
#include <glib.h>
#include <stdbool.h>
#include "core/json.h"
#include "core/tap.h"

typedef enum { VV_KIND_ELEVENLABS, VV_KIND_FIREWORKS, VV_KIND_CEREBRAS, VV_KIND_VERCEL_GATEWAY, VV_KIND_OPENAI_COMPATIBLE } VvProviderKind;
#define VV_KIND_COUNT 5

bool vv_kind_supports_transcription(VvProviderKind k);
bool vv_kind_supports_vocabulary(VvProviderKind k);
bool vv_kind_supports_chat(VvProviderKind k);
bool vv_kind_supports_model_listing(VvProviderKind k);
const char *vv_kind_display_name(VvProviderKind k);
const char *vv_kind_default_base_url(VvProviderKind k);
const char *vv_kind_wire_name(VvProviderKind k);
VvProviderKind vv_kind_from_wire(const char *wire);

typedef struct {
    char *id;            /* UUID string, lowercase */
    VvProviderKind kind;
    char *name;
    char *base_url;
    char *stt_model;
    char *chat_model;
} VvProvider;

VvProvider *vv_provider_preset(VvProviderKind kind);
void vv_provider_free(VvProvider *p);
VvJson *vv_provider_to_json(const VvProvider *p);
VvProvider *vv_provider_from_json(const VvJson *j);

typedef struct {
    int key_code;         /* evdev KEY_* code (Linux keycode); default KEY_RIGHTALT */
    int modifiers;        /* 1=Ctrl, 2=Alt, 4=Shift, 8=Super; 0 for modifier-only */
    bool is_modifier_only;
} VvHotkey;
bool vv_hotkey_same(const VvHotkey *a, const VvHotkey *b);

typedef enum { VV_CLEANUP_OFF, VV_CLEANUP_LIGHT, VV_CLEANUP_RICH } VvCleanupMode;
const char *vv_cleanup_mode_wire(VvCleanupMode m);
VvCleanupMode vv_cleanup_mode_from_wire(const char *wire);

typedef struct {
    char *id;
    char *name;
    VvHotkey hotkey;
    bool cleanup_enabled;          /* legacy switch, kept in sync with cleanup_mode */
    bool has_cleanup_mode;
    VvCleanupMode cleanup_mode;
    char *cleanup_provider_id;     /* NULL = global */
    char *custom_prompt;           /* "" = global */
    char *stt_provider_id;         /* NULL = global */
    char *vocabulary;              /* appended to global */
    bool review_before_paste;
    char *review_provider_id;      /* NULL = cleanup provider */
    bool screenshot_context;
    bool router_enabled;           /* AI routing; needs review_before_paste */
    char *router_provider_id;      /* NULL = review provider */
} VvProfile;

VvProfile *vv_profile_new(void);
void vv_profile_free(VvProfile *p);
VvJson *vv_profile_to_json(const VvProfile *p);
VvProfile *vv_profile_from_json(const VvJson *j);

typedef struct {
    VvCleanupMode mode;
    char *provider_id;     /* NULL = none */
    char *vocabulary;
    char *custom_prompt;
} VvCleanupConfig;

typedef struct { char *url; bool include_audio; bool enabled; } VvWebhook;

/* A paired machine (docs/multi-machine.md). */
typedef struct {
    char *name;
    char *fingerprint;    /* lowercase-hex SHA-256 of the peer's certificate (DER) */
    char *address;        /* host or host:port; "" = inbound-only */
    bool allow_screens;   /* peer may fetch my screens/windows */
    bool allow_deliver;   /* peer may paste into me */
} VvPeer;
VvPeer *vv_peer_ref_new(void);
void vv_peer_ref_free(VvPeer *p);

/* Multi-machine peering settings (docs/multi-machine.md). */
typedef struct {
    bool enabled;
    char *machine_name;   /* "" = the host name */
    int port;
    GPtrArray *peers;     /* VvPeer* */
} VvMultiMachine;
const char *vv_multi_machine_name(const VvMultiMachine *mm);

typedef enum { VV_HOTKEY_BACKEND_AUTO, VV_HOTKEY_BACKEND_PORTAL, VV_HOTKEY_BACKEND_RAW } VvHotkeyBackend;

typedef struct {
    int version;
    bool wizard_completed;
    GPtrArray *profiles;           /* VvProfile*, always ≥ 1 */
    VvTapStartMode tap_start_mode;
    GPtrArray *providers;          /* VvProvider* */
    char *stt_provider_id;         /* NULL = none */
    VvCleanupConfig cleanup;
    bool play_sounds;
    bool chunked_transcription;
    bool auto_paste;
    char *active_folder;
    GHashTable *folder_webhooks;   /* char* → VvWebhook* */
    char *library_path;            /* "~" expanded on use */
    bool keep_mic_warm_after_recording;
    bool keep_mic_always_warm;
    VvHotkeyBackend hotkey_backend; /* Linux-only */
    VvMultiMachine multi_machine;
} VvConfig;

VvConfig *vv_config_new(void);
void vv_config_free(VvConfig *c);
VvJson *vv_config_to_json(const VvConfig *c);
VvConfig *vv_config_from_json(const VvJson *j);
char *vv_config_expanded_library_path(const VvConfig *c);
const VvHotkey *vv_config_primary_hotkey(const VvConfig *c);
VvProvider *vv_config_find_provider(const VvConfig *c, const char *id);   /* borrowed or NULL */
VvProfile *vv_config_find_profile(const VvConfig *c, const char *id);     /* borrowed or NULL */

/* Store: ~/.config/voicevector/config.json (XDG), pretty-printed. */
char *vv_config_default_path(void);
VvConfig *vv_config_load(const char *path);     /* defaults when missing/invalid */
bool vv_config_save(const VvConfig *c, const char *path);

char *vv_uuid_new(void);
