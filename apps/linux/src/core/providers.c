#include "core/providers.h"
#include "core/json.h"
#include "core/log.h"
#include "core/wav.h"
#include <curl/curl.h>
#include <string.h>

static size_t collect(void *data, size_t size, size_t n, void *userp) {
    g_string_append_len((GString *)userp, data, size * n);
    return size * n;
}

static char *url_join(const char *base, const char *path) {
    size_t len = strlen(base);
    while (len && base[len - 1] == '/') len--;
    return g_strdup_printf("%.*s%s", (int)len, base, path);
}

static struct curl_slist *header_list(GPtrArray *headers) {
    struct curl_slist *list = NULL;
    for (guint i = 0; headers && i < headers->len; i++) list = curl_slist_append(list, g_ptr_array_index(headers, i));
    list = curl_slist_append(list, "User-Agent: VoiceVector/" VV_VERSION " (Linux)");
    return list;
}

static bool perform(CURL *curl, struct curl_slist *hdrs, GString *body, char **response, char **error) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    bool ok = rc == CURLE_OK && status >= 200 && status < 300;
    if (!ok) {
        if (rc != CURLE_OK) *error = g_strdup_printf("network error: %s", curl_easy_strerror(rc));
        else *error = g_strdup_printf("HTTP %ld: %.300s", status, body->str);
    }
    return ok;
}

bool vv_http_post_json(const char *url, GPtrArray *headers, const char *payload, char **response, char **error) {
    CURL *curl = curl_easy_init();
    if (!curl) { *error = g_strdup("curl init failed"); return false; }
    GString *body = g_string_new(NULL);
    g_ptr_array_add(headers, g_strdup("Content-Type: application/json"));
    struct curl_slist *hdrs = header_list(headers);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(payload));
    bool ok = perform(curl, hdrs, body, response, error);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    if (ok) *response = g_string_free(body, FALSE); else g_string_free(body, TRUE);
    return ok;
}

bool vv_http_get(const char *url, GPtrArray *headers, char **response, char **error) {
    CURL *curl = curl_easy_init();
    if (!curl) { *error = g_strdup("curl init failed"); return false; }
    GString *body = g_string_new(NULL);
    struct curl_slist *hdrs = header_list(headers);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    bool ok = perform(curl, hdrs, body, response, error);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    if (ok) *response = g_string_free(body, FALSE); else g_string_free(body, TRUE);
    return ok;
}

typedef struct { const char *name; const char *value; GBytes *file; const char *filename; const char *mime; } Part;

static bool post_multipart(const char *url, GPtrArray *headers, Part *parts, int count, char **response, char **error) {
    CURL *curl = curl_easy_init();
    if (!curl) { *error = g_strdup("curl init failed"); return false; }
    curl_mime *mime = curl_mime_init(curl);
    for (int i = 0; i < count; i++) {
        curl_mimepart *part = curl_mime_addpart(mime);
        curl_mime_name(part, parts[i].name);
        if (parts[i].file) {
            gsize n; const void *data = g_bytes_get_data(parts[i].file, &n);
            curl_mime_data(part, data, n);
            curl_mime_filename(part, parts[i].filename);
            curl_mime_type(part, parts[i].mime);
        } else {
            curl_mime_data(part, parts[i].value, CURL_ZERO_TERMINATED);
        }
    }
    GString *body = g_string_new(NULL);
    struct curl_slist *hdrs = header_list(headers);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    bool ok = perform(curl, hdrs, body, response, error);
    curl_slist_free_all(hdrs);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    if (ok) *response = g_string_free(body, FALSE); else g_string_free(body, TRUE);
    return ok;
}

static GPtrArray *auth_headers(const VvProvider *p, const char *api_key) {
    GPtrArray *h = g_ptr_array_new_with_free_func(g_free);
    if (!api_key) api_key = "";
    if (p->kind == VV_KIND_ELEVENLABS) g_ptr_array_add(h, g_strdup_printf("xi-api-key: %s", api_key));
    else g_ptr_array_add(h, g_strdup_printf("Authorization: Bearer %s", api_key));
    return h;
}

