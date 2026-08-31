/* GTK4 / libadwaita shell: main window (library + status), recording HUD,
 * settings dialog. Keeps to the VoiceVector family language: quiet, few
 * screens, everything in plain sight. */
#include <adwaita.h>
#include "ui/controller.h"
#include "core/cleanup.h"
#include "core/providers.h"
#include "core/log.h"
#include "platform/services.h"
#include "platform/peerservice.h"
#include <string.h>

typedef struct {
    AdwApplication *app;
    VvController *ctl;
    GtkWindow *window;
    GtkLabel *status;
    GtkButton *record;
    GtkListBox *entries;
    GtkDropDown *folders;
    AdwToastOverlay *toasts;
    /* HUD */
    GtkWindow *hud;
    GtkLabel *hud_label, *hud_clock, *hud_draft, *hud_hint, *hud_route;
    GtkWidget *hud_staging, *hud_bars_box;
    GtkWidget *bars[14];
    double levels[14];
    guint hud_timer;
    guint last_generation;
} App;

static App *app_singleton;

static void toast(App *a, const char *text) {
    if (a->toasts) adw_toast_overlay_add_toast(a->toasts, adw_toast_new(text));
}

/* --------------------------------------------------------- HUD */

static gboolean hud_tick(gpointer data) {
    App *a = data;
    VvController *c = a->ctl;
    if (c->state == VV_STATE_RECORDING) {
        for (int i = 0; i < 13; i++) a->levels[i] = a->levels[i + 1];
        a->levels[13] = MAX(0.12, vv_recorder_level(c->recorder));
        for (int i = 0; i < 14; i++) gtk_widget_set_size_request(a->bars[i], 3, (int)(6 + a->levels[i] * 22));
        int s = (int)vv_recorder_elapsed(c->recorder);
        char clock[16]; g_snprintf(clock, sizeof clock, "%d:%02d", s / 60, s % 60);
        gtk_label_set_text(a->hud_clock, clock);
    }
    return G_SOURCE_CONTINUE;
}

static void hud_build(App *a) {
    GtkWidget *win = gtk_window_new();
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_title(GTK_WINDOW(win), "VoiceVector");
    gtk_widget_add_css_class(win, "vv-hud");
    GtkWidget *stack = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(stack, 8); gtk_widget_set_margin_bottom(stack, 8);
    gtk_widget_set_margin_start(stack, 8); gtk_widget_set_margin_end(stack, 8);

    a->hud_staging = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(a->hud_staging, "vv-card");
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 170);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    a->hud_draft = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_wrap(a->hud_draft, TRUE); gtk_label_set_xalign(a->hud_draft, 0); gtk_label_set_selectable(a->hud_draft, TRUE);
    gtk_widget_set_size_request(GTK_WIDGET(a->hud_draft), 480, -1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(a->hud_draft));
    gtk_box_append(GTK_BOX(a->hud_staging), scroll);
    a->hud_route = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(a->hud_route), "accent");
    gtk_label_set_xalign(a->hud_route, 0);
    gtk_widget_set_visible(GTK_WIDGET(a->hud_route), FALSE);
    gtk_box_append(GTK_BOX(a->hud_staging), GTK_WIDGET(a->hud_route));
    a->hud_hint = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(a->hud_hint), "dim-label");
    gtk_label_set_xalign(a->hud_hint, 0);
    gtk_box_append(GTK_BOX(a->hud_staging), GTK_WIDGET(a->hud_hint));
    gtk_box_append(GTK_BOX(stack), a->hud_staging);

    GtkWidget *pill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(pill, "vv-pill");
    gtk_widget_set_halign(pill, GTK_ALIGN_CENTER);
    a->hud_bars_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_widget_set_valign(a->hud_bars_box, GTK_ALIGN_CENTER);
    for (int i = 0; i < 14; i++) {
        a->bars[i] = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(a->bars[i], "vv-bar");
        gtk_widget_set_size_request(a->bars[i], 3, 6);
        gtk_widget_set_valign(a->bars[i], GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(a->hud_bars_box), a->bars[i]);
        a->levels[i] = 0.12;
    }
    gtk_box_append(GTK_BOX(pill), a->hud_bars_box);
    a->hud_label = GTK_LABEL(gtk_label_new("Listening…"));
    gtk_box_append(GTK_BOX(pill), GTK_WIDGET(a->hud_label));
    a->hud_clock = GTK_LABEL(gtk_label_new("0:00"));
    gtk_widget_add_css_class(GTK_WIDGET(a->hud_clock), "monospace");
    gtk_box_append(GTK_BOX(pill), GTK_WIDGET(a->hud_clock));
    gtk_box_append(GTK_BOX(stack), pill);
    gtk_window_set_child(GTK_WINDOW(win), stack);
    a->hud = GTK_WINDOW(win);
}

