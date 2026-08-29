#include "platform/services.h"
#include "core/log.h"
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <libsecret/secret.h>
#include <linux/input-event-codes.h>
#include <string.h>

static const char *PORTAL_BUS = "org.freedesktop.portal.Desktop";
static const char *PORTAL_PATH = "/org/freedesktop/portal/desktop";

/* ------------------------------------------------ portal plumbing */

typedef struct { GMainLoop *loop; GVariant *results; guint response; } Pending;

static void on_response(GDBusConnection *bus, const char *sender, const char *path, const char *iface,
                        const char *signal, GVariant *params, gpointer data) {
    Pending *p = data;
    GVariant *results = NULL;
    g_variant_get(params, "(u@a{sv})", &p->response, &results);
    p->results = results;
    g_main_loop_quit(p->loop);
}

/* Calls a portal method whose last argument is the options vardict; injects
 * handle_token and waits for the Response. */
static GVariant *portal_request(GDBusConnection *bus, const char *iface, const char *method, GVariant *args, char **error) {
    char *token = g_strdup_printf("vv%u", (unsigned)g_random_int());
    char *sender = g_strdelimit(g_strdup(g_dbus_connection_get_unique_name(bus) + 1), ".", '_');
    char *request_path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
    Pending p = { g_main_loop_new(NULL, FALSE), NULL, 2 };
    guint sub = g_dbus_connection_signal_subscribe(bus, PORTAL_BUS, "org.freedesktop.portal.Request", "Response", request_path, NULL,
                                                   G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE, on_response, &p, NULL);
    GVariantBuilder ob; g_variant_builder_init(&ob, G_VARIANT_TYPE_VARDICT);
    gsize n = g_variant_n_children(args);
    GVariant *opts = g_variant_get_child_value(args, n - 1);
    GVariantIter it; const char *k; GVariant *v;
    g_variant_iter_init(&it, opts);
    while (g_variant_iter_next(&it, "{&sv}", &k, &v)) { g_variant_builder_add(&ob, "{sv}", k, v); g_variant_unref(v); }
    g_variant_builder_add(&ob, "{sv}", "handle_token", g_variant_new_string(token));
    g_variant_unref(opts);
    GVariantBuilder ab; g_variant_builder_init(&ab, G_VARIANT_TYPE_TUPLE);
    for (gsize i = 0; i + 1 < n; i++) { GVariant *c = g_variant_get_child_value(args, i); g_variant_builder_add_value(&ab, c); g_variant_unref(c); }
    g_variant_builder_add_value(&ab, g_variant_builder_end(&ob));
    GError *err = NULL;
    GVariant *ret = g_dbus_connection_call_sync(bus, PORTAL_BUS, PORTAL_PATH, iface, method, g_variant_builder_end(&ab), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
    g_variant_unref(args);
    GVariant *results = NULL;
    if (!ret) { *error = g_strdup(err->message); g_error_free(err); }
    else {
        g_variant_unref(ret);
        guint timeout = g_timeout_add_seconds(180, (GSourceFunc)g_main_loop_quit, p.loop);
        g_main_loop_run(p.loop);
        g_source_remove(timeout);
        if (p.response == 0) results = p.results;
        else { *error = g_strdup_printf("portal request %s", p.response == 1 ? "cancelled" : "failed"); if (p.results) g_variant_unref(p.results); }
    }
    g_dbus_connection_signal_unsubscribe(bus, sub);
    g_main_loop_unref(p.loop);
    g_free(request_path); g_free(sender); g_free(token);
    return results;
}

/* ---------------------------------------------------------- paste */

static GDBusConnection *bus;
static char *rd_session;         /* RemoteDesktop session handle, kept open */
static char *restore_token_path(void) { return g_build_filename(g_get_user_config_dir(), "voicevector", "remote-desktop.token", NULL); }

bool vv_paste_portal_available(void) {
    if (!bus) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus) return false;
    GVariant *ret = g_dbus_connection_call_sync(bus, PORTAL_BUS, PORTAL_PATH, "org.freedesktop.DBus.Properties", "Get",
                                                g_variant_new("(ss)", "org.freedesktop.portal.RemoteDesktop", "version"), NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    if (!ret) return false;
    g_variant_unref(ret);
    return true;
}

static bool remote_desktop_session(char **error) {
    if (rd_session) return true;
    if (!bus) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus) { *error = g_strdup("no session bus"); return false; }
    const char *iface = "org.freedesktop.portal.RemoteDesktop";
    GVariantBuilder ob; g_variant_builder_init(&ob, G_VARIANT_TYPE_VARDICT);
    char *tok = g_strdup_printf("vvs%u", (unsigned)g_random_int());
    g_variant_builder_add(&ob, "{sv}", "session_handle_token", g_variant_new_string(tok));
    g_free(tok);
    GVariant *r = portal_request(bus, iface, "CreateSession", g_variant_new("(a{sv})", &ob), error);
    if (!r) return false;
    const char *handle = NULL;
    g_variant_lookup(r, "session_handle", "&s", &handle);
    char *session = g_strdup(handle);
    g_variant_unref(r);
    if (!session) { *error = g_strdup("no session handle"); return false; }

    GVariantBuilder sb; g_variant_builder_init(&sb, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&sb, "{sv}", "types", g_variant_new_uint32(1));          /* KEYBOARD */
    g_variant_builder_add(&sb, "{sv}", "persist_mode", g_variant_new_uint32(2));   /* until revoked */
    char *token_path = restore_token_path();
    char *saved = NULL;
    if (g_file_get_contents(token_path, &saved, NULL, NULL) && *g_strstrip(saved))
        g_variant_builder_add(&sb, "{sv}", "restore_token", g_variant_new_string(saved));
    g_free(saved);
    r = portal_request(bus, iface, "SelectDevices", g_variant_new("(oa{sv})", session, &sb), error);
    if (!r) { g_free(session); g_free(token_path); return false; }
    g_variant_unref(r);

    GVariantBuilder stb; g_variant_builder_init(&stb, G_VARIANT_TYPE_VARDICT);
    r = portal_request(bus, iface, "Start", g_variant_new("(osa{sv})", session, "", &stb), error);
    if (!r) { g_free(session); g_free(token_path); return false; }
    const char *restore = NULL;
    if (g_variant_lookup(r, "restore_token", "&s", &restore) && restore) {
        char *dir = g_path_get_dirname(token_path);
        g_mkdir_with_parents(dir, 0700); g_free(dir);
        g_file_set_contents(token_path, restore, -1, NULL);
    }
    g_variant_unref(r);
    g_free(token_path);
    rd_session = session;
    return true;
}

