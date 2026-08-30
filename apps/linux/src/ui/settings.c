/* Settings dialog: Providers · Dictation · Folders · General · Multi-machine · About. */
#include <adwaita.h>
#include "ui/controller.h"
#include "core/cleanup.h"
#include "core/providers.h"
#include "core/log.h"
#include "platform/services.h"
#include "platform/peerservice.h"
#include <string.h>

VvController *vv_app_controller(void);
void vv_app_refresh(void);
void vv_app_toast(const char *text);

static AdwPreferencesDialog *dialog;
static GtkWindow *dialog_parent;
static AdwPreferencesPage *current_pages[6];

static void save(void) { vv_controller_save_config(vv_app_controller()); vv_app_refresh(); }
static void rebuild(void);

/* ------------------------------------------------------ helpers */

static GtkWidget *entry_row(const char *title, const char *value, void (*on_change)(GtkEditable *, gpointer), gpointer data) {
    GtkWidget *row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    gtk_editable_set_text(GTK_EDITABLE(row), value ? value : "");
    if (on_change) g_signal_connect(row, "changed", G_CALLBACK(on_change), data);
    return row;
}

static GtkWidget *switch_row(const char *title, const char *subtitle, bool value, void (*on_change)(GObject *, GParamSpec *, gpointer), gpointer data) {
    GtkWidget *row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle) adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row), value);
    if (on_change) g_signal_connect(row, "notify::active", G_CALLBACK(on_change), data);
    return row;
}

static GtkWidget *combo_row(const char *title, GtkStringList *items, guint selected, void (*on_change)(GObject *, GParamSpec *, gpointer), gpointer data) {
    GtkWidget *row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(items));
    adw_combo_row_set_selected(ADW_COMBO_ROW(row), selected);
    if (on_change) g_signal_connect(row, "notify::selected", G_CALLBACK(on_change), data);
    return row;
}

static GtkStringList *string_list(const char *first, ...) {
    GtkStringList *l = gtk_string_list_new(NULL);
    va_list ap; va_start(ap, first);
    for (const char *s = first; s; s = va_arg(ap, const char *)) gtk_string_list_append(l, s);
    va_end(ap);
    return l;
}

/* Provider pickers: "None"/"Default…" + matching providers. Returns the list
 * of provider ids parallel to the items after the first. */
static GtkStringList *provider_items(const char *first, bool (*filter)(VvProvider *), GPtrArray **ids_out) {
    VvConfig *c = vv_app_controller()->config;
    GtkStringList *l = gtk_string_list_new(NULL);
    gtk_string_list_append(l, first);
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < c->providers->len; i++) {
        VvProvider *p = g_ptr_array_index(c->providers, i);
        if (!filter(p)) continue;
        char *label = g_strdup_printf("%s — %s", p->name, filter == NULL ? "" : (vv_kind_supports_transcription(p->kind) && *p->stt_model && !vv_kind_supports_chat(p->kind)) ? p->stt_model : p->chat_model);
        gtk_string_list_append(l, label); g_free(label);
        g_ptr_array_add(ids, g_strdup(p->id));
    }
    *ids_out = ids;
    return l;
}

static bool is_stt(VvProvider *p) { return vv_kind_supports_transcription(p->kind) && *p->stt_model; }
static bool is_chat(VvProvider *p) { return vv_kind_supports_chat(p->kind) && *p->chat_model; }
static guint index_of(GPtrArray *ids, const char *id) {
    if (!id) return 0;
    for (guint i = 0; i < ids->len; i++) if (g_ascii_strcasecmp(g_ptr_array_index(ids, i), id) == 0) return i + 1;
    return 0;
}
static char *id_at(GPtrArray *ids, guint selected) { return selected == 0 || selected - 1 >= ids->len ? NULL : g_strdup(g_ptr_array_index(ids, selected - 1)); }

/* ---------------------------------------------------- providers */

typedef struct { VvProvider *p; GtkWidget *key; GtkWidget *status; } ProviderUi;

