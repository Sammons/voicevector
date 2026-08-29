#include "platform/hotkey.h"
#include "core/log.h"
#include <gio/gio.h>
#include <glib-unix.h>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>

#define MOD_CTRL 1
#define MOD_ALT 2
#define MOD_SHIFT 4
#define MOD_SUPER 8

typedef struct {
    char *profile_id;
    VvHotkey spec;
    bool down;
    char *portal_id;   /* "profile-<uuid>" */
} Binding;

struct VvHotkeyEngine {
    VvHotkeyActionFn on_action;
    VvHotkeyControlFn on_control;
    gpointer user;
    GPtrArray *bindings;          /* Binding* (deduped, first wins) */
    VvTap machine;
    VvTapStartMode start_mode;
    char *active_profile_id;
    guint expiry_source;
    bool recording_active, review_active;
    VvHotkeyBackend requested;
    /* raw */
    GPtrArray *fds;               /* GIOChannel-backed watches on /dev/input/event* */
    GPtrArray *watch_ids;
    int mods_down;                /* live modifier mask from evdev */
    VvHotkeyCaptureFn capture_fn; gpointer capture_user;
    /* portal */
    GDBusConnection *bus;
    char *session_handle;
    guint portal_signal_id;
    bool portal_bound;
    char *status;
    const char *backend_name;
};

/* ------------------------------------------------------- key names */

typedef struct { int code; const char *name; const char *xkb; } KeyName;
static const KeyName KEYS[] = {
    { KEY_RIGHTALT, "Right Alt", "Alt_R" }, { KEY_LEFTALT, "Left Alt", "Alt_L" },
    { KEY_RIGHTCTRL, "Right Ctrl", "Control_R" }, { KEY_LEFTCTRL, "Left Ctrl", "Control_L" },
    { KEY_RIGHTSHIFT, "Right Shift", "Shift_R" }, { KEY_LEFTSHIFT, "Left Shift", "Shift_L" },
    { KEY_RIGHTMETA, "Right Super", "Super_R" }, { KEY_LEFTMETA, "Left Super", "Super_L" },
    { KEY_CAPSLOCK, "CapsLock", "Caps_Lock" }, { KEY_SPACE, "Space", "space" }, { KEY_ENTER, "Enter", "Return" },
    { KEY_TAB, "Tab", "Tab" }, { KEY_ESC, "Esc", "Escape" }, { KEY_BACKSPACE, "Backspace", "BackSpace" },
    { KEY_GRAVE, "`", "grave" }, { KEY_MINUS, "-", "minus" }, { KEY_EQUAL, "=", "equal" },
    { KEY_F1, "F1", "F1" }, { KEY_F2, "F2", "F2" }, { KEY_F3, "F3", "F3" }, { KEY_F4, "F4", "F4" },
    { KEY_F5, "F5", "F5" }, { KEY_F6, "F6", "F6" }, { KEY_F7, "F7", "F7" }, { KEY_F8, "F8", "F8" },
    { KEY_F9, "F9", "F9" }, { KEY_F10, "F10", "F10" }, { KEY_F11, "F11", "F11" }, { KEY_F12, "F12", "F12" },
    { KEY_F13, "F13", "F13" }, { KEY_F14, "F14", "F14" }, { KEY_F15, "F15", "F15" }, { KEY_F16, "F16", "F16" },
    { KEY_F17, "F17", "F17" }, { KEY_F18, "F18", "F18" }, { KEY_F19, "F19", "F19" }, { KEY_F20, "F20", "F20" },
    { KEY_PAUSE, "Pause", "Pause" }, { KEY_SCROLLLOCK, "ScrollLock", "Scroll_Lock" }, { KEY_INSERT, "Insert", "Insert" },
    { KEY_HOME, "Home", "Home" }, { KEY_END, "End", "End" }, { KEY_PAGEUP, "PageUp", "Prior" }, { KEY_PAGEDOWN, "PageDown", "Next" },
    { KEY_DELETE, "Delete", "Delete" }, { KEY_RIGHT, "Right", "Right" }, { KEY_LEFT, "Left", "Left" }, { KEY_UP, "Up", "Up" }, { KEY_DOWN, "Down", "Down" },
};
static const char LETTERS[] = "qwertyuiopasdfghjklzxcvbnm";
static const int LETTER_CODES[] = { KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P, KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M };
static const int DIGIT_CODES[] = { KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9 };