static bool notify_key(int keycode, bool down) {
    GVariantBuilder ob; g_variant_builder_init(&ob, G_VARIANT_TYPE_VARDICT);
    GError *err = NULL;
    GVariant *ret = g_dbus_connection_call_sync(bus, PORTAL_BUS, PORTAL_PATH, "org.freedesktop.portal.RemoteDesktop", "NotifyKeyboardKeycode",
                                                g_variant_new("(oa{sv}iu)", rd_session, &ob, keycode, down ? 1u : 0u), NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err);
    if (!ret) { vv_log_error("NotifyKeyboardKeycode failed: %s", err->message); g_error_free(err); g_free(rd_session); rd_session = NULL; return false; }
    g_variant_unref(ret);
    return true;
}

static gboolean pump(gpointer data) { g_main_loop_quit(data); return G_SOURCE_REMOVE; }
static void sleep_ms(int ms) { GMainLoop *l = g_main_loop_new(NULL, FALSE); g_timeout_add(ms, pump, l); g_main_loop_run(l); g_main_loop_unref(l); }

VvPasteOutcome vv_paste_insert(const char *text, bool auto_paste, char **reason) {
    *reason = NULL;
    GdkDisplay *display = gdk_display_get_default();
    if (display) gdk_clipboard_set_text(gdk_display_get_clipboard(display), text);
    if (!auto_paste) { *reason = g_strdup("Auto-paste is off"); return VV_PASTE_COPIED_ONLY; }
    sleep_ms(60);   /* let the clipboard offer settle before the target reads it */
    char *error = NULL;
    if (!remote_desktop_session(&error)) {
        *reason = g_strdup_printf("Paste permission not available (%s)", error ? error : "portal");
        g_free(error);
        return VV_PASTE_COPIED_ONLY;
    }
    bool ok = notify_key(KEY_LEFTCTRL, true) && notify_key(KEY_V, true) && notify_key(KEY_V, false) && notify_key(KEY_LEFTCTRL, false);
    if (!ok) { *reason = g_strdup("Key injection failed"); return VV_PASTE_COPIED_ONLY; }
    return VV_PASTE_PASTED;
}