static void on_name(GtkEditable *e, gpointer d) { ProviderUi *u = d; g_free(u->p->name); u->p->name = g_strdup(gtk_editable_get_text(e)); save(); }
static void on_base(GtkEditable *e, gpointer d) { ProviderUi *u = d; g_free(u->p->base_url); u->p->base_url = g_strdup(gtk_editable_get_text(e)); save(); }
static void on_stt(GtkEditable *e, gpointer d) { ProviderUi *u = d; g_free(u->p->stt_model); u->p->stt_model = g_strdup(gtk_editable_get_text(e)); save(); }
static void on_chat(GtkEditable *e, gpointer d) { ProviderUi *u = d; g_free(u->p->chat_model); u->p->chat_model = g_strdup(gtk_editable_get_text(e)); save(); }
static void on_key_apply(GtkEditable *e, gpointer d) {
    ProviderUi *u = d;
    const char *key = gtk_editable_get_text(e);
    if (*key) vv_secret_set(u->p->id, key); else vv_secret_delete(u->p->id);
}
typedef struct { ProviderUi *u; char *key; char *error; bool ok; } TestTask;
static gboolean test_done(gpointer data) {
    TestTask *t = data;
    gtk_label_set_text(GTK_LABEL(t->u->status), t->ok ? "Connected ✓" : t->error);
    g_free(t->key); g_free(t->error); g_free(t);
    return G_SOURCE_REMOVE;
}
static gpointer test_thread(gpointer data) { TestTask *t = data; t->ok = vv_provider_test(t->u->p, t->key, &t->error); g_idle_add(test_done, t); return NULL; }
static void on_test(GtkButton *b, gpointer d) {
    ProviderUi *u = d;
    gtk_label_set_text(GTK_LABEL(u->status), "Testing…");
    TestTask *t = g_new0(TestTask, 1);
    t->u = u; t->key = vv_secret_get(u->p->id);
    g_thread_unref(g_thread_new("vv-test", test_thread, t));
}
static void on_remove(GtkButton *b, gpointer d) {
    ProviderUi *u = d;
    VvConfig *c = vv_app_controller()->config;
    vv_secret_delete(u->p->id);
    if (c->stt_provider_id && g_ascii_strcasecmp(c->stt_provider_id, u->p->id) == 0) { g_free(c->stt_provider_id); c->stt_provider_id = NULL; }
    if (c->cleanup.provider_id && g_ascii_strcasecmp(c->cleanup.provider_id, u->p->id) == 0) { g_free(c->cleanup.provider_id); c->cleanup.provider_id = NULL; }
    g_ptr_array_remove(c->providers, u->p);
    save(); rebuild();
}
static void on_add_provider(GObject *row, GParamSpec *ps, gpointer d) {
    guint sel = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    if (sel == 0) return;
    VvConfig *c = vv_app_controller()->config;
    VvProvider *p = vv_provider_preset((VvProviderKind)(sel - 1));
    g_ptr_array_add(c->providers, p);
    if (!c->stt_provider_id && vv_kind_supports_transcription(p->kind)) c->stt_provider_id = g_strdup(p->id);
    if (!c->cleanup.provider_id && vv_kind_supports_chat(p->kind)) c->cleanup.provider_id = g_strdup(p->id);
    save(); rebuild();
}

static AdwPreferencesPage *page_providers(void) {
    VvConfig *c = vv_app_controller()->config;
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(page, "Providers");
    adw_preferences_page_set_icon_name(page, "network-server-symbolic");
    for (guint i = 0; i < c->providers->len; i++) {
        VvProvider *p = g_ptr_array_index(c->providers, i);
        ProviderUi *u = g_new0(ProviderUi, 1); u->p = p;
        GtkWidget *group = adw_preferences_group_new();
        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), p->name);
        adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group), vv_kind_display_name(p->kind));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), entry_row("Name", p->name, on_name, u));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), entry_row("Base URL", p->base_url, on_base, u));
        if (vv_kind_supports_transcription(p->kind)) adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), entry_row("Transcription model", p->stt_model, on_stt, u));
        if (vv_kind_supports_chat(p->kind)) adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), entry_row("Chat model (cleanup)", p->chat_model, on_chat, u));
        GtkWidget *key = adw_password_entry_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(key), "API key");
        char *existing = vv_secret_get(p->id);
        if (existing) { gtk_editable_set_text(GTK_EDITABLE(key), existing); g_free(existing); }
        g_signal_connect(key, "apply", G_CALLBACK(on_key_apply), u);
        adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(key), TRUE);
        u->key = key;
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), key);
        GtkWidget *actions = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(actions), "Connection");
        u->status = gtk_label_new("");
        gtk_widget_add_css_class(u->status, "dim-label");
        adw_action_row_add_suffix(ADW_ACTION_ROW(actions), u->status);
        GtkWidget *test = gtk_button_new_with_label("Test");
        gtk_widget_set_valign(test, GTK_ALIGN_CENTER);
        g_signal_connect(test, "clicked", G_CALLBACK(on_test), u);
        adw_action_row_add_suffix(ADW_ACTION_ROW(actions), test);
        GtkWidget *remove = gtk_button_new_with_label("Remove");
        gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(remove, "destructive-action");
        g_signal_connect(remove, "clicked", G_CALLBACK(on_remove), u);
        adw_action_row_add_suffix(ADW_ACTION_ROW(actions), remove);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), actions);
        adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(group));
    }
    GtkWidget *add = adw_preferences_group_new();
    GtkStringList *kinds = string_list("Add provider…", "ElevenLabs (transcription)", "Fireworks (cleanup LLM)", "Cerebras (cleanup LLM)", "Vercel AI Gateway (STT + cleanup)", "OpenAI-compatible (Azure Foundry, Ollama, …)", NULL);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(add), combo_row("Add", kinds, 0, on_add_provider, NULL));
    adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(add));
    return page;
}

/* ---------------------------------------------------- dictation */

typedef struct { VvProfile *p; GPtrArray *stt_ids, *chat_ids, *review_ids; GtkWidget *hotkey_button;     GPtrArray *router_ids;
} ProfileUi;