static void hud_update(App *a) {
    VvController *c = a->ctl;
    bool show = c->state == VV_STATE_RECORDING || c->state == VV_STATE_PROCESSING || c->state == VV_STATE_REVIEWING;
    if (!show) { if (gtk_widget_get_visible(GTK_WIDGET(a->hud))) gtk_widget_set_visible(GTK_WIDGET(a->hud), FALSE); if (a->hud_timer) { g_source_remove(a->hud_timer); a->hud_timer = 0; } return; }
    bool recording = c->state == VV_STATE_RECORDING;
    gtk_widget_set_visible(a->hud_bars_box, recording);
    gtk_widget_set_visible(GTK_WIDGET(a->hud_clock), recording);
    gtk_label_set_text(a->hud_label, recording ? "Listening…" : c->state == VV_STATE_REVIEWING ? "Reviewing" : c->detail);
    if (c->review_draft) {
        gtk_label_set_text(a->hud_draft, c->review_draft);
        const char *hint = recording ? "Listening for a change…" : c->state == VV_STATE_PROCESSING ? c->detail : "Press the hotkey and say a change";
        char *h = g_strdup_printf("%s      Ctrl+Alt+Enter: paste   Ctrl+Alt+Esc: discard", hint);
        gtk_label_set_text(a->hud_hint, h); g_free(h);
        if (c->review_route) { gtk_label_set_text(a->hud_route, c->review_route); gtk_widget_set_visible(GTK_WIDGET(a->hud_route), TRUE); }
        else gtk_widget_set_visible(GTK_WIDGET(a->hud_route), FALSE);
        gtk_widget_set_visible(a->hud_staging, TRUE);
    } else gtk_widget_set_visible(a->hud_staging, FALSE);
    if (!gtk_widget_get_visible(GTK_WIDGET(a->hud))) gtk_widget_set_visible(GTK_WIDGET(a->hud), TRUE);
    if (!a->hud_timer) a->hud_timer = g_timeout_add(50, hud_tick, a);
}

/* --------------------------------------------------- main window */

static void copy_text(GtkButton *b, gpointer data) {
    const char *text = data;
    gdk_clipboard_set_text(gdk_display_get_clipboard(gdk_display_get_default()), text);
    toast(app_singleton, "Copied");
}

static void reload_entries(App *a) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(a->entries)))) gtk_list_box_remove(a->entries, child);
    guint sel = gtk_drop_down_get_selected(a->folders);
    GPtrArray *names = vv_library_folder_names(a->ctl->library);
    const char *folder = sel < names->len ? g_ptr_array_index(names, sel) : "Inbox";
    GPtrArray *ids = vv_library_entry_ids(a->ctl->library, folder);
    for (guint i = 0; i < ids->len && i < 200; i++) {
        VvEntry *e = vv_library_get_entry(a->ctl->library, folder, g_ptr_array_index(ids, i));
        if (!e) continue;
        GtkWidget *row = adw_action_row_new();
        char *date = vv_library_render_date(e->date_unix);
        char *title = g_strdup_printf("%s · %.1fs · %s", date, e->duration, vv_entry_is_error(e) ? e->status : e->cleanup_label);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), e->cleaned);
        adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 4);
        GtkWidget *copy = gtk_button_new_from_icon_name("edit-copy-symbolic");
        gtk_widget_set_valign(copy, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(copy, "flat");
        g_signal_connect_data(copy, "clicked", G_CALLBACK(copy_text), g_strdup(e->cleaned), (GClosureNotify)g_free, 0);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), copy);
        gtk_list_box_append(a->entries, row);
        g_free(title); g_free(date); vv_entry_free(e);
    }
    g_ptr_array_unref(ids); g_ptr_array_unref(names);
}

static void reload_folders(App *a) {
    GPtrArray *names = vv_library_folder_names(a->ctl->library);
    GtkStringList *list = gtk_string_list_new(NULL);
    guint selected = 0;
    for (guint i = 0; i < names->len; i++) {
        gtk_string_list_append(list, g_ptr_array_index(names, i));
        if (strcmp(g_ptr_array_index(names, i), a->ctl->config->active_folder) == 0) selected = i;
    }
    gtk_drop_down_set_model(a->folders, G_LIST_MODEL(list));
    gtk_drop_down_set_selected(a->folders, selected);
    g_ptr_array_unref(names);
}