/* -------------------------------------------------------- secrets */

static const SecretSchema *schema(void) {
    static const SecretSchema s = {
        "io.sammons.voicevector", SECRET_SCHEMA_NONE,
        { { "provider", SECRET_SCHEMA_ATTRIBUTE_STRING }, { NULL, 0 } },
    };
    return &s;
}

char *vv_secret_get(const char *provider_id) {
    GError *err = NULL;
    char *value = secret_password_lookup_sync(schema(), NULL, &err, "provider", provider_id, NULL);
    if (err) { vv_log_error("Secret lookup failed: %s", err->message); g_error_free(err); return NULL; }
    if (value && !*value) { secret_password_free(value); return NULL; }
    char *copy = value ? g_strdup(value) : NULL;
    secret_password_free(value);
    return copy;
}

bool vv_secret_set(const char *provider_id, const char *api_key) {
    GError *err = NULL;
    char *label = g_strdup_printf("VoiceVector provider %s", provider_id);
    bool ok = secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT, label, api_key, NULL, &err, "provider", provider_id, NULL);
    g_free(label);
    if (!ok) { vv_log_error("Secret store failed: %s", err ? err->message : "?"); if (err) g_error_free(err); }
    return ok;
}

void vv_secret_delete(const char *provider_id) {
    secret_password_clear_sync(schema(), NULL, NULL, "provider", provider_id, NULL);
}

/* ---------------------------------------------------- screenshot */

GBytes *vv_screenshot_jpeg(void) {
    if (!bus) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus) return NULL;
    GVariantBuilder ob; g_variant_builder_init(&ob, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&ob, "{sv}", "interactive", g_variant_new_boolean(FALSE));
    char *error = NULL;
    GVariant *r = portal_request(bus, "org.freedesktop.portal.Screenshot", "Screenshot", g_variant_new("(sa{sv})", "", &ob), &error);
    if (!r) { vv_log_error("Screenshot portal: %s", error); g_free(error); return NULL; }
    const char *uri = NULL;
    g_variant_lookup(r, "uri", "&s", &uri);
    GBytes *jpeg = NULL;
    if (uri) {
        GFile *file = g_file_new_for_uri(uri);
        char *path = g_file_get_path(file);
        GdkPixbuf *pix = path ? gdk_pixbuf_new_from_file(path, NULL) : NULL;
        if (pix) {
            int w = gdk_pixbuf_get_width(pix), h = gdk_pixbuf_get_height(pix);
            if (w > 1280) { GdkPixbuf *s = gdk_pixbuf_scale_simple(pix, 1280, h * 1280 / w, GDK_INTERP_BILINEAR); g_object_unref(pix); pix = s; }
            gchar *buf = NULL; gsize n = 0;
            if (gdk_pixbuf_save_to_buffer(pix, &buf, &n, "jpeg", NULL, "quality", "60", NULL)) jpeg = g_bytes_new_take(buf, n);
            g_object_unref(pix);
        }
        if (path) { g_unlink(path); g_free(path); }
        g_object_unref(file);
    }
    g_variant_unref(r);
    return jpeg;
}