static void on_stt_default(GObject *row, GParamSpec *ps, gpointer ids) {
    VvConfig *c = vv_app_controller()->config;
    g_free(c->stt_provider_id); c->stt_provider_id = id_at(ids, adw_combo_row_get_selected(ADW_COMBO_ROW(row))); save();
}
static void on_profile_name(GtkEditable *e, gpointer d) { ProfileUi *u = d; g_free(u->p->name); u->p->name = g_strdup(gtk_editable_get_text(e)); save(); }
static void on_profile_mode(GObject *row, GParamSpec *ps, gpointer d) {
    ProfileUi *u = d; u->p->has_cleanup_mode = true; u->p->cleanup_mode = (VvCleanupMode)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    u->p->cleanup_enabled = u->p->cleanup_mode != VV_CLEANUP_OFF; save(); rebuild();
}
static void on_profile_stt(GObject *row, GParamSpec *ps, gpointer d) { ProfileUi *u = d; g_free(u->p->stt_provider_id); u->p->stt_provider_id = id_at(u->stt_ids, adw_combo_row_get_selected(ADW_COMBO_ROW(row))); save(); }
static void on_profile_chat(GObject *row, GParamSpec *ps, gpointer d) { ProfileUi *u = d; g_free(u->p->cleanup_provider_id); u->p->cleanup_provider_id = id_at(u->chat_ids, adw_combo_row_get_selected(ADW_COMBO_ROW(row))); save(); }
static void on_profile_review_provider(GObject *row, GParamSpec *ps, gpointer d) { ProfileUi *u = d; g_free(u->p->review_provider_id); u->p->review_provider_id = id_at(u->review_ids, adw_combo_row_get_selected(ADW_COMBO_ROW(row))); save(); }
static void on_profile_vocab(GtkEditable *e, gpointer d) { ProfileUi *u = d; g_free(u->p->vocabulary); u->p->vocabulary = g_strdup(gtk_editable_get_text(e)); save(); }
static void on_profile_prompt(GtkTextBuffer *b, gpointer d) {
    ProfileUi *u = d; GtkTextIter s, e; gtk_text_buffer_get_bounds(b, &s, &e);
    g_free(u->p->custom_prompt); u->p->custom_prompt = gtk_text_buffer_get_text(b, &s, &e, FALSE); save();
}
static void on_profile_review(GObject *row, GParamSpec *ps, gpointer d) { ProfileUi *u = d; u->p->review_before_paste = adw_switch_row_get_active(ADW_SWITCH_ROW(row)); save(); rebuild(); }
static void on_profile_screenshot(GObject *row, GParamSpec *ps, gpointer d) { ProfileUi *u = d; u->p->screenshot_context = adw_switch_row_get_active(ADW_SWITCH_ROW(row)); save(); }
static void on_profile_router(GObject *row, GParamSpec *ps, gpointer d) { ProfileUi *u = d; u->p->router_enabled = adw_switch_row_get_active(ADW_SWITCH_ROW(row)); save(); rebuild(); }
static void on_profile_router_provider(GObject *row, GParamSpec *ps, gpointer d) {
    ProfileUi *u = d;
    guint sel = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    g_free(u->p->router_provider_id);
    u->p->router_provider_id = sel == 0 || sel > u->router_ids->len ? NULL : g_strdup(g_ptr_array_index(u->router_ids, sel - 1));
    save();
}
static void on_profile_remove(GtkButton *b, gpointer d) {
    ProfileUi *u = d; VvConfig *c = vv_app_controller()->config;
    if (c->profiles->len <= 1) return;
    g_ptr_array_remove(c->profiles, u->p); save(); rebuild();
}
static void on_captured(const VvHotkey *spec, gpointer d) {
    ProfileUi *u = d; u->p->hotkey = *spec; save(); rebuild();
}
static void on_hotkey_button(GtkButton *b, gpointer d) {
    ProfileUi *u = d;
    VvController *ctl = vv_app_controller();
    if (strcmp(vv_hotkey_engine_backend_name(ctl->hotkey), "raw") == 0) {
        gtk_button_set_label(b, "Press a key…");
        vv_hotkey_engine_capture(ctl->hotkey, on_captured, u);
    } else {
        vv_app_toast("Portal mode: shortcuts are chosen in the system dialog (Settings → Keyboard → Applications). Combos only.");
    }
}
static void on_add_profile(GtkButton *b, gpointer d) {
    VvConfig *c = vv_app_controller()->config;
    VvProfile *p = vv_profile_new();
    g_free(p->name); p->name = g_strdup_printf("Hotkey %u", c->profiles->len + 1);
    p->hotkey.key_code = 0; p->hotkey.is_modifier_only = false;
    p->has_cleanup_mode = true; p->cleanup_mode = vv_effective_mode(NULL, c);
    g_ptr_array_add(c->profiles, p); save(); rebuild();
}
static void on_tap_mode(GObject *row, GParamSpec *ps, gpointer d) { VvConfig *c = vv_app_controller()->config; c->tap_start_mode = adw_combo_row_get_selected(ADW_COMBO_ROW(row)) == 0 ? VV_TAP_DOUBLE : VV_TAP_SINGLE; save(); }
static void on_chunked(GObject *row, GParamSpec *ps, gpointer d) { vv_app_controller()->config->chunked_transcription = adw_switch_row_get_active(ADW_SWITCH_ROW(row)); save(); }
static void on_shared_vocab(GtkTextBuffer *b, gpointer d) {
    VvConfig *c = vv_app_controller()->config; GtkTextIter s, e; gtk_text_buffer_get_bounds(b, &s, &e);
    g_free(c->cleanup.vocabulary); c->cleanup.vocabulary = gtk_text_buffer_get_text(b, &s, &e, FALSE); save();
}