static const KeyName *lookup(int code) {
    for (size_t i = 0; i < G_N_ELEMENTS(KEYS); i++) if (KEYS[i].code == code) return &KEYS[i];
    return NULL;
}

const char *vv_keycode_name(int code) {
    static char buf[16];
    const KeyName *k = lookup(code);
    if (k) return k->name;
    for (int i = 0; i < 26; i++) if (LETTER_CODES[i] == code) { buf[0] = g_ascii_toupper(LETTERS[i]); buf[1] = 0; return buf; }
    for (int i = 0; i < 10; i++) if (DIGIT_CODES[i] == code) { buf[0] = '0' + i; buf[1] = 0; return buf; }
    g_snprintf(buf, sizeof buf, "key %d", code);
    return buf;
}

static const char *xkb_name(int code, char *buf, size_t n) {
    const KeyName *k = lookup(code);
    if (k) return k->xkb;
    for (int i = 0; i < 26; i++) if (LETTER_CODES[i] == code) { buf[0] = LETTERS[i]; buf[1] = 0; return buf; }
    for (int i = 0; i < 10; i++) if (DIGIT_CODES[i] == code) { buf[0] = '0' + i; buf[1] = 0; return buf; }
    return NULL;
}

char *vv_hotkey_describe(const VvHotkey *h) {
    GString *s = g_string_new(NULL);
    if (!h->is_modifier_only) {
        if (h->modifiers & MOD_CTRL) g_string_append(s, "Ctrl+");
        if (h->modifiers & MOD_ALT) g_string_append(s, "Alt+");
        if (h->modifiers & MOD_SHIFT) g_string_append(s, "Shift+");
        if (h->modifiers & MOD_SUPER) g_string_append(s, "Super+");
    }
    g_string_append(s, vv_keycode_name(h->key_code));
    return g_string_free(s, FALSE);
}

char *vv_hotkey_portal_trigger(const VvHotkey *h) {
    if (h->is_modifier_only) return NULL;
    char buf[8];
    const char *key = xkb_name(h->key_code, buf, sizeof buf);
    if (!key) return NULL;
    GString *s = g_string_new(NULL);
    if (h->modifiers & MOD_CTRL) g_string_append(s, "<Control>");
    if (h->modifiers & MOD_ALT) g_string_append(s, "<Alt>");
    if (h->modifiers & MOD_SHIFT) g_string_append(s, "<Shift>");
    if (h->modifiers & MOD_SUPER) g_string_append(s, "<Super>");
    g_string_append(s, key);
    return g_string_free(s, FALSE);
}

static bool is_modifier_code(int code) {
    return code == KEY_LEFTALT || code == KEY_RIGHTALT || code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL
        || code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT || code == KEY_LEFTMETA || code == KEY_RIGHTMETA || code == KEY_CAPSLOCK;
}

/* ------------------------------------------------------ gestures */

static double now_seconds(void) { return g_get_monotonic_time() / 1e6; }

static void emit_actions(VvHotkeyEngine *e, int n, VvTapAct act);

static gboolean on_expiry(gpointer data) {
    VvHotkeyEngine *e = data;
    e->expiry_source = 0;
    VvTapAct act;
    int n = vv_tap_expire(&e->machine, now_seconds(), &act);
    emit_actions(e, n, act);
    return G_SOURCE_REMOVE;
}

static void schedule_expiry(VvHotkeyEngine *e) {
    if (e->expiry_source) { g_source_remove(e->expiry_source); e->expiry_source = 0; }
    double deadline = vv_tap_pending_deadline(&e->machine);
    if (deadline < 0) return;
    double delay = deadline - now_seconds();
    if (delay < 0.01) delay = 0.01;
    e->expiry_source = g_timeout_add((guint)(delay * 1000), on_expiry, e);
}

