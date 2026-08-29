#include "core/library.h"
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>

VvEntry *vv_entry_new(void) {
    VvEntry *e = g_new0(VvEntry, 1);
    e->id = g_strdup("");
    e->folder = g_strdup("Inbox");
    e->stt_label = g_strdup("");
    e->cleanup_label = g_strdup("");
    e->status = g_strdup("complete");
    e->cleaned = g_strdup("");
    e->raw = g_strdup("");
    return e;
}

void vv_entry_free(VvEntry *e) {
    if (!e) return;
    g_free(e->id); g_free(e->folder); g_free(e->stt_label); g_free(e->cleanup_label);
    g_free(e->status); g_free(e->cleaned); g_free(e->raw);
    g_free(e);
}

bool vv_entry_is_error(const VvEntry *e) { return g_str_has_prefix(e->status, "error"); }

VvLibrary *vv_library_new(const char *root) {
    VvLibrary *lib = g_new0(VvLibrary, 1);
    lib->root = g_strdup(root);
    char *inbox = g_build_filename(root, "Inbox", NULL);
    g_mkdir_with_parents(inbox, 0755);
    g_free(inbox);
    return lib;
}

void vv_library_free(VvLibrary *lib) { if (lib) { g_free(lib->root); g_free(lib); } }

static gint ci_compare(gconstpointer a, gconstpointer b) {
    char *fa = g_utf8_casefold(*(const char **)a, -1), *fb = g_utf8_casefold(*(const char **)b, -1);
    gint r = g_utf8_collate(fa, fb);
    g_free(fa); g_free(fb);
    return r;
}

GPtrArray *vv_library_folder_names(VvLibrary *lib) {
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    GDir *dir = g_dir_open(lib->root, 0, NULL);
    if (dir) {
        const char *name;
        while ((name = g_dir_read_name(dir))) {
            if (name[0] == '.' || strcmp(name, "Inbox") == 0) continue;
            char *path = g_build_filename(lib->root, name, NULL);
            if (g_file_test(path, G_FILE_TEST_IS_DIR)) g_ptr_array_add(names, g_strdup(name));
            g_free(path);
        }
        g_dir_close(dir);
    }
    g_ptr_array_sort(names, ci_compare);
    g_ptr_array_insert(names, 0, g_strdup("Inbox"));
    return names;
}

char *vv_library_sanitize_folder(const char *name) {
    char *s = g_strstrip(g_strdup(name));
    for (char *p = s; *p; p++) if (*p == '/' || *p == '\\' || *p == ':') *p = '-';
    return s;
}

void vv_library_create_folder(VvLibrary *lib, const char *name) {
    char *s = vv_library_sanitize_folder(name);
    if (*s) { char *path = g_build_filename(lib->root, s, NULL); g_mkdir_with_parents(path, 0755); g_free(path); }
    g_free(s);
}

char *vv_library_folder_path(VvLibrary *lib, const char *folder) { return g_build_filename(lib->root, folder, NULL); }

static gint desc_compare(gconstpointer a, gconstpointer b) { return strcmp(*(const char **)b, *(const char **)a); }

GPtrArray *vv_library_entry_ids(VvLibrary *lib, const char *folder) {
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
    char *path = vv_library_folder_path(lib, folder);
    GDir *dir = g_dir_open(path, 0, NULL);
    if (dir) {
        const char *name;
        while ((name = g_dir_read_name(dir)))
            if (g_str_has_suffix(name, ".md")) g_ptr_array_add(ids, g_strndup(name, strlen(name) - 3));
        g_dir_close(dir);
    }
    g_free(path);
    g_ptr_array_sort(ids, desc_compare);
    return ids;
}

VvEntry *vv_library_get_entry(VvLibrary *lib, const char *folder, const char *id) {
    char *dir = vv_library_folder_path(lib, folder);
    char *name = g_strconcat(id, ".md", NULL);
    char *path = g_build_filename(dir, name, NULL);
    char *text = NULL;
    VvEntry *e = NULL;
    if (g_file_get_contents(path, &text, NULL, NULL)) { e = vv_library_parse(text, id, folder); g_free(text); }
    g_free(path); g_free(name); g_free(dir);
    return e;
}

void vv_library_new_slot(VvLibrary *lib, const char *folder, char **id, char **audio_path) {
    char *dir = vv_library_folder_path(lib, folder);
    g_mkdir_with_parents(dir, 0755);
    GDateTime *now = g_date_time_new_now_local();
    char *stamp = g_date_time_format(now, "%Y%m%d-%H%M%S");
    g_date_time_unref(now);
    char *candidate = g_strdup(stamp);
    int suffix = 1;
    for (;;) {
        char *wav = g_strconcat(candidate, ".wav", NULL);
        char *path = g_build_filename(dir, wav, NULL);
        bool exists = g_file_test(path, G_FILE_TEST_EXISTS);
        g_free(wav);
        if (!exists) { *audio_path = path; break; }
        g_free(path);
        g_free(candidate);
        candidate = g_strdup_printf("%s-%d", stamp, ++suffix);
    }
    *id = candidate;
    g_free(stamp); g_free(dir);
}

bool vv_library_save(VvLibrary *lib, const VvEntry *e) {
    char *dir = vv_library_folder_path(lib, e->folder);
    g_mkdir_with_parents(dir, 0755);
    char *name = g_strconcat(e->id, ".md", NULL);
    char *path = g_build_filename(dir, name, NULL);
    char *md = vv_library_render(e);
    bool ok = g_file_set_contents(path, md, -1, NULL);
    g_free(md); g_free(path); g_free(name); g_free(dir);
    return ok;
}