static VvJson *parse_or_error(const char *response, char **error) {
    char *perr = NULL;
    VvJson *j = vv_json_parse(response, &perr);
    if (!j) { *error = g_strdup_printf("unreadable response: %s", perr ? perr : "?"); g_free(perr); }
    return j;
}

/* ------------------------------------------------------------ STT */

static bool transcribe_elevenlabs(const VvProvider *p, const char *key, GBytes *wav, const char *filename,
                                  GPtrArray *vocabulary, char **text_out, char **error) {
    char *url = url_join(p->base_url, "/v1/speech-to-text");
    GPtrArray *headers = auth_headers(p, key);
    GPtrArray *parts = g_ptr_array_new();
    Part base[] = {
        { "file", NULL, wav, filename, "audio/wav" },
        { "model_id", p->stt_model, NULL, NULL, NULL },
        { "tag_audio_events", "false", NULL, NULL, NULL },
    };
    Part *all = g_new0(Part, 3 + (vocabulary ? vocabulary->len : 0));
    int n = 0;
    for (int i = 0; i < 3; i++) all[n++] = base[i];
    for (guint i = 0; vocabulary && i < vocabulary->len && i < 100; i++) {
        Part kp = { "keyterms", g_ptr_array_index(vocabulary, i), NULL, NULL, NULL };
        all[n++] = kp;
    }
    char *response = NULL;
    bool ok = post_multipart(url, headers, all, n, &response, error);
    if (ok) {
        VvJson *j = parse_or_error(response, error);
        if (j) { *text_out = g_strdup(vv_json_get_string(j, "text", "")); vv_json_free(j); } else ok = false;
    }
    g_free(all); g_ptr_array_unref(parts); g_free(response); g_ptr_array_unref(headers); g_free(url);
    return ok;
}

static bool transcribe_openai(const VvProvider *p, const char *key, GBytes *wav, const char *filename,
                              GPtrArray *vocabulary, char **text_out, char **error) {
    char *url = url_join(p->base_url, "/audio/transcriptions");
    GPtrArray *headers = auth_headers(p, key);
    char *prompt = NULL;
    if (vocabulary && vocabulary->len) {
        GString *s = g_string_new(NULL);
        for (guint i = 0; i < vocabulary->len; i++) { if (i) g_string_append(s, ", "); g_string_append(s, g_ptr_array_index(vocabulary, i)); }
        prompt = g_string_free(s, FALSE);
    }
    Part parts[] = {
        { "file", NULL, wav, filename, "audio/wav" },
        { "model", p->stt_model, NULL, NULL, NULL },
        { "response_format", "json", NULL, NULL, NULL },
        { "prompt", prompt, NULL, NULL, NULL },
    };
    char *response = NULL;
    bool ok = post_multipart(url, headers, parts, prompt ? 4 : 3, &response, error);
    if (ok) {
        VvJson *j = parse_or_error(response, error);
        if (j) { *text_out = g_strdup(vv_json_get_string(j, "text", "")); vv_json_free(j); } else ok = false;
    }
    g_free(prompt); g_free(response); g_ptr_array_unref(headers); g_free(url);
    return ok;
}

/* Vercel AI Gateway's SDK wire protocol: /v4/ai/transcription-model with the
 * protocol headers (docs/providers.md). */