static GtkWidget *text_row(const char *title, const char *text, void (*on_change)(GtkTextBuffer *, gpointer), gpointer data, int height) {
    GtkWidget *row = adw_preferences_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(box, 8); gtk_widget_set_margin_bottom(box, 8); gtk_widget_set_margin_start(box, 12); gtk_widget_set_margin_end(box, 12);
    GtkWidget *label = gtk_label_new(title); gtk_label_set_xalign(GTK_LABEL(label), 0); gtk_widget_add_css_class(label, "dim-label");
    gtk_box_append(GTK_BOX(box), label);
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_widget_set_size_request(view, -1, height);
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_set_text(buf, text ? text : "", -1);
    g_signal_connect(buf, "changed", G_CALLBACK(on_change), data);
    gtk_box_append(GTK_BOX(box), view);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    return row;
}

static AdwPreferencesPage *page_dictation(void) {
    VvController *ctl = vv_app_controller();
    VvConfig *c = ctl->config;
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(page, "Dictation");
    adw_preferences_page_set_icon_name(page, "audio-input-microphone-symbolic");

    GtkWidget *stt = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(stt), "Transcription");
    GPtrArray *stt_ids; GtkStringList *stt_items = provider_items("None", is_stt, &stt_ids);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(stt), combo_row("Provider", stt_items, index_of(stt_ids, c->stt_provider_id), on_stt_default, stt_ids));
    adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(stt));

    GtkWidget *hk = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(hk), "Hotkeys");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(hk), "Each hotkey has its own cleanup: raw for LLM chats, polished for email — with its own model and prompt if you like.");
    GtkWidget *add = gtk_button_new_with_label("Add hotkey");
    gtk_widget_add_css_class(add, "flat");
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_profile), NULL);
    adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(hk), add);
    adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(hk));

    for (guint i = 0; i < c->profiles->len; i++) {
        VvProfile *p = g_ptr_array_index(c->profiles, i);
        ProfileUi *u = g_new0(ProfileUi, 1); u->p = p;
        GtkWidget *group = adw_preferences_group_new();
        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), p->name);
        if (c->profiles->len > 1) {
            GtkWidget *remove = gtk_button_new_from_icon_name("user-trash-symbolic");
            gtk_widget_add_css_class(remove, "flat");
            g_signal_connect(remove, "clicked", G_CALLBACK(on_profile_remove), u);
            adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(group), remove);
        }
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), entry_row("Name", p->name, on_profile_name, u));
        GtkWidget *hotkey = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(hotkey), "Hotkey");
        char *desc = p->hotkey.key_code == 0 ? g_strdup("Set hotkey…") : vv_hotkey_describe(&p->hotkey);
        char *trigger = vv_hotkey_portal_trigger(&p->hotkey);
        const char *backend = vv_hotkey_engine_backend_name(ctl->hotkey);
        if (strcmp(backend, "portal") == 0 && !trigger) adw_action_row_set_subtitle(ADW_ACTION_ROW(hotkey), "Modifier-only keys need raw input mode (Settings → General)");
        else if (strcmp(backend, "portal") == 0) adw_action_row_set_subtitle(ADW_ACTION_ROW(hotkey), "Bound through the system shortcut dialog; hold to talk or tap");
        g_free(trigger);
        u->hotkey_button = gtk_button_new_with_label(desc);
        g_free(desc);
        gtk_widget_set_valign(u->hotkey_button, GTK_ALIGN_CENTER);
        g_signal_connect(u->hotkey_button, "clicked", G_CALLBACK(on_hotkey_button), u);
        adw_action_row_add_suffix(ADW_ACTION_ROW(hotkey), u->hotkey_button);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), hotkey);
        GtkStringList *modes = string_list("Raw transcript", "Light cleanup", "Rich cleanup", NULL);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), combo_row("Cleanup", modes, vv_effective_mode(p, c), on_profile_mode, u));
        GtkStringList *stts = provider_items("Default transcriber", is_stt, &u->stt_ids);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), combo_row("Transcriber", stts, index_of(u->stt_ids, p->stt_provider_id), on_profile_stt, u));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), entry_row("Extra vocabulary (added to the shared list)", p->vocabulary, on_profile_vocab, u));
        if (vv_effective_mode(p, c) != VV_CLEANUP_OFF) {
            GtkStringList *chats = provider_items("Default cleanup model", is_chat, &u->chat_ids);
            adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), combo_row("Cleanup model", chats, index_of(u->chat_ids, p->cleanup_provider_id), on_profile_chat, u));
            VvEffective eff = vv_effective(p, c);
            char *shown = *p->custom_prompt ? g_strdup(p->custom_prompt) : vv_cleanup_system_prompt_base(&eff.config);
            vv_effective_clear(&eff);
            adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), text_row(*p->custom_prompt ? "Cleanup prompt (custom for this hotkey)" : "Cleanup prompt (built-in — any edit saves a custom prompt)", shown, on_profile_prompt, u, 110));
            g_free(shown);
        }
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), switch_row("Review before pasting", "Stage the text; press the hotkey and say a change. Ctrl+Alt+Enter pastes, Ctrl+Alt+Esc discards.", p->review_before_paste, on_profile_review, u));
        if (p->review_before_paste) {
            GtkStringList *reviewers = provider_items("Same as cleanup", is_chat, &u->review_ids);
            adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), combo_row("Review model", reviewers, index_of(u->review_ids, p->review_provider_id), on_profile_review_provider, u));
        }
        if (p->review_before_paste) {
            adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), switch_row("Route with AI", "A router model looks at your screens and paired machines and picks where the draft should go; the staging card shows its choice.", p->router_enabled, on_profile_router, u));
            if (p->router_enabled) {
                GtkStringList *routers = provider_items("Same as review", is_chat, &u->router_ids);
                adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), combo_row("Router model", routers, index_of(u->router_ids, p->router_provider_id), on_profile_router_provider, u));
            }
        }
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), switch_row("Screenshot context", "Attach a screenshot of every display (Screenshot portal) to cleanup and review calls. Models without vision ignore it.", p->screenshot_context, on_profile_screenshot, u));
        adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(group));
    }

    GtkWidget *behavior = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(behavior), "Tap behavior");
    GtkStringList *taps = string_list("Double-tap to start (tap once to stop)", "Single tap to start (tap again to stop)", NULL);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(behavior), combo_row("Start gesture", taps, c->tap_start_mode == VV_TAP_DOUBLE ? 0 : 1, on_tap_mode, NULL));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(behavior), switch_row("Transcribe during pauses", "Long dictations finish faster. Hold-to-talk always works: press and hold, speak, release.", c->chunked_transcription, on_chunked, NULL));
    adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(behavior));

    GtkWidget *vocab = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(vocab), "Shared vocabulary");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(vocab), "Names and jargon every hotkey should get right — comma separated. Sent to the transcriber when it supports it (ElevenLabs key terms, Whisper-style prompt) and always added to the cleanup prompt.");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(vocab), text_row("Vocabulary", c->cleanup.vocabulary, on_shared_vocab, NULL, 60));
    adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(vocab));
    return page;
}

