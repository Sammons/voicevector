#include "platform/peerservice.h"
#include "platform/services.h"
#include "core/peer.h"
#include "core/log.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

static VvConfig *config;
static VvPeerDeliverFn deliver_cb;
static VvPeerPairFn pair_cb;
static gpointer cb_user;
static GSocketService *service;
static int service_port = -1;
static GTlsCertificate *identity;
static char *identity_fp_hex;          /* lowercase hex */
static GBytes *identity_fp;            /* 32 bytes */
static volatile int pairing_busy;

void vv_machine_context_free(VvMachineContext *ctx) {
    if (!ctx) return;
    g_free(ctx->machine); g_free(ctx->window_lines);
    if (ctx->screens) g_ptr_array_unref(ctx->screens);
    g_free(ctx);
}

/* ------------------------------------------------------ identity */

static char *config_dir(void) {
    return g_build_filename(g_get_user_config_dir(), "voicevector", NULL);
}

static GBytes *pem_to_der(const char *pem) {
    const char *start = strstr(pem, "-----BEGIN CERTIFICATE-----");
    if (!start) return NULL;
    start += strlen("-----BEGIN CERTIFICATE-----");
    const char *end = strstr(start, "-----END CERTIFICATE-----");
    if (!end) return NULL;
    char *b64 = g_strndup(start, end - start);
    char *compact = g_strdup(b64);
    int at = 0;
    for (const char *p = b64; *p; p++) if (!g_ascii_isspace(*p)) compact[at++] = *p;
    compact[at] = 0;
    gsize n = 0;
    guchar *der = g_base64_decode(compact, &n);
    g_free(b64); g_free(compact);
    if (!der || n == 0) { g_free(der); return NULL; }
    return g_bytes_new_take(der, n);
}

/* Loads (or creates, once) the identity certificate + key. */
static bool ensure_identity(void) {
    if (identity) return true;
    char *dir = config_dir();
    char *cert_path = g_build_filename(dir, "peer-cert.pem", NULL);
    char *cert_pem = NULL, *key_pem = vv_secret_get("peer-key");
    g_file_get_contents(cert_path, &cert_pem, NULL, NULL);
    if (!cert_pem || !key_pem) {
        g_free(cert_pem); cert_pem = NULL;
        g_free(key_pem); key_pem = NULL;
        /* One-time generation via the openssl CLI (present on any desktop). */
        char *key_tmp = g_build_filename(g_get_tmp_dir(), "vv-peer-key.pem", NULL);
        char *cert_tmp = g_build_filename(g_get_tmp_dir(), "vv-peer-cert.pem", NULL);
        char *subject = g_strdup_printf("/CN=VoiceVector-%s", vv_multi_machine_name(&config->multi_machine));
        char *argv[] = { "openssl", "req", "-x509", "-newkey", "ec",
                         "-pkeyopt", "ec_paramgen_curve:P-256", "-sha256",
                         "-days", "36500", "-nodes", "-subj", subject,
                         "-keyout", key_tmp, "-out", cert_tmp, NULL };
        int status = -1;
        GError *error = NULL;
        bool ran = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL
                                | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &status, &error);
        if (ran && status == 0
            && g_file_get_contents(key_tmp, &key_pem, NULL, NULL)
            && g_file_get_contents(cert_tmp, &cert_pem, NULL, NULL)) {
            g_mkdir_with_parents(dir, 0700);
            g_file_set_contents(cert_path, cert_pem, -1, NULL);
            vv_secret_set("peer-key", key_pem);
        } else {
            vv_log_error("Peer identity: openssl generation failed (%s)",
                         error ? error->message : "nonzero exit");
        }
        if (error) g_error_free(error);
        g_unlink(key_tmp); g_unlink(cert_tmp);
        g_free(key_tmp); g_free(cert_tmp); g_free(subject);
    }
    if (cert_pem && key_pem) {
        char *both = g_strconcat(cert_pem, "\n", key_pem, NULL);
        GError *error = NULL;
        identity = g_tls_certificate_new_from_pem(both, -1, &error);
        if (!identity) {
            vv_log_error("Peer identity: %s", error ? error->message : "bad PEM");
            if (error) g_error_free(error);
        } else {
            GBytes *der = pem_to_der(cert_pem);
            if (der) {
                identity_fp = vv_peer_fingerprint(der);
                identity_fp_hex = vv_peer_hex(g_bytes_get_data(identity_fp, NULL), 32);
                g_bytes_unref(der);
            }
        }
        g_free(both);
    }
    g_free(cert_pem); g_free(key_pem); g_free(cert_path); g_free(dir);
    return identity != NULL && identity_fp != NULL;
}