static bool transcribe_vercel(const VvProvider *p, const char *key, GBytes *wav, char **text_out, char **error) {
    size_t len = strlen(p->base_url);
    while (len && p->base_url[len - 1] == '/') len--;
    char *base = g_strndup(p->base_url, len);
    if (g_str_has_suffix(base, "/v1")) base[strlen(base) - 3] = '\0';
    char *url = g_strconcat(base, "/v4/ai/transcription-model", NULL);
    GPtrArray *headers = auth_headers(p, key);
    g_ptr_array_add(headers, g_strdup("ai-gateway-protocol-version: 0.0.1"));
    g_ptr_array_add(headers, g_strdup("ai-transcription-model-specification-version: 4"));
    gsize n; const guchar *data = g_bytes_get_data(wav, &n);
    char *b64 = g_base64_encode(data, n);
    VvJson *payload = vv_json_object();
    vv_json_object_set(payload, "model", vv_json_string(p->stt_model));
    vv_json_object_set(payload, "audio", vv_json_string(b64));
    vv_json_object_set(payload, "mediaType", vv_json_string("audio/wav"));
    char *body = vv_json_write(payload, 0);
    char *response = NULL;
    bool ok = vv_http_post_json(url, headers, body, &response, error);
    if (ok) {
        VvJson *j = parse_or_error(response, error);
        if (j) { *text_out = g_strdup(vv_json_get_string(j, "text", "")); vv_json_free(j); } else ok = false;
    }
    g_free(response); g_free(body); vv_json_free(payload); g_free(b64); g_ptr_array_unref(headers); g_free(url); g_free(base);
    return ok;
}

bool vv_provider_transcribe(const VvProvider *p, const char *api_key, GBytes *wav, const char *filename,
                            GPtrArray *vocabulary, char **text_out, char **error) {
    *text_out = NULL; *error = NULL;
    switch (p->kind) {
    case VV_KIND_ELEVENLABS: return transcribe_elevenlabs(p, api_key, wav, filename, vocabulary, text_out, error);
    case VV_KIND_VERCEL_GATEWAY: return transcribe_vercel(p, api_key, wav, text_out, error);
    case VV_KIND_OPENAI_COMPATIBLE: return transcribe_openai(p, api_key, wav, filename, vocabulary, text_out, error);
    default:
        *error = g_strdup_printf("%s does not offer transcription — pick another STT provider.", vv_kind_display_name(p->kind));
        return false;
    }
}

/* ----------------------------------------------------------- chat */

static VvJson *image_part(GBytes *jpeg) {
    gsize n; const guchar *data = g_bytes_get_data(jpeg, &n);
    char *b64 = g_base64_encode(data, n);
    char *url = g_strconcat("data:image/jpeg;base64,", b64, NULL);
    VvJson *part = vv_json_object();
    vv_json_object_set(part, "type", vv_json_string("image_url"));
    VvJson *iu = vv_json_object();
    vv_json_object_set(iu, "url", vv_json_string(url));
    vv_json_object_set(iu, "detail", vv_json_string("low"));
    vv_json_object_set(part, "image_url", iu);
    g_free(url); g_free(b64);
    return part;
}

static bool chat_payload(const VvProvider *p, const char *key, VvJson *messages, char **reply_out, char **error) {
    if (!vv_kind_supports_chat(p->kind)) { *error = g_strdup_printf("%s has no chat endpoint", vv_kind_display_name(p->kind)); vv_json_free(messages); return false; }
    char *url = url_join(p->base_url, "/chat/completions");
    VvJson *payload = vv_json_object();
    vv_json_object_set(payload, "model", vv_json_string(p->chat_model));
    vv_json_object_set(payload, "messages", messages);
    vv_json_object_set(payload, "temperature", vv_json_number(0.2));
    char *body = vv_json_write(payload, 0);
    GPtrArray *headers = auth_headers(p, key);
    char *response = NULL;
    bool ok = vv_http_post_json(url, headers, body, &response, error);
    if (ok) {
        VvJson *j = parse_or_error(response, error);
        if (j) {
            VvJson *choices = vv_json_get_array(j, "choices");
            VvJson *first = vv_json_array_get(choices, 0);
            VvJson *message = vv_json_get_object(first, "message");
            const char *content = vv_json_get_string(message, "content", NULL);
            if (content) *reply_out = g_strdup(content);
            else { *error = g_strdup("no choices[0].message.content in chat response"); ok = false; }
            vv_json_free(j);
        } else ok = false;
    }
    g_free(response); g_ptr_array_unref(headers); g_free(body); vv_json_free(payload); g_free(url);
    return ok;
}