void vv_library_delete(VvLibrary *lib, const VvEntry *e) {
    char *dir = vv_library_folder_path(lib, e->folder);
    const char *exts[] = { ".md", ".wav" };
    for (int i = 0; i < 2; i++) {
        char *name = g_strconcat(e->id, exts[i], NULL);
        char *path = g_build_filename(dir, name, NULL);
        g_unlink(path);
        g_free(path); g_free(name);
    }
    g_free(dir);
}

char *vv_library_audio_path(VvLibrary *lib, const VvEntry *e) {
    char *dir = vv_library_folder_path(lib, e->folder);
    char *name = g_strconcat(e->id, ".wav", NULL);
    char *path = g_build_filename(dir, name, NULL);
    g_free(name); g_free(dir);
    return path;
}

/* ------------------------------------------------------------ markdown */

char *vv_library_render_date(gint64 unix_seconds) {
    GDateTime *dt = g_date_time_new_from_unix_utc(unix_seconds);
    char *s = g_date_time_format(dt, "%Y-%m-%dT%H:%M:%SZ");
    g_date_time_unref(dt);
    return s;
}

char *vv_library_render(const VvEntry *e) {
    GString *sb = g_string_new("---\n");
    char *date = vv_library_render_date(e->date_unix);
    g_string_append_printf(sb, "date: %s\n", date);
    g_free(date);
    char dur[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(dur, sizeof dur, "%.1f", e->duration);
    g_string_append_printf(sb, "duration: %s\n", dur);
    g_string_append_printf(sb, "audio: %s.wav\n", e->id);
    g_string_append_printf(sb, "stt: %s\n", e->stt_label);
    if (*e->cleanup_label) g_string_append_printf(sb, "cleanup: %s\n", e->cleanup_label);
    g_string_append_printf(sb, "status: %s\n", e->status);
    g_string_append(sb, "---\n\n");
    g_string_append(sb, e->cleaned);
    if (*e->raw && strcmp(e->raw, e->cleaned) != 0) {
        g_string_append(sb, "\n\n## Raw transcript\n\n");
        g_string_append(sb, e->raw);
    }
    g_string_append_c(sb, '\n');
    return g_string_free(sb, FALSE);
}

static gint64 parse_iso_date(const char *value) {
    GDateTime *dt = g_date_time_new_from_iso8601(value, NULL);
    if (!dt) {
        /* Tolerate a missing timezone by assuming UTC. */
        char *z = g_strconcat(value, "Z", NULL);
        dt = g_date_time_new_from_iso8601(z, NULL);
        g_free(z);
    }
    if (!dt) return -1;
    gint64 seconds = g_date_time_to_unix(dt);
    g_date_time_unref(dt);
    return seconds;
}

static void set_str(char **field, const char *value) { g_free(*field); *field = g_strdup(value); }

VvEntry *vv_library_parse(const char *markdown, const char *id, const char *folder) {
    VvEntry *e = vv_entry_new();
    set_str(&e->id, id);
    set_str(&e->folder, folder);
    char *normalized = g_strjoinv("\n", g_strsplit(markdown, "\r\n", -1));
    char **lines = g_strsplit(normalized, "\n", -1);
    g_free(normalized);
    guint count = g_strv_length(lines);
    guint body_start = 0;
    bool got_date = false;

    if (count > 0 && strcmp(lines[0], "---") == 0) {
        int end = -1;
        for (guint i = 1; i < count; i++) if (strcmp(lines[i], "---") == 0) { end = (int)i; break; }
        if (end > 0) {
            for (int i = 1; i < end; i++) {
                const char *line = lines[i];
                const char *colon = strchr(line, ':');
                if (!colon) continue;
                char *key = g_strndup(line, colon - line);
                char *value = g_strstrip(g_strdup(colon + 1));
                if (strcmp(key, "date") == 0) {
                    gint64 seconds = parse_iso_date(value);
                    if (seconds >= 0) { e->date_unix = seconds; got_date = true; }
                } else if (strcmp(key, "duration") == 0) {
                    e->duration = g_ascii_strtod(value, NULL);
                } else if (strcmp(key, "stt") == 0) set_str(&e->stt_label, value);
                else if (strcmp(key, "cleanup") == 0) set_str(&e->cleanup_label, value);
                else if (strcmp(key, "status") == 0) set_str(&e->status, value);
                g_free(key); g_free(value);
            }
            body_start = (guint)end + 1;
        }
    }

    GString *body = g_string_new(NULL);
    for (guint i = body_start; i < count; i++) {
        if (i > body_start) g_string_append_c(body, '\n');
        g_string_append(body, lines[i]);
    }
    g_strfreev(lines);

    const char *marker = "\n## Raw transcript\n";
    const char *raw_at = strstr(body->str, marker);
    if (raw_at) {
        char *cleaned = g_strndup(body->str, raw_at - body->str);
        set_str(&e->cleaned, g_strstrip(cleaned));
        char *raw = g_strdup(raw_at + strlen(marker));
        set_str(&e->raw, g_strstrip(raw));
        g_free(cleaned); g_free(raw);
    } else {
        char *cleaned = g_strdup(body->str);
        set_str(&e->cleaned, g_strstrip(cleaned));
        set_str(&e->raw, e->cleaned);
        g_free(cleaned);
    }
    g_string_free(body, TRUE);

    if (!got_date && strlen(id) >= 15) {
        int Y, M, D, h, m, s;
        if (sscanf(id, "%4d%2d%2d-%2d%2d%2d", &Y, &M, &D, &h, &m, &s) == 6) {
            GDateTime *dt = g_date_time_new_local(Y, M, D, h, m, s);
            if (dt) { e->date_unix = g_date_time_to_unix(dt); g_date_time_unref(dt); }
        }
    }
    return e;
}