/* ------------------------------------------------------ folders */

typedef struct { char *folder; } HookUi;
static VvWebhook *hook_for(const char *folder) {
    VvConfig *c = vv_app_controller()->config;
    VvWebhook *h = g_hash_table_lookup(c->folder_webhooks, folder);
    if (!h) { h = g_new0(VvWebhook, 1); h->url = g_strdup(""); g_hash_table_insert(c->folder_webhooks, g_strdup(folder), h); }
    return h;
}
static void on_hook_enabled(GObject *row, GParamSpec *ps, gpointer d) { HookUi *u = d; hook_for(u->folder)->enabled = adw_switch_row_get_active(ADW_SWITCH_ROW(row)); save(); }
static void on_hook_audio(GObject *row, GParamSpec *ps, gpointer d) { HookUi *u = d; hook_for(u->folder)->include_audio = adw_switch_row_get_active(ADW_SWITCH_ROW(row)); save(); }
static void on_hook_url(GtkEditable *e, gpointer d) { HookUi *u = d; VvWebhook *h = hook_for(u->folder); g_free(h->url); h->url = g_strdup(gtk_editable_get_text(e)); save(); }
static void on_new_folder(GtkEditable *e, gpointer d) {
    const char *name = gtk_editable_get_text(e);
    if (!*name) return;
    vv_library_create_folder(vv_app_controller()->library, name);
    gtk_editable_set_text(e, "");
    vv_app_refresh(); rebuild();
}

static AdwPreferencesPage *page_folders(void) {
    VvController *ctl = vv_app_controller();
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(page, "Folders");
    adw_preferences_page_set_icon_name(page, "folder-symbolic");
    GPtrArray *names = vv_library_folder_names(ctl->library);
    for (guint i = 0; i < names->len; i++) {
        const char *folder = g_ptr_array_index(names, i);
        HookUi *u = g_new0(HookUi, 1); u->folder = g_strdup(folder);
        VvWebhook *h = g_hash_table_lookup(ctl->config->folder_webhooks, folder);
        GtkWidget *group = adw_preferences_group_new();
        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), folder);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), switch_row("Forward to a webhook", NULL, h && h->enabled, on_hook_enabled, u));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), entry_row("Webhook URL", h ? h->url : "", on_hook_url, u));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), switch_row("Attach audio file (multipart)", NULL, h && h->include_audio, on_hook_audio, u));
        adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(group));
    }
    g_ptr_array_unref(names);
    GtkWidget *add = adw_preferences_group_new();
    GtkWidget *row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "New folder name (press Enter)");
    adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(row), TRUE);
    g_signal_connect(row, "apply", G_CALLBACK(on_new_folder), NULL);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(add), row);
    adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(add));
    return page;
}

/* ------------------------------------------------------ general */

static void on_sounds(GObject *r, GParamSpec *p, gpointer d) { vv_app_controller()->config->play_sounds = adw_switch_row_get_active(ADW_SWITCH_ROW(r)); save(); }
static void on_autopaste(GObject *r, GParamSpec *p, gpointer d) { vv_app_controller()->config->auto_paste = adw_switch_row_get_active(ADW_SWITCH_ROW(r)); save(); }
static void on_warm_after(GObject *r, GParamSpec *p, gpointer d) { vv_app_controller()->config->keep_mic_warm_after_recording = adw_switch_row_get_active(ADW_SWITCH_ROW(r)); save(); }
static void on_warm_always(GObject *r, GParamSpec *p, gpointer d) { vv_app_controller()->config->keep_mic_always_warm = adw_switch_row_get_active(ADW_SWITCH_ROW(r)); save(); }
static void on_backend(GObject *r, GParamSpec *p, gpointer d) { vv_app_controller()->config->hotkey_backend = (VvHotkeyBackend)adw_combo_row_get_selected(ADW_COMBO_ROW(r)); save(); rebuild(); }
static void on_library_path(GtkEditable *e, gpointer d) {
    VvController *ctl = vv_app_controller(); g_free(ctl->config->library_path); ctl->config->library_path = g_strdup(gtk_editable_get_text(e));
    vv_controller_save_config(ctl); vv_controller_reload_library(ctl); vv_app_refresh();
}
static void open_url(GtkButton *b, gpointer url) { gtk_uri_launcher_launch(gtk_uri_launcher_new(url), dialog_parent, NULL, NULL, NULL); }