char *vv_peer_service_fingerprint_hex(void) {
    return identity_fp_hex ? g_strdup(identity_fp_hex) : NULL;
}

static GBytes *connection_peer_fp(GTlsConnection *tls) {
    GTlsCertificate *peer = g_tls_connection_get_peer_certificate(tls);
    if (!peer) return NULL;
    GByteArray *der = NULL;
    g_object_get(peer, "certificate", &der, NULL);
    if (!der) return NULL;
    GBytes *bytes = g_bytes_new(der->data, der->len);
    GBytes *fp = vv_peer_fingerprint(bytes);
    g_bytes_unref(bytes);
    g_byte_array_unref(der);
    return fp;
}

static gboolean accept_any_cert(GTlsConnection *c, GTlsCertificate *cert,
                                GTlsCertificateFlags errors, gpointer user) {
    /* Self-signed certs never chain; pinning happens at the app layer. */
    return TRUE;
}

/* ------------------------------------------------- frame plumbing */

static VvJson *read_frame(GIOStream *stream, GByteArray *buffer) {
    GInputStream *in = g_io_stream_get_input_stream(stream);
    guint8 chunk[65536];
    while (true) {
        bool bad = false;
        VvJson *obj = vv_peer_parse_frame(buffer, &bad);
        if (obj) return obj;
        if (bad) return NULL;
        gssize n = g_input_stream_read(in, chunk, sizeof chunk, NULL, NULL);
        if (n <= 0) return NULL;
        g_byte_array_append(buffer, chunk, (guint)n);
    }
}

static bool send_frame(GIOStream *stream, VvJson *obj_take) {
    GBytes *frame = vv_peer_frame(obj_take);
    gsize n; const guint8 *data = g_bytes_get_data(frame, &n);
    bool ok = g_output_stream_write_all(g_io_stream_get_output_stream(stream),
                                        data, n, NULL, NULL, NULL);
    g_bytes_unref(frame);
    return ok;
}

static VvJson *msg(const char *type) {
    VvJson *o = vv_json_object();
    vv_json_object_set(o, "t", vv_json_string(type));
    return o;
}

static VvJson *err_msg(const char *error) {
    VvJson *o = msg("err");
    vv_json_object_set(o, "err", vv_json_string(error));
    return o;
}

/* ------------------------------------------------- main-loop bridges */

typedef struct {
    GMutex lock; GCond cond;
    bool done, ok;
    char *error;
    char *code_answered;         /* unused for deliver */
} Waiter;

static void waiter_done(bool ok, const char *error, gpointer token) {
    Waiter *w = token;
    g_mutex_lock(&w->lock);
    w->ok = ok;
    w->error = g_strdup(error ? error : "");
    w->done = true;
    g_cond_signal(&w->cond);
    g_mutex_unlock(&w->lock);
}

typedef struct { char *text; guint32 window; Waiter *waiter; } DeliverIdle;

static gboolean deliver_on_main(gpointer data) {
    DeliverIdle *d = data;
    if (deliver_cb) deliver_cb(d->text, d->window, waiter_done, d->waiter, cb_user);
    else waiter_done(false, "not ready", d->waiter);
    g_free(d->text); g_free(d);
    return G_SOURCE_REMOVE;
}

typedef struct { char *name; char *code; Waiter *waiter; } PairIdle;

static void pair_answered(bool accepted, gpointer token) {
    waiter_done(accepted, NULL, token);
}

static gboolean pair_on_main(gpointer data) {
    PairIdle *p = data;
    if (pair_cb) pair_cb(p->name, p->code, pair_answered, p->waiter, cb_user);
    else waiter_done(false, "no ui", p->waiter);
    g_free(p->name); g_free(p->code); g_free(p);
    return G_SOURCE_REMOVE;
}

typedef struct { GPtrArray *out; GMutex lock; GCond cond; bool done; } ContextIdle;