static void emit_actions(VvHotkeyEngine *e, int n, VvTapAct act) {
    schedule_expiry(e);
    if (n <= 0) return;
    VvHotkeyAction a = act == VV_ACT_START_RECORDING ? VV_HK_ACTION_START : act == VV_ACT_COMMIT ? VV_HK_ACTION_COMMIT : VV_HK_ACTION_DISCARD;
    e->on_action(a, e->active_profile_id, e->user);
}

static void gesture(VvHotkeyEngine *e, Binding *b, bool down) {
    if (vv_tap_is_active(&e->machine) && g_strcmp0(e->active_profile_id, b->profile_id) != 0) return;
    if (down) {
        if (b->down) return;
        b->down = true;
        if (!vv_tap_is_active(&e->machine)) { g_free(e->active_profile_id); e->active_profile_id = g_strdup(b->profile_id); }
        VvTapAct act; int n = vv_tap_key_down(&e->machine, now_seconds(), &act);
        emit_actions(e, n, act);
    } else {
        b->down = false;
        VvTapAct act; int n = vv_tap_key_up(&e->machine, now_seconds(), &act);
        emit_actions(e, n, act);
    }
}

/* ---------------------------------------------------------- raw */

bool vv_raw_input_available(void) {
    GDir *dir = g_dir_open("/dev/input", 0, NULL);
    if (!dir) return false;
    const char *name; bool ok = false;
    while (!ok && (name = g_dir_read_name(dir))) {
        if (!g_str_has_prefix(name, "event")) continue;
        char *path = g_build_filename("/dev/input", name, NULL);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) { ok = true; close(fd); }
        g_free(path);
    }
    g_dir_close(dir);
    return ok;
}

static bool is_keyboard(int fd) {
    unsigned long evbits[(EV_MAX + 1) / (8 * sizeof(unsigned long)) + 1] = {0};
    unsigned long keybits[(KEY_MAX + 1) / (8 * sizeof(unsigned long)) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits) < 0) return false;
    if (!(evbits[EV_KEY / (8 * sizeof(unsigned long))] & (1UL << (EV_KEY % (8 * sizeof(unsigned long)))))) return false;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits) < 0) return false;
    #define HAS(code) (keybits[(code) / (8 * sizeof(unsigned long))] & (1UL << ((code) % (8 * sizeof(unsigned long)))))
    return HAS(KEY_A) && HAS(KEY_SPACE);
    #undef HAS
}

static int mod_bit(int code) {
    switch (code) {
    case KEY_LEFTCTRL: case KEY_RIGHTCTRL: return MOD_CTRL;
    case KEY_LEFTALT: case KEY_RIGHTALT: return MOD_ALT;
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: return MOD_SHIFT;
    case KEY_LEFTMETA: case KEY_RIGHTMETA: return MOD_SUPER;
    default: return 0;
    }
}

static void raw_key(VvHotkeyEngine *e, int code, bool down) {
    int mb = mod_bit(code);
    if (mb) { if (down) e->mods_down |= mb; else e->mods_down &= ~mb; }

    if (e->capture_fn && down) {
        VvHotkey spec = { code, is_modifier_code(code) ? 0 : e->mods_down, is_modifier_code(code) };
        VvHotkeyCaptureFn fn = e->capture_fn; gpointer u = e->capture_user;
        e->capture_fn = NULL;
        fn(&spec, u);
        return;
    }

    /* Control combos: Ctrl+Alt+Enter accept / Ctrl+Alt+Esc discard or cancel. */
    if (down && (e->mods_down & (MOD_CTRL | MOD_ALT)) == (MOD_CTRL | MOD_ALT)) {
        if (code == KEY_ENTER || code == KEY_KPENTER) { if (e->review_active) e->on_control(VV_HK_REVIEW_ACCEPT, e->user); return; }
        if (code == KEY_ESC) {
            if (e->recording_active) { VvTapAct act; int n = vv_tap_cancel(&e->machine, &act); emit_actions(e, n, act); }
            else if (e->review_active) e->on_control(VV_HK_REVIEW_DISCARD, e->user);
            return;
        }
    }

    for (guint i = 0; i < e->bindings->len; i++) {
        Binding *b = g_ptr_array_index(e->bindings, i);
        if (b->spec.key_code != code) continue;
        if (!b->spec.is_modifier_only && down && !vv_tap_is_active(&e->machine) && (e->mods_down & ~mb) != b->spec.modifiers) continue;
        gesture(e, b, down);
        return;
    }
}