static void on_folder_changed(GObject *dd, GParamSpec *ps, gpointer data) {
    App *a = data;
    guint sel = gtk_drop_down_get_selected(a->folders);
    GPtrArray *names = vv_library_folder_names(a->ctl->library);
    if (sel < names->len && strcmp(g_ptr_array_index(names, sel), a->ctl->config->active_folder) != 0) {
        g_free(a->ctl->config->active_folder); a->ctl->config->active_folder = g_strdup(g_ptr_array_index(names, sel));
        vv_controller_save_config(a->ctl);
    }
    g_ptr_array_unref(names);
    reload_entries(a);
}

static void on_state(VvController *c, gpointer data) {
    App *a = data;
    char *hk = vv_hotkey_describe(vv_config_primary_hotkey(c->config));
    const char *backend = vv_hotkey_engine_backend_name(c->hotkey);
    char *idle = strcmp(backend, "none") == 0
        ? g_strdup("No hotkey backend — see Settings → General")
        : g_strdup_printf("Press %s anywhere to dictate%s", hk, strcmp(backend, "portal") == 0 ? " (portal shortcut)" : "");
    switch (c->state) {
    case VV_STATE_IDLE: gtk_label_set_text(a->status, idle); break;
    case VV_STATE_RECORDING: gtk_label_set_text(a->status, "Recording…"); break;
    case VV_STATE_PROCESSING: gtk_label_set_text(a->status, c->detail); break;
    case VV_STATE_REVIEWING: gtk_label_set_text(a->status, "Reviewing — press the hotkey to say a change, Ctrl+Alt+Enter to paste, Ctrl+Alt+Esc to discard"); break;
    case VV_STATE_FAILED: gtk_label_set_text(a->status, c->detail); toast(a, c->detail); break;
    }
    g_free(idle); g_free(hk);
    gtk_button_set_label(a->record, c->state == VV_STATE_RECORDING ? "Stop" : "Record");
    gtk_widget_set_sensitive(GTK_WIDGET(a->record), c->state != VV_STATE_PROCESSING);
    hud_update(a);
    if (c->library_generation != a->last_generation) { a->last_generation = c->library_generation; reload_entries(a); }
}

static void on_record(GtkButton *b, gpointer data) {
    App *a = data;
    if (a->ctl->state == VV_STATE_RECORDING) vv_controller_finish(a->ctl); else vv_controller_start(a->ctl, NULL);
}

void vv_settings_open(App *a, GtkWindow *parent);
static void on_settings(GtkButton *b, gpointer data) { App *a = data; vv_settings_open(a, a->window); }

static void build_window(App *a) {
    GtkWidget *win = adw_application_window_new(GTK_APPLICATION(a->app));
    gtk_window_set_title(GTK_WINDOW(win), "VoiceVector");
    gtk_window_set_default_size(GTK_WINDOW(win), 640, 560);
    a->window = GTK_WINDOW(win);

    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    GtkWidget *settings = gtk_button_new_from_icon_name("emblem-system-symbolic");
    gtk_widget_set_tooltip_text(settings, "Settings");
    g_signal_connect(settings, "clicked", G_CALLBACK(on_settings), a);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), settings);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(content, 12); gtk_widget_set_margin_bottom(content, 12);
    gtk_widget_set_margin_start(content, 16); gtk_widget_set_margin_end(content, 16);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    a->status = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_wrap(a->status, TRUE); gtk_label_set_xalign(a->status, 0);
    gtk_widget_set_hexpand(GTK_WIDGET(a->status), TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(a->status), "dim-label");
    gtk_box_append(GTK_BOX(top), GTK_WIDGET(a->status));
    a->record = GTK_BUTTON(gtk_button_new_with_label("Record"));
    gtk_widget_add_css_class(GTK_WIDGET(a->record), "suggested-action");
    gtk_widget_add_css_class(GTK_WIDGET(a->record), "pill");
    gtk_widget_set_valign(GTK_WIDGET(a->record), GTK_ALIGN_CENTER);
    g_signal_connect(a->record, "clicked", G_CALLBACK(on_record), a);
    gtk_box_append(GTK_BOX(top), GTK_WIDGET(a->record));
    gtk_box_append(GTK_BOX(content), top);

    GtkWidget *folder_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(folder_row), gtk_label_new("Folder"));
    a->folders = GTK_DROP_DOWN(gtk_drop_down_new(NULL, NULL));
    g_signal_connect(a->folders, "notify::selected", G_CALLBACK(on_folder_changed), a);
    gtk_box_append(GTK_BOX(folder_row), GTK_WIDGET(a->folders));
    gtk_box_append(GTK_BOX(content), folder_row);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    a->entries = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(a->entries, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(a->entries), "boxed-list");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(a->entries));
    gtk_box_append(GTK_BOX(content), scroll);

    a->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(a->toasts, content);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), GTK_WIDGET(a->toasts));
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), toolbar);
}