static VvJson *message(const char *role, VvJson *content) {
    VvJson *m = vv_json_object();
    vv_json_object_set(m, "role", vv_json_string(role));
    vv_json_object_set(m, "content", content);
    return m;
}

bool vv_provider_chat(const VvProvider *p, const char *api_key, const char *system, const char *user,
                      GBytes *jpeg, char **reply_out, char **error) {
    *reply_out = NULL; *error = NULL;
    VvJson *messages = vv_json_array();
    vv_json_array_add(messages, message("system", vv_json_string(system)));
    if (jpeg) {
        VvJson *parts = vv_json_array();
        VvJson *text = vv_json_object();
        vv_json_object_set(text, "type", vv_json_string("text"));
        vv_json_object_set(text, "text", vv_json_string(user));
        vv_json_array_add(parts, text);
        vv_json_array_add(parts, image_part(jpeg));
        vv_json_array_add(messages, message("user", parts));
    } else {
        vv_json_array_add(messages, message("user", vv_json_string(user)));
    }
    return chat_payload(p, api_key, messages, reply_out, error);
}

bool vv_provider_chat_with_audio(const VvProvider *p, const char *api_key, const char *system, GBytes *wav,
                                 GBytes *jpeg, char **reply_out, char **error) {
    *reply_out = NULL; *error = NULL;
    gsize n; const guchar *data = g_bytes_get_data(wav, &n);
    char *b64 = g_base64_encode(data, n);
    VvJson *parts = vv_json_array();
    VvJson *audio = vv_json_object();
    vv_json_object_set(audio, "type", vv_json_string("input_audio"));
    VvJson *ia = vv_json_object();
    vv_json_object_set(ia, "data", vv_json_string(b64));
    vv_json_object_set(ia, "format", vv_json_string("wav"));
    vv_json_object_set(audio, "input_audio", ia);
    vv_json_array_add(parts, audio);
    if (jpeg) vv_json_array_add(parts, image_part(jpeg));
    g_free(b64);
    VvJson *messages = vv_json_array();
    vv_json_array_add(messages, message("system", vv_json_string(system)));
    vv_json_array_add(messages, message("user", parts));
    return chat_payload(p, api_key, messages, reply_out, error);
}

/* ------------------------------------------------- models & test */

GPtrArray *vv_provider_list_models(const VvProvider *p, const char *api_key, char **error) {
    *error = NULL;
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
    if (!vv_kind_supports_model_listing(p->kind)) return ids;
    char *url = url_join(p->base_url, "/models");
    GPtrArray *headers = auth_headers(p, api_key);
    char *response = NULL;
    if (vv_http_get(url, headers, &response, error)) {
        VvJson *j = parse_or_error(response, error);
        if (j) {
            VvJson *data = vv_json_get_array(j, "data");
            for (guint i = 0; i < vv_json_array_length(data); i++) {
                const char *id = vv_json_get_string(vv_json_array_get(data, i), "id", NULL);
                if (id) g_ptr_array_add(ids, g_strdup(id));
            }
            vv_json_free(j);
        }
    }
    g_free(response); g_ptr_array_unref(headers); g_free(url);
    return ids;
}

bool vv_provider_test(const VvProvider *p, const char *api_key, char **error) {
    *error = NULL;
    if (p->kind == VV_KIND_ELEVENLABS || (p->kind == VV_KIND_OPENAI_COMPATIBLE && !*p->chat_model)) {
        /* Exercise the endpoint the key is scoped to (STT-only keys 401 on /v1/user). */
        GBytes *silent = vv_wav_silent(1.0, VV_WAV_SAMPLE_RATE);
        char *text = NULL;
        bool ok = vv_provider_transcribe(p, api_key, silent, "test.wav", NULL, &text, error);
        g_free(text); g_bytes_unref(silent);
        return ok;
    }
    GPtrArray *models = vv_provider_list_models(p, api_key, error);
    bool ok = *error == NULL;
    g_ptr_array_unref(models);
    return ok;
}
