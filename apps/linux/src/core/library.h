/* Files-first store: folders are directories under the library root, entries
 * are WAV + Markdown pairs. Byte-compatible with the other two apps
 * (docs/storage-format.md). */
#pragma once
#include <glib.h>
#include <stdbool.h>
#include "core/cleanup.h"

typedef struct {
    char *id;
    char *folder;
    gint64 date_unix;      /* seconds since epoch, UTC */
    double duration;
    char *stt_label;
    char *cleanup_label;
    char *status;
    char *cleaned;
    char *raw;
    /* Screenshot context saved beside the entry (<id>-screen-N.jpg): count,
     * 1-based index of the active display (0 = unknown), outline flag. */
    int screenshots;
    int active_screenshot;
    bool screenshot_outline;
} VvEntry;

/* Records the set's shape in the front matter fields (NULL clears). */
void vv_entry_attach(VvEntry *e, const VvScreenshotSet *set);

VvEntry *vv_entry_new(void);
void vv_entry_free(VvEntry *e);
bool vv_entry_is_error(const VvEntry *e);

typedef struct { char *root; } VvLibrary;

VvLibrary *vv_library_new(const char *root);
void vv_library_free(VvLibrary *lib);

GPtrArray *vv_library_folder_names(VvLibrary *lib);               /* char*, Inbox first */
char *vv_library_sanitize_folder(const char *name);
void vv_library_create_folder(VvLibrary *lib, const char *name);
char *vv_library_folder_path(VvLibrary *lib, const char *folder);
GPtrArray *vv_library_entry_ids(VvLibrary *lib, const char *folder); /* char*, newest first */
VvEntry *vv_library_get_entry(VvLibrary *lib, const char *folder, const char *id);
/* Reserve a new id + audio path for `folder`. Caller frees both. */
void vv_library_new_slot(VvLibrary *lib, const char *folder, char **id, char **audio_path);
bool vv_library_save(VvLibrary *lib, const VvEntry *e);
void vv_library_delete(VvLibrary *lib, const VvEntry *e);
char *vv_library_audio_path(VvLibrary *lib, const VvEntry *e);

/* Screenshots beside the entry (<id>-screen-N.jpg). */
void vv_library_save_screenshots(VvLibrary *lib, const char *id, const char *folder, const VvScreenshotSet *set);
VvScreenshotSet *vv_library_load_screenshots(VvLibrary *lib, const VvEntry *e);   /* NULL when none */
void vv_library_delete_screenshots(VvLibrary *lib, const char *id, const char *folder);

/* Markdown format (must match apps/macos and apps/windows). */
char *vv_library_render_date(gint64 unix_seconds);
char *vv_library_render(const VvEntry *e);
VvEntry *vv_library_parse(const char *markdown, const char *id, const char *folder);