static gboolean on_raw_readable(GIOChannel *ch, GIOCondition cond, gpointer data) {
    VvHotkeyEngine *e = data;
    if (cond & (G_IO_ERR | G_IO_HUP)) return G_SOURCE_REMOVE;
    int fd = g_io_channel_unix_get_fd(ch);
    struct input_event ev[32];
    ssize_t n;
    while ((n = read(fd, ev, sizeof ev)) > 0) {
        for (size_t i = 0; i < (size_t)n / sizeof ev[0]; i++) {
            if (ev[i].type != EV_KEY || ev[i].value == 2) continue;   /* ignore auto-repeat */
            raw_key(e, ev[i].code, ev[i].value == 1);
        }
    }
    return G_SOURCE_CONTINUE;
}

static void raw_stop(VvHotkeyEngine *e) {
    for (guint i = 0; i < e->watch_ids->len; i++) g_source_remove(GPOINTER_TO_UINT(g_ptr_array_index(e->watch_ids, i)));
    g_ptr_array_set_size(e->watch_ids, 0);
    g_ptr_array_set_size(e->fds, 0);
}

static bool raw_start(VvHotkeyEngine *e) {
    raw_stop(e);
    GDir *dir = g_dir_open("/dev/input", 0, NULL);
    if (!dir) return false;
    const char *name; int opened = 0;
    while ((name = g_dir_read_name(dir))) {
        if (!g_str_has_prefix(name, "event")) continue;
        char *path = g_build_filename("/dev/input", name, NULL);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        g_free(path);
        if (fd < 0) continue;
        if (!is_keyboard(fd)) { close(fd); continue; }
        GIOChannel *ch = g_io_channel_unix_new(fd);
        g_io_channel_set_close_on_unref(ch, TRUE);
        g_io_channel_set_encoding(ch, NULL, NULL);
        guint id = g_io_add_watch(ch, G_IO_IN | G_IO_ERR | G_IO_HUP, on_raw_readable, e);
        g_ptr_array_add(e->fds, ch);
        g_ptr_array_add(e->watch_ids, GUINT_TO_POINTER(id));
        opened++;
    }
    g_dir_close(dir);
    return opened > 0;
}

/* -------------------------------------------------------- portal */

static const char *PORTAL_BUS = "org.freedesktop.portal.Desktop";
static const char *PORTAL_PATH = "/org/freedesktop/portal/desktop";
static const char *SHORTCUTS_IFACE = "org.freedesktop.portal.GlobalShortcuts";

static Binding *binding_for_portal_id(VvHotkeyEngine *e, const char *id) {
    for (guint i = 0; i < e->bindings->len; i++) {
        Binding *b = g_ptr_array_index(e->bindings, i);
        if (g_strcmp0(b->portal_id, id) == 0) return b;
    }
    return NULL;
}

static void on_portal_signal(GDBusConnection *bus, const char *sender, const char *path, const char *iface,
                             const char *signal, GVariant *params, gpointer data) {
    VvHotkeyEngine *e = data;
    const char *session = NULL, *shortcut_id = NULL;
    guint64 timestamp = 0;
    GVariant *options = NULL;
    g_variant_get(params, "(&o&st@a{sv})", &session, &shortcut_id, &timestamp, &options);
    if (options) g_variant_unref(options);
    if (g_strcmp0(session, e->session_handle) != 0) return;
    bool down = strcmp(signal, "Activated") == 0;
    if (!down && strcmp(signal, "Deactivated") != 0) return;
    if (strcmp(shortcut_id, "review-accept") == 0) { if (down && e->review_active) e->on_control(VV_HK_REVIEW_ACCEPT, e->user); return; }
    if (strcmp(shortcut_id, "cancel") == 0) {
        if (!down) return;
        if (e->recording_active) { VvTapAct act; int n = vv_tap_cancel(&e->machine, &act); emit_actions(e, n, act); }
        else if (e->review_active) e->on_control(VV_HK_REVIEW_DISCARD, e->user);
        return;
    }
    Binding *b = binding_for_portal_id(e, shortcut_id);
    if (b) gesture(e, b, down);
}

