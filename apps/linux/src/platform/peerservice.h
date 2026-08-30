/* Multi-machine peering: TLS listener + client (docs/multi-machine.md).
 * Identity: P-256 self-signed cert generated once via the openssl CLI; the
 * private key lives in the Secret Service, the certificate beside the
 * config. TLS via GIO (needs the glib-networking runtime module, standard
 * on desktop distros). Mirrors macOS/Windows PeerService.
 * Blocking calls (pair/context/deliver) must run OFF the main thread. */
#pragma once
#include <glib.h>
#include <stdbool.h>
#include "core/config.h"
#include "core/cleanup.h"

/* One machine's routing context. */
typedef struct {
    char *machine;
    bool is_local;
    char *window_lines;      /* "" on Wayland peers */
    GPtrArray *screens;      /* VvScreenshot*, captions prefixed with the machine */
} VvMachineContext;
void vv_machine_context_free(VvMachineContext *ctx);

/* Inbound delivery: paste text (window id is best-effort; 0 = focus) and call
 * done(ok, error) when finished. Invoked on the main loop. */
typedef void (*VvPeerDeliverFn)(const char *text, guint32 window,
                                void (*done)(bool ok, const char *error, gpointer token),
                                gpointer token, gpointer user);
/* Inbound pairing: show the code; call answer(accepted). On the main loop. */
typedef void (*VvPeerPairFn)(const char *name, const char *code,
                             void (*answer)(bool accepted, gpointer token),
                             gpointer token, gpointer user);

void vv_peer_service_init(VvConfig *config /* borrowed */,
                          VvPeerDeliverFn on_deliver, VvPeerPairFn on_pair, gpointer user);
/* Start/stop the listener to match config->multi_machine. Main thread. */
void vv_peer_service_apply(void);
/* NULL until the identity exists (created when peering is first enabled). */
char *vv_peer_service_fingerprint_hex(void);

/* Outbound pairing (runs on its own thread). on_code fires on the main loop
 * with the 6-digit code and an answer function; done fires on the main loop
 * with the stored peer (caller owns) or NULL + error (g_free). */
typedef void (*VvPeerCodeFn)(const char *code, void (*answer)(bool accepted, gpointer token),
                             gpointer token, gpointer user);
typedef void (*VvPeerPairDoneFn)(VvPeer *peer_or_null, const char *error, gpointer user);
void vv_peer_service_pair_async(const char *address, VvPeerCodeFn on_code,
                                VvPeerPairDoneFn done, gpointer user);

/* Blocking; call from a worker thread. NULL on failure. */
VvMachineContext *vv_peer_service_fetch_context(const VvPeer *peer);
/* Blocking; NULL on success, else an error message (g_free). */
char *vv_peer_service_deliver(const VvPeer *peer, const char *text, guint32 window);

/* This machine's context for the router (main thread — takes screenshots). */
VvMachineContext *vv_peer_service_local_context(const char *machine_name,
                                                VvScreenshotSet *screens_or_null);