static AdwPreferencesPage *page_general(void) {
    VvController *ctl = vv_app_controller();
    VvConfig *c = ctl->config;
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(page, "General");
    adw_preferences_page_set_icon_name(page, "emblem-system-symbolic");

    GtkWidget *behavior = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(behavior), "Behavior");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(behavior), switch_row("Play chime when recording starts/stops", NULL, c->play_sounds, on_sounds, NULL));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(behavior), switch_row("Paste transcript into the active app", "Uses the RemoteDesktop portal (one-time permission). Otherwise the text is copied and you press Ctrl+V.", c->auto_paste, on_autopaste, NULL));
    adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(behavior));

    GtkWidget *hk = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(hk), "Hotkey backend");
    char *status = vv_hotkey_engine_status(ctl->hotkey);
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(hk), status);
    g_free(status);
    GtkStringList *backends = string_list("Automatic (raw input when available, else portal)", "Portal shortcut (no setup; combos; hold-to-talk works)", "Raw input (/dev/input; any key incl. modifier-only)", NULL);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(hk), combo_row("Backend", backends, c->hotkey_backend, on_backend, NULL));
    GtkWidget *raw = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(raw), vv_raw_input_available() ? "Raw input: available ✓" : "Raw input: not available");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(raw), vv_raw_input_available() ? "This account can read /dev/input." : "Run:  sudo usermod -aG input $USER   then log out and back in. This lets VoiceVector see keystrokes globally — the same power macOS grants under Accessibility.");
    adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(raw), TRUE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(hk), raw);
    GtkWidget *paste = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(paste), vv_paste_portal_available() ? "Paste: RemoteDesktop portal available ✓" : "Paste: portal unavailable (clipboard only)");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(hk), paste);
    adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(hk));

    GtkWidget *mic = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(mic), "Microphone");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(mic), "Opening an audio interface can take a moment before recording starts. Keeping the microphone open avoids that, but the desktop shows its mic-in-use indicator while it's open.");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(mic), switch_row("Keep the microphone open for 15 seconds after a recording", NULL, c->keep_mic_warm_after_recording, on_warm_after, NULL));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(mic), switch_row("Always keep the microphone open while VoiceVector is running", NULL, c->keep_mic_always_warm, on_warm_always, NULL));
    adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(mic));

    GtkWidget *storage = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(storage), "Storage");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(storage), entry_row("Library folder", c->library_path, on_library_path, NULL));
    adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(storage));

    GPtrArray *errors = vv_log_recent_errors();
    if (errors->len) {
        GtkWidget *log = adw_preferences_group_new();
        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(log), "Recent errors");
        for (guint i = errors->len > 8 ? errors->len - 8 : 0; i < errors->len; i++) {
            GtkWidget *row = adw_action_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), g_ptr_array_index(errors, i));
            adw_preferences_group_add(ADW_PREFERENCES_GROUP(log), row);
        }
        adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(log));
    }
    g_ptr_array_unref(errors);
    return page;
}

/* -------------------------------------------------------- about */

static AdwPreferencesPage *page_about(void) {
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(page, "About");
    adw_preferences_page_set_icon_name(page, "help-about-symbolic");
    GtkWidget *g = adw_preferences_group_new();
    char *title = g_strdup_printf("VoiceVector %s", VV_VERSION);
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g), title); g_free(title);
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(g), "A Sammons Software LLC product. © 2026 Sammons Software LLC. All rights reserved.");
    const char *links[][2] = { { "Website", "https://voicevector.sammons.io" }, { "What's new", "https://github.com/Sammons/voicevector/blob/main/CHANGELOG.md" }, { "Source on GitHub", "https://github.com/Sammons/voicevector" } };
    for (int i = 0; i < 3; i++) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), links[i][0]);
        GtkWidget *b = gtk_button_new_from_icon_name("external-link-symbolic"); gtk_widget_add_css_class(b, "flat"); gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
        g_signal_connect(b, "clicked", G_CALLBACK(open_url), (gpointer)links[i][1]);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), b);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(g), row);
    }
    adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(g));

    GtkWidget *lic = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(lic), "Licensing");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(lic), "VoiceVector is source-available under the VoiceVector Community License.");
    const char *rows[][2] = {
        { "Free", "For individuals, and for any company or organization with fewer than 1,000 employees and contractors (counted together with affiliates). Commercial use included." },
        { "Commercial license — US $50 per seat per year", "Required for organizations with 1,000 or more employees and contractors. A seat is one person who uses VoiceVector for their work. Organizations that cross the threshold get a 90-day grace period; site licenses are available." },
        { "The same software for everyone", "No feature differences, no license keys, no telemetry. Compliance rests with the organization, like any other license in its software inventory." },
    };
    for (int i = 0; i < 3; i++) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), rows[i][0]);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), rows[i][1]);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(lic), row);
    }
    const char *llinks[][2] = { { "License text", "https://github.com/Sammons/voicevector/blob/main/LICENSE" }, { "Commercial terms", "https://github.com/Sammons/voicevector/blob/main/COMMERCIAL.md" }, { "Buy a commercial license", "mailto:sales@sammons.io?subject=VoiceVector%20commercial%20license" } };
    for (int i = 0; i < 3; i++) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), llinks[i][0]);
        GtkWidget *b = gtk_button_new_from_icon_name("external-link-symbolic"); gtk_widget_add_css_class(b, "flat"); gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
        g_signal_connect(b, "clicked", G_CALLBACK(open_url), (gpointer)llinks[i][1]);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), b);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(lic), row);
    }
    adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(lic));
    return page;
}

