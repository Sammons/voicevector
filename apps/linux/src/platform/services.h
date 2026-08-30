/* Paste, secrets, screenshots — portal-first, no privileges. */
#pragma once
#include <glib.h>
#include <stdbool.h>

typedef enum { VV_PASTE_PASTED, VV_PASTE_COPIED_ONLY } VvPasteOutcome;
/* Copies `text` to the clipboard, then tries to type Ctrl+V into the
 * focused app via the RemoteDesktop portal. `reason` (g_free) explains a
 * copied-only outcome. Must be called on the GTK main thread. */
VvPasteOutcome vv_paste_insert(const char *text, bool auto_paste, char **reason);
bool vv_paste_portal_available(void);

/* API keys in the Secret Service (GNOME Keyring / KWallet), keyed by provider id. */
char *vv_secret_get(const char *provider_id);              /* NULL when absent */
bool vv_secret_set(const char *provider_id, const char *api_key);
void vv_secret_delete(const char *provider_id);

/* One JPEG (≤1280 px) per display, cropped from the Screenshot portal's
 * desktop capture; GPtrArray of VvScreenshot (free func set), NULL on failure. */
GPtrArray *vv_screenshots(void);