static gboolean local_context_on_main(gpointer data) {
    ContextIdle *ci = data;
    VvMachineContext *ctx = vv_peer_service_local_context(
        vv_multi_machine_name(&config->multi_machine), NULL);
    g_mutex_lock(&ci->lock);
    g_ptr_array_add(ci->out, ctx);
    ci->done = true;
    g_cond_signal(&ci->cond);
    g_mutex_unlock(&ci->lock);
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------- server */

static VvPeer *find_peer_by_fp(const char *fp_hex) {
    for (guint i = 0; i < config->multi_machine.peers->len; i++) {
        VvPeer *p = g_ptr_array_index(config->multi_machine.peers, i);
        if (g_strcmp0(p->fingerprint, fp_hex) == 0) return p;
    }
    return NULL;
}

typedef struct { char *name; char *fp; } AddPeerIdle;

static gboolean add_peer_on_main(gpointer data) {
    AddPeerIdle *a = data;
    if (!find_peer_by_fp(a->fp)) {
        VvPeer *p = vv_peer_ref_new();
        g_free(p->name); p->name = g_strdup(a->name);
        g_free(p->fingerprint); p->fingerprint = g_strdup(a->fp);
        g_ptr_array_add(config->multi_machine.peers, p);
        char *path = vv_config_default_path();
        vv_config_save(config, path);
        g_free(path);
    }
    g_free(a->name); g_free(a->fp); g_free(a);
    return G_SOURCE_REMOVE;
}

static void serve_pairing(GIOStream *tls, GByteArray *buffer, const char *peer_name, GBytes *client_fp) {
    if (!g_atomic_int_compare_and_exchange(&pairing_busy, 0, 1)) {
        send_frame(tls, err_msg("busy"));
        return;
    }
    guint8 nonce[32];
    for (int i = 0; i < 32; i++) nonce[i] = (guint8)g_random_int_range(0, 256);
    VvJson *hello = msg("hello");
    vv_json_object_set(hello, "ver", vv_json_number(1));
    vv_json_object_set(hello, "name", vv_json_string(vv_multi_machine_name(&config->multi_machine)));
    if (!send_frame(tls, hello)) goto out;
    VvJson *commit = read_frame(tls, buffer);
    char *their_commit = commit ? g_strdup(vv_json_get_string(commit, "h", "")) : NULL;
    vv_json_free(commit);
    if (!their_commit) goto out;
    VvJson *mine = msg("commit");
    char *my_commit = vv_peer_commitment(nonce, 32);
    vv_json_object_set(mine, "h", vv_json_string(my_commit));
    g_free(my_commit);
    if (!send_frame(tls, mine)) { g_free(their_commit); goto out; }
    VvJson *reveal = read_frame(tls, buffer);
    GBytes *their_nonce = reveal ? vv_peer_unhex(vv_json_get_string(reveal, "n", "")) : NULL;
    vv_json_free(reveal);
    bool commit_ok = false;
    if (their_nonce && g_bytes_get_size(their_nonce) == 32) {
        char *check = vv_peer_commitment(g_bytes_get_data(their_nonce, NULL), 32);
        commit_ok = g_strcmp0(check, their_commit) == 0;
        g_free(check);
    }
    g_free(their_commit);
    if (!commit_ok) { if (their_nonce) g_bytes_unref(their_nonce); goto out; }
    VvJson *rv = msg("reveal");
    char *nonce_hex = vv_peer_hex(nonce, 32);
    vv_json_object_set(rv, "n", vv_json_string(nonce_hex));
    g_free(nonce_hex);
    if (!send_frame(tls, rv)) { g_bytes_unref(their_nonce); goto out; }
    char *code = vv_peer_pairing_code(g_bytes_get_data(client_fp, NULL),
                                      g_bytes_get_data(identity_fp, NULL),
                                      g_bytes_get_data(their_nonce, NULL), nonce);
    g_bytes_unref(their_nonce);

    Waiter waiter = { 0 };
    g_mutex_init(&waiter.lock); g_cond_init(&waiter.cond);
    PairIdle *pi = g_new0(PairIdle, 1);
    pi->name = g_strdup(peer_name); pi->code = code; pi->waiter = &waiter;
    g_idle_add(pair_on_main, pi);
    g_mutex_lock(&waiter.lock);
    while (!waiter.done) g_cond_wait(&waiter.cond, &waiter.lock);
    bool accepted = waiter.ok;
    g_mutex_unlock(&waiter.lock);
    g_free(waiter.error);
    if (!accepted) { send_frame(tls, msg("deny")); goto out; }
    VvJson *answer = read_frame(tls, buffer);
    bool confirmed = answer && g_strcmp0(vv_json_get_string(answer, "t", ""), "confirm") == 0;
    vv_json_free(answer);
    if (confirmed) {
        AddPeerIdle *ai = g_new0(AddPeerIdle, 1);
        ai->name = g_strdup(peer_name);
        ai->fp = vv_peer_hex(g_bytes_get_data(client_fp, NULL), 32);
        g_idle_add(add_peer_on_main, ai);
        send_frame(tls, msg("confirm"));
    }
out:
    g_atomic_int_set(&pairing_busy, 0);
}

static void serve_peer(GIOStream *tls, GByteArray *buffer, GBytes *client_fp) {
    char *fp_hex = vv_peer_hex(g_bytes_get_data(client_fp, NULL), 32);
    VvPeer *peer = find_peer_by_fp(fp_hex);
    g_free(fp_hex);
    if (!peer) { send_frame(tls, err_msg("untrusted")); return; }
    bool allow_screens = peer->allow_screens, allow_deliver = peer->allow_deliver;
    VvJson *hello = msg("hello");
    vv_json_object_set(hello, "ver", vv_json_number(1));
    vv_json_object_set(hello, "name", vv_json_string(vv_multi_machine_name(&config->multi_machine)));
    if (!send_frame(tls, hello)) return;
    VvJson *request = read_frame(tls, buffer);
    if (!request) return;
    const char *type = vv_json_get_string(request, "t", "");
    if (strcmp(type, "context") == 0) {
        if (!allow_screens) { send_frame(tls, err_msg("screens not allowed")); vv_json_free(request); return; }
        ContextIdle ci = { 0 };
        ci.out = g_ptr_array_new();
        g_mutex_init(&ci.lock); g_cond_init(&ci.cond);
        g_idle_add(local_context_on_main, &ci);
        g_mutex_lock(&ci.lock);
        while (!ci.done) g_cond_wait(&ci.cond, &ci.lock);
        g_mutex_unlock(&ci.lock);
        VvMachineContext *ctx = ci.out->len ? g_ptr_array_index(ci.out, 0) : NULL;
        g_ptr_array_unref(ci.out);
        VvJson *reply = msg("context");
        vv_json_object_set(reply, "machine", vv_json_string(vv_multi_machine_name(&config->multi_machine)));
        VvJson *screens = vv_json_array();
        if (ctx) {
            for (guint i = 0; i < ctx->screens->len; i++) {
                VvScreenshot *shot = g_ptr_array_index(ctx->screens, i);
                gsize n; const guchar *data = g_bytes_get_data(shot->jpeg, &n);
                char *b64 = g_base64_encode(data, n);
                VvJson *sj = vv_json_object();
                vv_json_object_set(sj, "jpeg", vv_json_string(b64));
                /* Strip the local machine prefix; the fetcher adds its own. */
                const char *caption = shot->caption;
                const char *dash = strstr(caption, "— ");
                vv_json_object_set(sj, "caption", vv_json_string(dash ? dash + strlen("— ") : caption));
                vv_json_array_add(screens, sj);
                g_free(b64);
            }
        }
        vv_json_object_set(reply, "screens", screens);
        vv_json_object_set(reply, "windows", vv_json_array());   /* Wayland: unknowable */
        send_frame(tls, reply);
        if (ctx) vv_machine_context_free(ctx);
    } else if (strcmp(type, "deliver") == 0) {
        if (!allow_deliver) { send_frame(tls, err_msg("deliver not allowed")); vv_json_free(request); return; }
        const char *text = vv_json_get_string(request, "text", "");
        guint32 window = (guint32)vv_json_get_number(request, "window", 0);
        if (!*text) { send_frame(tls, err_msg("empty")); vv_json_free(request); return; }
        Waiter waiter = { 0 };
        g_mutex_init(&waiter.lock); g_cond_init(&waiter.cond);
        DeliverIdle *di = g_new0(DeliverIdle, 1);
        di->text = g_strdup(text); di->window = window; di->waiter = &waiter;
        g_idle_add(deliver_on_main, di);
        g_mutex_lock(&waiter.lock);
        while (!waiter.done) g_cond_wait(&waiter.cond, &waiter.lock);
        g_mutex_unlock(&waiter.lock);
        send_frame(tls, waiter.ok ? msg("ok") : err_msg(*waiter.error ? waiter.error : "delivery failed"));
        g_free(waiter.error);
    }
    vv_json_free(request);
}

static gpointer serve_thread(gpointer data) {
    GSocketConnection *raw = data;
    GError *error = NULL;
    GIOStream *tls = g_tls_server_connection_new(G_IO_STREAM(raw), identity, &error);
    if (!tls) {
        vv_log_error("Peer TLS: %s (is glib-networking installed?)", error ? error->message : "?");
        if (error) g_error_free(error);
        g_object_unref(raw);
        return NULL;
    }
    g_object_set(tls, "authentication-mode", G_TLS_AUTHENTICATION_REQUIRED, NULL);
    g_signal_connect(tls, "accept-certificate", G_CALLBACK(accept_any_cert), NULL);
    if (g_tls_connection_handshake(G_TLS_CONNECTION(tls), NULL, NULL)) {
        GBytes *client_fp = connection_peer_fp(G_TLS_CONNECTION(tls));
        if (client_fp) {
            GByteArray *buffer = g_byte_array_new();
            VvJson *hello = read_frame(tls, buffer);
            if (hello && g_strcmp0(vv_json_get_string(hello, "t", ""), "hello") == 0) {
                const char *purpose = vv_json_get_string(hello, "purpose", "");
                char *peer_name = g_strdup(vv_json_get_string(hello, "name", "?"));
                if (strcmp(purpose, "pair") == 0) serve_pairing(tls, buffer, peer_name, client_fp);
                else if (strcmp(purpose, "peer") == 0) serve_peer(tls, buffer, client_fp);
                g_free(peer_name);
            }
            vv_json_free(hello);
            g_byte_array_unref(buffer);
            g_bytes_unref(client_fp);
        }
    }
    g_io_stream_close(tls, NULL, NULL);
    g_object_unref(tls);
    g_object_unref(raw);
    return NULL;
}

static gboolean on_incoming(GSocketService *svc, GSocketConnection *connection,
                            GObject *source, gpointer user) {
    GThread *t = g_thread_new("vv-peer-serve", serve_thread, g_object_ref(connection));
    g_thread_unref(t);
    return TRUE;
}

void vv_peer_service_init(VvConfig *c, VvPeerDeliverFn on_deliver, VvPeerPairFn on_pair, gpointer user) {
    config = c;
    deliver_cb = on_deliver;
    pair_cb = on_pair;
    cb_user = user;
}

void vv_peer_service_apply(void) {
    if (!config) return;
    VvMultiMachine *mm = &config->multi_machine;
    if (!mm->enabled) {
        if (service) { g_socket_service_stop(service); g_object_unref(service); service = NULL; service_port = -1; }
        return;
    }
    if (!ensure_identity()) return;
    if (service && service_port == mm->port) return;
    if (service) { g_socket_service_stop(service); g_object_unref(service); service = NULL; }
    service = g_socket_service_new();
    GError *error = NULL;
    if (!g_socket_listener_add_inet_port(G_SOCKET_LISTENER(service), (guint16)mm->port, NULL, &error)) {
        vv_log_error("Peer listener: %s", error ? error->message : "?");
        if (error) g_error_free(error);
        g_object_unref(service); service = NULL;
        return;
    }
    service_port = mm->port;
    g_signal_connect(service, "incoming", G_CALLBACK(on_incoming), NULL);
    g_socket_service_start(service);
}

/* ------------------------------------------------------- client */

static GIOStream *open_session(const char *address, const char *purpose,
                               GSocketConnection **raw_out, GByteArray **buffer_out,
                               char **server_name_out, char **server_fp_out, char **error_out) {
    *error_out = NULL;
    if (!ensure_identity()) { *error_out = g_strdup("no identity"); return NULL; }
    char *host = g_strdup(address);
    int port = config->multi_machine.port;
    char *colon = strrchr(host, ':');
    if (colon && strchr(host, ':') == colon) { *colon = 0; port = atoi(colon + 1); }
    GSocketClient *client = g_socket_client_new();
    g_socket_client_set_timeout(client, 8);
    GError *gerr = NULL;
    GSocketConnection *raw = g_socket_client_connect_to_host(client, host, (guint16)port, NULL, &gerr);
    g_object_unref(client);
    g_free(host);
    if (!raw) {
        *error_out = g_strdup(gerr ? gerr->message : "could not connect");
        if (gerr) g_error_free(gerr);
        return NULL;
    }
    GIOStream *tls = g_tls_client_connection_new(G_IO_STREAM(raw), NULL, &gerr);
    if (!tls) {
        *error_out = g_strdup_printf("TLS unavailable: %s (is glib-networking installed?)",
                                     gerr ? gerr->message : "?");
        if (gerr) g_error_free(gerr);
        g_object_unref(raw);
        return NULL;
    }
    g_tls_connection_set_certificate(G_TLS_CONNECTION(tls), identity);
    g_tls_client_connection_set_validation_flags(G_TLS_CLIENT_CONNECTION(tls), 0);
    g_signal_connect(tls, "accept-certificate", G_CALLBACK(accept_any_cert), NULL);
    if (!g_tls_connection_handshake(G_TLS_CONNECTION(tls), NULL, &gerr)) {
        *error_out = g_strdup(gerr ? gerr->message : "TLS handshake failed");
        if (gerr) g_error_free(gerr);
        g_object_unref(tls); g_object_unref(raw);
        return NULL;
    }
    GBytes *fp = connection_peer_fp(G_TLS_CONNECTION(tls));
    *server_fp_out = fp ? vv_peer_hex(g_bytes_get_data(fp, NULL), 32) : g_strdup("");
    if (fp) g_bytes_unref(fp);
    GByteArray *buffer = g_byte_array_new();
    VvJson *hello = msg("hello");
    vv_json_object_set(hello, "ver", vv_json_number(1));
    vv_json_object_set(hello, "name", vv_json_string(vv_multi_machine_name(&config->multi_machine)));
    vv_json_object_set(hello, "purpose", vv_json_string(purpose));
    VvJson *reply = send_frame(tls, hello) ? read_frame(tls, buffer) : NULL;
    if (!reply || g_strcmp0(vv_json_get_string(reply, "t", ""), "hello") != 0) {
        *error_out = g_strdup(reply ? vv_json_get_string(reply, "err", "handshake failed") : "handshake failed");
        vv_json_free(reply);
        g_byte_array_unref(buffer);
        g_free(*server_fp_out); *server_fp_out = NULL;
        g_io_stream_close(tls, NULL, NULL);
        g_object_unref(tls); g_object_unref(raw);
        return NULL;
    }
    *server_name_out = g_strdup(vv_json_get_string(reply, "name", "?"));
    vv_json_free(reply);
    *raw_out = raw;
    *buffer_out = buffer;
    return tls;
}

static void close_session(GIOStream *tls, GSocketConnection *raw, GByteArray *buffer) {
    g_io_stream_close(tls, NULL, NULL);
    g_object_unref(tls);
    g_object_unref(raw);
    g_byte_array_unref(buffer);
}

/* -- pairing (worker thread + main-loop callbacks) -- */

typedef struct {
    char *address;
    VvPeerCodeFn on_code;
    VvPeerPairDoneFn done;
    gpointer user;
    /* result */
    VvPeer *peer;
    char *error;
    char *code;
    Waiter answer;
} PairTask;

static gboolean pair_task_code_on_main(gpointer data) {
    PairTask *t = data;
    t->on_code(t->code, pair_answered, &t->answer, t->user);
    return G_SOURCE_REMOVE;
}

static gboolean pair_task_done_on_main(gpointer data) {
    PairTask *t = data;
    t->done(t->peer, t->error, t->user);
    g_free(t->address); g_free(t->error); g_free(t->code);
    g_mutex_clear(&t->answer.lock); g_cond_clear(&t->answer.cond);
    g_free(t);
    return G_SOURCE_REMOVE;
}

static gpointer pair_thread(gpointer data) {
    PairTask *t = data;
    GSocketConnection *raw = NULL; GByteArray *buffer = NULL;
    char *server_name = NULL, *server_fp = NULL;
    GIOStream *tls = open_session(t->address, "pair", &raw, &buffer, &server_name, &server_fp, &t->error);
    if (!tls) { g_idle_add(pair_task_done_on_main, t); return NULL; }
    guint8 nonce[32];
    for (int i = 0; i < 32; i++) nonce[i] = (guint8)g_random_int_range(0, 256);
    VvJson *commit = msg("commit");
    char *my_commit = vv_peer_commitment(nonce, 32);
    vv_json_object_set(commit, "h", vv_json_string(my_commit));
    g_free(my_commit);
    bool ok = send_frame(tls, commit);
    VvJson *their_commit_msg = ok ? read_frame(tls, buffer) : NULL;
    char *their_commit = their_commit_msg
        ? g_strdup(vv_json_get_string(their_commit_msg, "h", "")) : NULL;
    vv_json_free(their_commit_msg);
    GBytes *their_nonce = NULL;
    if (their_commit) {
        VvJson *rv = msg("reveal");
        char *hex = vv_peer_hex(nonce, 32);
        vv_json_object_set(rv, "n", vv_json_string(hex));
        g_free(hex);
        VvJson *their_reveal = send_frame(tls, rv) ? read_frame(tls, buffer) : NULL;
        their_nonce = their_reveal ? vv_peer_unhex(vv_json_get_string(their_reveal, "n", "")) : NULL;
        vv_json_free(their_reveal);
        if (their_nonce && g_bytes_get_size(their_nonce) == 32) {
            char *check = vv_peer_commitment(g_bytes_get_data(their_nonce, NULL), 32);
            if (g_strcmp0(check, their_commit) != 0) { g_bytes_unref(their_nonce); their_nonce = NULL; }
            g_free(check);
        } else if (their_nonce) {
            g_bytes_unref(their_nonce); their_nonce = NULL;
        }
        g_free(their_commit);
    }
    if (!their_nonce) {
        t->error = g_strdup("pairing failed");
    } else {
        GBytes *server_fp_bytes = vv_peer_unhex(server_fp);
        t->code = vv_peer_pairing_code(g_bytes_get_data(identity_fp, NULL),
                                       g_bytes_get_data(server_fp_bytes, NULL),
                                       nonce, g_bytes_get_data(their_nonce, NULL));
        g_bytes_unref(server_fp_bytes);
        g_bytes_unref(their_nonce);
        g_mutex_init(&t->answer.lock); g_cond_init(&t->answer.cond);
        g_idle_add(pair_task_code_on_main, t);
        g_mutex_lock(&t->answer.lock);
        while (!t->answer.done) g_cond_wait(&t->answer.cond, &t->answer.lock);
        bool accepted = t->answer.ok;
        g_mutex_unlock(&t->answer.lock);
        g_free(t->answer.error);
        if (!accepted) {
            send_frame(tls, msg("deny"));
            t->error = g_strdup("cancelled");
        } else if (send_frame(tls, msg("confirm"))) {
            VvJson *confirmed = read_frame(tls, buffer);
            if (confirmed && g_strcmp0(vv_json_get_string(confirmed, "t", ""), "confirm") == 0) {
                t->peer = vv_peer_ref_new();
                g_free(t->peer->name); t->peer->name = g_strdup(server_name);
                g_free(t->peer->fingerprint); t->peer->fingerprint = g_strdup(server_fp);
                g_free(t->peer->address); t->peer->address = g_strdup(t->address);
            } else {
                t->error = g_strdup("the other machine denied the pairing");
            }
            vv_json_free(confirmed);
        } else {
            t->error = g_strdup("connection lost");
        }
    }
    g_free(server_name); g_free(server_fp);
    close_session(tls, raw, buffer);
    g_idle_add(pair_task_done_on_main, t);
    return NULL;
}

void vv_peer_service_pair_async(const char *address, VvPeerCodeFn on_code,
                                VvPeerPairDoneFn done, gpointer user) {
    PairTask *t = g_new0(PairTask, 1);
    t->address = g_strdup(address);
    t->on_code = on_code; t->done = done; t->user = user;
    GThread *th = g_thread_new("vv-peer-pair", pair_thread, t);
    g_thread_unref(th);
}

/* -- context + deliver (blocking; worker thread) -- */

VvMachineContext *vv_peer_service_fetch_context(const VvPeer *peer) {
    if (!peer->address || !*peer->address) return NULL;
    GSocketConnection *raw = NULL; GByteArray *buffer = NULL;
    char *server_name = NULL, *server_fp = NULL, *error = NULL;
    GIOStream *tls = open_session(peer->address, "peer", &raw, &buffer, &server_name, &server_fp, &error);
    if (!tls) { g_free(error); return NULL; }
    VvMachineContext *ctx = NULL;
    if (g_strcmp0(server_fp, peer->fingerprint) == 0 && send_frame(tls, msg("context"))) {
        VvJson *reply = read_frame(tls, buffer);
        if (reply && g_strcmp0(vv_json_get_string(reply, "t", ""), "context") == 0) {
            const char *machine = vv_json_get_string(reply, "machine", peer->name);
            ctx = g_new0(VvMachineContext, 1);
            ctx->machine = g_strdup(machine);
            ctx->window_lines = g_strdup("");
            ctx->screens = g_ptr_array_new_with_free_func((GDestroyNotify)vv_screenshot_free);
            VvJson *screens = vv_json_get_array(reply, "screens");
            for (guint i = 0; screens && i < vv_json_array_length(screens); i++) {
                VvJson *sj = vv_json_array_get(screens, i);
                const char *b64 = sj ? vv_json_get_string(sj, "jpeg", NULL) : NULL;
                if (!b64) continue;
                gsize n = 0;
                guchar *jpeg = g_base64_decode(b64, &n);
                if (!jpeg || !n) { g_free(jpeg); continue; }
                GBytes *bytes = g_bytes_new_take(jpeg, n);
                char *caption = g_strdup_printf("Machine \"%s\" — %s", machine,
                                                vv_json_get_string(sj, "caption", ""));
                g_ptr_array_add(ctx->screens, vv_screenshot_new(bytes, caption));
                g_bytes_unref(bytes);
            }
            VvJson *windows = vv_json_get_array(reply, "windows");
            GString *lines = g_string_new(NULL);
            for (guint i = 0; windows && i < vv_json_array_length(windows); i++) {
                VvJson *wj = vv_json_array_get(windows, i);
                if (!wj) continue;
                if (lines->len) g_string_append_c(lines, '\n');
                g_string_append_printf(lines, "%u: %s — %s",
                                       (guint)vv_json_get_number(wj, "id", 0),
                                       vv_json_get_string(wj, "app", "?"),
                                       vv_json_get_string(wj, "title", "(untitled)"));
            }
            g_free(ctx->window_lines);
            ctx->window_lines = g_string_free(lines, FALSE);
        }
        vv_json_free(reply);
    }
    g_free(server_name); g_free(server_fp);
    close_session(tls, raw, buffer);
    return ctx;
}

char *vv_peer_service_deliver(const VvPeer *peer, const char *text, guint32 window) {
    if (!peer->address || !*peer->address) return g_strdup("peer has no address");
    GSocketConnection *raw = NULL; GByteArray *buffer = NULL;
    char *server_name = NULL, *server_fp = NULL, *error = NULL;
    GIOStream *tls = open_session(peer->address, "peer", &raw, &buffer, &server_name, &server_fp, &error);
    if (!tls) return error ? error : g_strdup("could not connect");
    char *result = NULL;
    if (g_strcmp0(server_fp, peer->fingerprint) != 0) {
        result = g_strdup_printf("could not reach %s", peer->name);
    } else {
        VvJson *req = msg("deliver");
        vv_json_object_set(req, "text", vv_json_string(text));
        vv_json_object_set(req, "window", vv_json_number(window));
        VvJson *reply = send_frame(tls, req) ? read_frame(tls, buffer) : NULL;
        if (!reply || g_strcmp0(vv_json_get_string(reply, "t", ""), "ok") != 0)
            result = g_strdup(reply ? vv_json_get_string(reply, "err", "delivery failed") : "delivery failed");
        vv_json_free(reply);
    }
    g_free(server_name); g_free(server_fp);
    close_session(tls, raw, buffer);
    return result;
}

VvMachineContext *vv_peer_service_local_context(const char *machine_name, VvScreenshotSet *set) {
    VvMachineContext *ctx = g_new0(VvMachineContext, 1);
    ctx->machine = g_strdup(machine_name);
    ctx->is_local = true;
    ctx->window_lines = g_strdup("");   /* Wayland: other apps' windows are unknowable */
    ctx->screens = g_ptr_array_new_with_free_func((GDestroyNotify)vv_screenshot_free);
    VvScreenshotSet *owned = NULL;
    if (!set) set = owned = vv_screenshots();
    GPtrArray *attachments = set ? vv_screenshot_set_attachments(set) : NULL;
    for (guint i = 0; attachments && i < attachments->len; i++) {
        VvScreenshot *shot = g_ptr_array_index(attachments, i);
        char *caption = g_strdup_printf("Machine \"%s\" — %s", machine_name, shot->caption);
        g_ptr_array_add(ctx->screens, vv_screenshot_new(shot->jpeg, caption));
    }
    if (attachments) g_ptr_array_unref(attachments);
    if (owned) vv_screenshot_set_unref(owned);
    return ctx;
}