/* --------------------------------------------- multi-machine page */

static GtkWidget *pair_status_label;
static GtkWidget *pair_address_row;

static void on_mm_enabled(GObject *row, GParamSpec *ps, gpointer d) {
    VvConfig *c = vv_app_controller()->config;
    c->multi_machine.enabled = adw_switch_row_get_active(ADW_SWITCH_ROW(row));
    save();
    vv_peer_service_apply();
    rebuild();
}

static void on_mm_name(GtkEditable *e, gpointer d) {
    VvConfig *c = vv_app_controller()->config;
    g_free(c->multi_machine.machine_name);
    c->multi_machine.machine_name = g_strdup(gtk_editable_get_text(e));
    save();
}

static void on_mm_port(GtkEditable *e, gpointer d) {
    VvConfig *c = vv_app_controller()->config;
    int port = atoi(gtk_editable_get_text(e));
    if (port > 0 && port < 65536) { c->multi_machine.port = port; save(); vv_peer_service_apply(); }
}

static void on_peer_address(GtkEditable *e, gpointer d) {
    VvPeer *p = d;
    g_free(p->address); p->address = g_strdup(gtk_editable_get_text(e));
    save();
}

static void on_peer_screens(GObject *row, GParamSpec *ps, gpointer d) { VvPeer *p = d; p->allow_screens = adw_switch_row_get_active(ADW_SWITCH_ROW(row)); save(); }
static void on_peer_deliver(GObject *row, GParamSpec *ps, gpointer d) { VvPeer *p = d; p->allow_deliver = adw_switch_row_get_active(ADW_SWITCH_ROW(row)); save(); }

static void on_peer_remove(GtkButton *b, gpointer d) {
    VvPeer *p = d;
    VvConfig *c = vv_app_controller()->config;
    g_ptr_array_remove(c->multi_machine.peers, p);
    save();
    rebuild();
}

static void pair_set_status(const char *text) {
    if (pair_status_label && GTK_IS_LABEL(pair_status_label)) gtk_label_set_text(GTK_LABEL(pair_status_label), text);
}

typedef struct { void (*answer)(bool, gpointer); gpointer token; } PairAnswer;

static void on_pair_dialog_response(AdwAlertDialog *d, const char *response, gpointer data) {
    PairAnswer *pa = data;
    pa->answer(g_strcmp0(response, "pair") == 0, pa->token);
    g_free(pa);
}