static char *unique_token(void) { return g_strdup_printf("vv%u", (unsigned)g_random_int()); }

/* Blocking request helper: calls `method`, waits for the Response signal on
 * the request object. Portal calls are quick; a nested loop keeps this simple. */
typedef struct { GMainLoop *loop; GVariant *results; guint response; } Pending;

static void on_response(GDBusConnection *bus, const char *sender, const char *path, const char *iface,
                        const char *signal, GVariant *params, gpointer data) {
    Pending *p = data;
    GVariant *results = NULL;
    g_variant_get(params, "(u@a{sv})", &p->response, &results);
    p->results = results;
    g_main_loop_quit(p->loop);
}

static GVariant *portal_call(GDBusConnection *bus, const char *iface, const char *method, GVariant *args, guint *response, char **error) {
    char *token = unique_token();
    const char *unique = g_dbus_connection_get_unique_name(bus);
    char *sender = g_strdelimit(g_strdup(unique + 1), ".", '_');
    char *request_path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
    Pending p = { g_main_loop_new(NULL, FALSE), NULL, 2 };
    guint sub = g_dbus_connection_signal_subscribe(bus, PORTAL_BUS, "org.freedesktop.portal.Request", "Response", request_path, NULL,
                                                   G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE, on_response, &p, NULL);
    /* Inject handle_token into the options dict (last argument). */
    GVariantBuilder ob; g_variant_builder_init(&ob, G_VARIANT_TYPE_VARDICT);
    GVariant *opts = g_variant_get_child_value(args, g_variant_n_children(args) - 1);
    GVariantIter it; const char *k; GVariant *v;
    g_variant_iter_init(&it, opts);
    while (g_variant_iter_next(&it, "{&sv}", &k, &v)) { g_variant_builder_add(&ob, "{sv}", k, v); g_variant_unref(v); }
    g_variant_builder_add(&ob, "{sv}", "handle_token", g_variant_new_string(token));
    g_variant_unref(opts);
    GVariantBuilder ab; g_variant_builder_init(&ab, G_VARIANT_TYPE_TUPLE);
    for (gsize i = 0; i + 1 < g_variant_n_children(args); i++) { GVariant *c = g_variant_get_child_value(args, i); g_variant_builder_add_value(&ab, c); g_variant_unref(c); }
    g_variant_builder_add_value(&ab, g_variant_builder_end(&ob));
    GError *err = NULL;
    GVariant *ret = g_dbus_connection_call_sync(bus, PORTAL_BUS, PORTAL_PATH, iface, method, g_variant_builder_end(&ab), NULL,
                                                G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
    g_variant_unref(args);
    if (!ret) { *error = g_strdup(err->message); g_error_free(err); g_dbus_connection_signal_unsubscribe(bus, sub); g_main_loop_unref(p.loop); g_free(request_path); g_free(sender); g_free(token); return NULL; }
    g_variant_unref(ret);
    guint timeout = g_timeout_add_seconds(120, (GSourceFunc)g_main_loop_quit, p.loop);
    g_main_loop_run(p.loop);
    g_source_remove(timeout);
    g_dbus_connection_signal_unsubscribe(bus, sub);
    g_main_loop_unref(p.loop);
    g_free(request_path); g_free(sender); g_free(token);
    *response = p.response;
    if (p.response != 0) { *error = g_strdup_printf("portal request %s (code %u)", p.response == 1 ? "cancelled" : "failed", p.response); if (p.results) g_variant_unref(p.results); return NULL; }
    return p.results;
}

static void portal_stop(VvHotkeyEngine *e) {
    if (e->portal_signal_id && e->bus) g_dbus_connection_signal_unsubscribe(e->bus, e->portal_signal_id);
    e->portal_signal_id = 0;
    if (e->session_handle && e->bus)
        g_dbus_connection_call(e->bus, PORTAL_BUS, e->session_handle, "org.freedesktop.portal.Session", "Close", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    g_free(e->session_handle); e->session_handle = NULL;
    e->portal_bound = false;
}

static bool portal_start(VvHotkeyEngine *e, char **error) {
    portal_stop(e);
    if (!e->bus) { e->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL); if (!e->bus) { *error = g_strdup("no session bus"); return false; } }

    /* Portal ≥ 1.21 wants an app id for non-sandboxed apps. */
    g_dbus_connection_call_sync(e->bus, PORTAL_BUS, PORTAL_PATH, "org.freedesktop.host.portal.Registry", "Register",
                                g_variant_new("(sa{sv})", "io.sammons.voicevector", NULL), NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);

    char *session_token = unique_token();
    GVariantBuilder ob; g_variant_builder_init(&ob, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&ob, "{sv}", "session_handle_token", g_variant_new_string(session_token));
    guint response;
    GVariant *results = portal_call(e->bus, SHORTCUTS_IFACE, "CreateSession", g_variant_new("(a{sv})", &ob), &response, error);
    g_free(session_token);
    if (!results) return false;
    const char *handle = NULL;
    g_variant_lookup(results, "session_handle", "&s", &handle);
    e->session_handle = g_strdup(handle);
    g_variant_unref(results);
    if (!e->session_handle) { *error = g_strdup("no session handle"); return false; }

    /* Bind: every profile with a portal-expressible trigger + control combos. */
    GVariantBuilder sb; g_variant_builder_init(&sb, G_VARIANT_TYPE("a(sa{sv})"));
    int bound = 0;
    for (guint i = 0; i < e->bindings->len; i++) {
        Binding *b = g_ptr_array_index(e->bindings, i);
        char *trigger = vv_hotkey_portal_trigger(&b->spec);
        if (!trigger) continue;
        GVariantBuilder pb; g_variant_builder_init(&pb, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&pb, "{sv}", "description", g_variant_new_string("Dictate (hold or tap)"));
        g_variant_builder_add(&pb, "{sv}", "preferred_trigger", g_variant_new_string(trigger));
        g_variant_builder_add(&sb, "(sa{sv})", b->portal_id, &pb);
        g_free(trigger);
        bound++;
    }
    const char *controls[][3] = { { "review-accept", "Paste the reviewed text", "<Control><Alt>Return" }, { "cancel", "Cancel recording / discard review", "<Control><Alt>Escape" } };
    for (int i = 0; i < 2; i++) {
        GVariantBuilder pb; g_variant_builder_init(&pb, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&pb, "{sv}", "description", g_variant_new_string(controls[i][1]));
        g_variant_builder_add(&pb, "{sv}", "preferred_trigger", g_variant_new_string(controls[i][2]));
        g_variant_builder_add(&sb, "(sa{sv})", controls[i][0], &pb);
    }
    GVariantBuilder bo; g_variant_builder_init(&bo, G_VARIANT_TYPE_VARDICT);
    results = portal_call(e->bus, SHORTCUTS_IFACE, "BindShortcuts",
                          g_variant_new("(oa(sa{sv})sa{sv})", e->session_handle, &sb, "", &bo), &response, error);
    if (!results) return false;
    g_variant_unref(results);
    e->portal_signal_id = g_dbus_connection_signal_subscribe(e->bus, PORTAL_BUS, SHORTCUTS_IFACE, NULL, PORTAL_PATH, NULL,
                                                             G_DBUS_SIGNAL_FLAGS_NONE, on_portal_signal, e, NULL);
    e->portal_bound = true;
    if (bound == 0) vv_log_error("No hotkey can be expressed as a portal shortcut (modifier-only keys need raw input mode).");
    return true;
}

/* -------------------------------------------------------- engine */

static void binding_free(gpointer data) { Binding *b = data; g_free(b->profile_id); g_free(b->portal_id); g_free(b); }

VvHotkeyEngine *vv_hotkey_engine_new(VvHotkeyActionFn on_action, VvHotkeyControlFn on_control, gpointer user) {
    VvHotkeyEngine *e = g_new0(VvHotkeyEngine, 1);
    e->on_action = on_action; e->on_control = on_control; e->user = user;
    e->bindings = g_ptr_array_new_with_free_func(binding_free);
    e->fds = g_ptr_array_new_with_free_func((GDestroyNotify)g_io_channel_unref);
    e->watch_ids = g_ptr_array_new();
    vv_tap_init(&e->machine, VV_TAP_DOUBLE);
    e->backend_name = "none";
    e->status = g_strdup("Not started");
    return e;
}

void vv_hotkey_engine_free(VvHotkeyEngine *e) {
    if (!e) return;
    raw_stop(e); portal_stop(e);
    if (e->expiry_source) g_source_remove(e->expiry_source);
    g_ptr_array_unref(e->bindings); g_ptr_array_unref(e->fds); g_ptr_array_unref(e->watch_ids);
    if (e->bus) g_object_unref(e->bus);
    g_free(e->active_profile_id); g_free(e->status); g_free(e);
}

void vv_hotkey_engine_configure(VvHotkeyEngine *e, const VvConfig *config) {
    g_ptr_array_set_size(e->bindings, 0);
    for (guint i = 0; i < config->profiles->len; i++) {
        VvProfile *p = g_ptr_array_index(config->profiles, i);
        bool dup = false;
        for (guint j = 0; j < e->bindings->len; j++) if (vv_hotkey_same(&((Binding *)g_ptr_array_index(e->bindings, j))->spec, &p->hotkey)) dup = true;
        if (dup || p->hotkey.key_code == 0) continue;
        Binding *b = g_new0(Binding, 1);
        b->profile_id = g_strdup(p->id);
        b->spec = p->hotkey;
        b->portal_id = g_strdup_printf("profile-%s", p->id);
        g_ptr_array_add(e->bindings, b);
    }
    vv_tap_init(&e->machine, config->tap_start_mode);
    g_free(e->active_profile_id); e->active_profile_id = NULL;
    e->requested = config->hotkey_backend;

    bool want_raw = config->hotkey_backend == VV_HOTKEY_BACKEND_RAW
                 || (config->hotkey_backend == VV_HOTKEY_BACKEND_AUTO && vv_raw_input_available());
    g_free(e->status);
    if (want_raw && raw_start(e)) {
        portal_stop(e);
        e->backend_name = "raw";
        e->status = g_strdup("Raw input (/dev/input): any key, hold-to-talk. Ctrl+Alt+Esc cancels, Ctrl+Alt+Enter pastes a reviewed draft.");
        return;
    }
    raw_stop(e);
    char *error = NULL;
    if (portal_start(e, &error)) {
        e->backend_name = "portal";
        e->status = g_strdup("Portal shortcuts: bound via the system dialog; rebind in Settings → Keyboard. Ctrl+Alt+Esc cancels, Ctrl+Alt+Enter pastes a reviewed draft.");
    } else {
        e->backend_name = "none";
        e->status = g_strdup_printf("No hotkey backend available — portal: %s. For raw input, add yourself to the 'input' group: sudo usermod -aG input $USER, then log out and back in.", error ? error : "unavailable");
        g_free(error);
    }
}

void vv_hotkey_engine_set_recording_active(VvHotkeyEngine *e, bool active) { e->recording_active = active; }
void vv_hotkey_engine_set_review_active(VvHotkeyEngine *e, bool active) { e->review_active = active; }
const char *vv_hotkey_engine_backend_name(const VvHotkeyEngine *e) { return e->backend_name; }
char *vv_hotkey_engine_status(const VvHotkeyEngine *e) { return g_strdup(e->status); }
void vv_hotkey_engine_capture(VvHotkeyEngine *e, VvHotkeyCaptureFn fn, gpointer user) { e->capture_fn = fn; e->capture_user = user; }
void vv_hotkey_engine_cancel_capture(VvHotkeyEngine *e) { e->capture_fn = NULL; }