static void load_css(void) {
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        ".vv-hud { background: transparent; }"
        ".vv-pill { background: alpha(#201e2a, 0.92); border: 1px solid alpha(#7359f2, 0.35); border-radius: 24px; padding: 12px 18px; color: #f0eef8; }"
        ".vv-card { background: alpha(#201e2a, 0.94); border: 1px solid alpha(#7359f2, 0.35); border-radius: 14px; padding: 14px; color: #f0eef8; }"
        ".vv-bar { background: alpha(#7359f2, 0.85); border-radius: 2px; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/* ---------------------------------------------- multi-machine glue */

static void on_peer_deliver(const char *text, guint32 window, bool submit,
                            void (*done)(bool ok, const char *error, gpointer token),
                            gpointer token, gpointer user) {
    App *a = user;
    (void)window;   /* Wayland: we can only paste into the current focus */
    vv_controller_receive_routed(a->ctl, text, submit, done, token);
}

typedef struct { void (*answer)(bool, gpointer); gpointer token; } IncomingPair;

static void on_incoming_pair_response(AdwAlertDialog *d, const char *response, gpointer data) {
    IncomingPair *ip = data;
    ip->answer(g_strcmp0(response, "pair") == 0, ip->token);
    g_free(ip);
}

static void on_peer_pair(const char *name, const char *code,
                         void (*answer)(bool accepted, gpointer token),
                         gpointer token, gpointer user) {
    App *a = user;
    char *title = g_strdup_printf("Pair with \u201c%s\u201d?", name);
    char *body = g_strdup_printf("Confirm only if the other machine shows this code:\n\n%.3s %s", code, code + 3);
    AdwAlertDialog *d = ADW_ALERT_DIALOG(adw_alert_dialog_new(title, body));
    g_free(title); g_free(body);
    adw_alert_dialog_add_responses(d, "cancel", "Cancel", "pair", "They match \u2014 pair", NULL);
    adw_alert_dialog_set_response_appearance(d, "pair", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_close_response(d, "cancel");
    IncomingPair *ip = g_new0(IncomingPair, 1);
    ip->answer = answer; ip->token = token;
    g_signal_connect(d, "response", G_CALLBACK(on_incoming_pair_response), ip);
    if (a->window) gtk_window_present(a->window);
    adw_dialog_present(ADW_DIALOG(d), a->window ? GTK_WIDGET(a->window) : NULL);
}

static void on_activate(GtkApplication *gapp, gpointer data) {
    App *a = data;
    if (a->window) { gtk_window_present(a->window); return; }
    load_css();
    build_window(a);
    hud_build(a);
    vv_controller_set_observer(a->ctl, on_state, a);
    vv_peer_service_init(a->ctl->config, on_peer_deliver, on_peer_pair, a);
    vv_peer_service_apply();
    reload_folders(a);
    reload_entries(a);
    on_state(a->ctl, a);
    gtk_window_present(a->window);
    if (!a->ctl->config->wizard_completed) {
        toast(a, "Welcome — add a transcription provider in Settings to start dictating.");
        a->ctl->config->wizard_completed = true;
        vv_controller_save_config(a->ctl);
        vv_settings_open(a, a->window);
    }
}

int vv_app_run(int argc, char **argv) {
    App *a = g_new0(App, 1);
    app_singleton = a;
    char *path = vv_config_default_path();
    VvConfig *config = vv_config_load(path);
    vv_config_save(config, path);   /* materialize config.json */
    a->app = adw_application_new("io.sammons.voicevector", G_APPLICATION_DEFAULT_FLAGS);
    a->ctl = vv_controller_new(config, path);
    g_signal_connect(a->app, "activate", G_CALLBACK(on_activate), a);
    int status = g_application_run(G_APPLICATION(a->app), argc, argv);
    vv_controller_free(a->ctl);
    vv_config_free(config);
    g_free(path);
    return status;
}

/* Exposed for settings.c */
VvController *vv_app_controller(void) { return app_singleton->ctl; }
void vv_app_refresh(void) { App *a = app_singleton; reload_folders(a); reload_entries(a); on_state(a->ctl, a); }
void vv_app_toast(const char *text) { toast(app_singleton, text); }