static void show_pair_code_dialog(const char *title, const char *code,
                                  void (*answer)(bool, gpointer), gpointer token) {
    char *body = g_strdup_printf("Confirm only if the other machine shows this code:\n\n%.3s %s", code, code + 3);
    AdwAlertDialog *d = ADW_ALERT_DIALOG(adw_alert_dialog_new(title, body));
    g_free(body);
    adw_alert_dialog_add_responses(d, "cancel", "Cancel", "pair", "They match — pair", NULL);
    adw_alert_dialog_set_response_appearance(d, "pair", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(d, "pair");
    adw_alert_dialog_set_close_response(d, "cancel");
    PairAnswer *pa = g_new0(PairAnswer, 1);
    pa->answer = answer; pa->token = token;
    g_signal_connect(d, "response", G_CALLBACK(on_pair_dialog_response), pa);
    GtkWindow *parent = dialog_parent;
    adw_dialog_present(ADW_DIALOG(d), parent ? GTK_WIDGET(parent) : NULL);
}

static void on_pair_code(const char *code, void (*answer)(bool, gpointer), gpointer token, gpointer user) {
    show_pair_code_dialog("Pair machines?", code, answer, token);
}

static void on_pair_done(VvPeer *peer, const char *error, gpointer user) {
    VvConfig *c = vv_app_controller()->config;
    if (peer) {
        bool known = false;
        for (guint i = 0; i < c->multi_machine.peers->len; i++)
            if (g_strcmp0(((VvPeer *)g_ptr_array_index(c->multi_machine.peers, i))->fingerprint, peer->fingerprint) == 0) known = true;
        char *m = g_strdup_printf("Paired with %s.", peer->name);
        if (known) vv_peer_ref_free(peer);
        else { g_ptr_array_add(c->multi_machine.peers, peer); save(); }
        pair_set_status(m); g_free(m);
        rebuild();
    } else {
        char *m = g_strdup_printf("Pairing failed: %s", error ? error : "unknown error");
        pair_set_status(m); g_free(m);
    }
}

static void on_pair_clicked(GtkButton *b, gpointer d) {
    if (!pair_address_row) return;
    const char *address = gtk_editable_get_text(GTK_EDITABLE(pair_address_row));
    if (!address || !*address) return;
    pair_set_status("Connecting…");
    vv_peer_service_pair_async(address, on_pair_code, on_pair_done, NULL);
}

static AdwPreferencesPage *page_multimachine(void) {
    VvConfig *c = vv_app_controller()->config;
    VvMultiMachine *mm = &c->multi_machine;
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(page, "Multi-machine");
    adw_preferences_page_set_icon_name(page, "network-workgroup-symbolic");

    GtkWidget *self_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(self_group), "This machine");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(self_group),
        "Machines pair once with a 6-digit code confirmed on both screens, then talk over TLS with pinned identities. Use tailnet or LAN addresses only.");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(self_group), switch_row("Allow paired machines to connect", NULL, mm->enabled, on_mm_enabled, NULL));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(self_group), entry_row("Machine name (blank = host name)", mm->machine_name, on_mm_name, NULL));
    char port[16]; g_snprintf(port, sizeof port, "%d", mm->port);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(self_group), entry_row("Port", port, on_mm_port, NULL));
    if (mm->enabled) {
        char *fp = vv_peer_service_fingerprint_hex();
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Identity");
        char *sub = fp ? g_strdup_printf("%.16s…", fp) : g_strdup("created when the listener starts");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
        g_free(sub); g_free(fp);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(self_group), row);
    }
    adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(self_group));

    GtkWidget *peers = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(peers), "Paired machines");
    if (mm->peers->len == 0)
        adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(peers), "No machines paired yet.");
    for (guint i = 0; i < mm->peers->len; i++) {
        VvPeer *p = g_ptr_array_index(mm->peers, i);
        GtkWidget *head = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(head), *p->name ? p->name : "(unnamed)");
        char *sub = g_strdup_printf("%.12s…", p->fingerprint);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(head), sub);
        g_free(sub);
        GtkWidget *remove = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_widget_add_css_class(remove, "flat");
        gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
        g_signal_connect(remove, "clicked", G_CALLBACK(on_peer_remove), p);
        adw_action_row_add_suffix(ADW_ACTION_ROW(head), remove);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(peers), head);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(peers), entry_row("Address (host or host:port)", p->address, on_peer_address, p));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(peers), switch_row("May see my screens", NULL, p->allow_screens, on_peer_screens, p));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(peers), switch_row("May paste into me", NULL, p->allow_deliver, on_peer_deliver, p));
    }
    adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(peers));

    GtkWidget *pair = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(pair), "Pair a new machine");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(pair),
        "VoiceVector must be running (and enabled above) on the other machine. Start pairing from either side.");
    pair_address_row = entry_row("Other machine's address (host or host:port)", "", NULL, NULL);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(pair), pair_address_row);
    GtkWidget *actions = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(actions), "Pair");
    GtkWidget *go = gtk_button_new_with_label("Pair…");
    gtk_widget_set_valign(go, GTK_ALIGN_CENTER);
    g_signal_connect(go, "clicked", G_CALLBACK(on_pair_clicked), NULL);
    adw_action_row_add_suffix(ADW_ACTION_ROW(actions), go);
    pair_status_label = gtk_label_new("");
    gtk_widget_add_css_class(pair_status_label, "dim-label");
    adw_action_row_add_suffix(ADW_ACTION_ROW(actions), pair_status_label);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(pair), actions);
    adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(pair));
    return page;
}

/* -------------------------------------------------------- dialog */

static void on_closed(AdwDialog *d, gpointer data) {
    dialog = NULL;
    for (int i = 0; i < 6; i++) current_pages[i] = NULL;
    /* These widgets die with the dialog; a late pairing callback must not touch them. */
    pair_status_label = NULL; pair_address_row = NULL;
    /* An armed hotkey capture would otherwise write into a freed profile. */
    vv_hotkey_engine_cancel_capture(vv_app_controller()->hotkey);
}

static void rebuild(void) {
    if (!dialog) return;
    vv_hotkey_engine_cancel_capture(vv_app_controller()->hotkey);
    const char *current = adw_preferences_dialog_get_visible_page_name(dialog);
    char *keep = g_strdup(current ? current : "");
    AdwPreferencesPage *pages[] = { page_providers(), page_dictation(), page_folders(), page_general(), page_multimachine(), page_about() };
    const char *names[] = { "providers", "dictation", "folders", "general", "multimachine", "about" };
    for (int i = 0; i < 6; i++) if (current_pages[i]) { adw_preferences_dialog_remove(dialog, current_pages[i]); current_pages[i] = NULL; }
    for (int i = 0; i < 6; i++) { adw_preferences_page_set_name(pages[i], names[i]); adw_preferences_dialog_add(dialog, pages[i]); current_pages[i] = pages[i]; }
    if (*keep) adw_preferences_dialog_set_visible_page_name(dialog, keep);
    g_free(keep);
}

void vv_settings_open(gpointer app_unused, GtkWindow *parent) {
    if (dialog) return;
    dialog_parent = parent;
    dialog = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    adw_dialog_set_title(ADW_DIALOG(dialog), "Settings");
    adw_dialog_set_content_width(ADW_DIALOG(dialog), 720);
    adw_dialog_set_content_height(ADW_DIALOG(dialog), 640);
    g_signal_connect(dialog, "closed", G_CALLBACK(on_closed), NULL);
    rebuild();
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
}
