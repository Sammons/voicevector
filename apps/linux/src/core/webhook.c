#include "core/webhook.h"
#include "core/providers.h"
#include "core/json.h"
#include "core/log.h"
#include <curl/curl.h>
#include <unistd.h>

char *vv_webhook_payload(const VvEntry *e, const char *app) {
    VvJson *o = vv_json_object();
    vv_json_object_set(o, "app", vv_json_string(app));
    vv_json_object_set(o, "id", vv_json_string(e->id));
    vv_json_object_set(o, "folder", vv_json_string(e->folder));
    char *date = vv_library_render_date(e->date_unix);
    vv_json_object_set(o, "date", vv_json_string(date));
    g_free(date);
    vv_json_object_set(o, "duration", vv_json_number(e->duration));
    vv_json_object_set(o, "raw", vv_json_string(e->raw));
    vv_json_object_set(o, "cleaned", vv_json_string(e->cleaned));
    vv_json_object_set(o, "stt", vv_json_string(e->stt_label));
    vv_json_object_set(o, "cleanup", vv_json_string(e->cleanup_label));
    char *s = vv_json_write(o, 0);
    vv_json_free(o);
    return s;
}

static size_t discard(void *d, size_t s, size_t n, void *u) { return s * n; }

static bool attempt(const VvEntry *e, const char *audio_path, const VvWebhook *config, char **error) {
    CURL *curl = curl_easy_init();
    if (!curl) { *error = g_strdup("curl init failed"); return false; }
    char *payload = vv_webhook_payload(e, "voicevector-linux");
    struct curl_slist *hdrs = NULL;
    curl_mime *mime = NULL;
    GBytes *audio = NULL;
    if (config->include_audio && audio_path && g_file_test(audio_path, G_FILE_TEST_EXISTS)) {
        char *data = NULL; gsize n = 0;
        if (g_file_get_contents(audio_path, &data, &n, NULL)) {
            audio = g_bytes_new_take(data, n);
            mime = curl_mime_init(curl);
            curl_mimepart *part = curl_mime_addpart(mime);
            curl_mime_name(part, "payload");
            curl_mime_data(part, payload, CURL_ZERO_TERMINATED);
            part = curl_mime_addpart(mime);
            curl_mime_name(part, "audio");
            curl_mime_data(part, g_bytes_get_data(audio, NULL), n);
            char *fname = g_strconcat(e->id, ".wav", NULL);
            curl_mime_filename(part, fname);
            g_free(fname);
            curl_mime_type(part, "audio/wav");
            curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        }
    }
    if (!mime) {
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    }
    curl_easy_setopt(curl, CURLOPT_URL, config->url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    bool ok = rc == CURLE_OK && status >= 200 && status < 300;
    if (!ok) *error = rc != CURLE_OK ? g_strdup(curl_easy_strerror(rc)) : g_strdup_printf("HTTP %ld", status);
    if (mime) curl_mime_free(mime);
    if (audio) g_bytes_unref(audio);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    g_free(payload);
    return ok;
}

bool vv_webhook_send(const VvEntry *e, const char *audio_path, const VvWebhook *config) {
    if (!config->enabled || !config->url || !g_str_has_prefix(config->url, "http")) return false;
    for (int n = 1; n <= 3; n++) {
        char *error = NULL;
        if (attempt(e, audio_path, config, &error)) return true;
        vv_log_error("Webhook attempt %d for %s failed: %s", n, e->id, error);
        g_free(error);
        if (n < 3) sleep(2 * n);
    }
    return false;
}
