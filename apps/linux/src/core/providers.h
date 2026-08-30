/* Provider HTTP clients on libcurl — docs/providers.md. All calls are
 * blocking; the app runs them on a worker thread. */
#pragma once
#include <glib.h>
#include <stdbool.h>
#include "core/config.h"
#include "core/cleanup.h"

typedef struct { char *text; } VvTranscription;

/* Errors are returned as g_malloc'd messages in *error (NULL on success). */
bool vv_provider_transcribe(const VvProvider *p, const char *api_key, GBytes *wav, const char *filename,
                            GPtrArray *vocabulary, char **text_out, char **error);
/* screenshots: GPtrArray of VvScreenshot (or NULL / empty); each is sent as a
 * caption text part followed by an image_url part. */
bool vv_provider_chat(const VvProvider *p, const char *api_key, const char *system, const char *user,
                      GPtrArray *screenshots, char **reply_out, char **error);
bool vv_provider_chat_with_audio(const VvProvider *p, const char *api_key, const char *system, GBytes *wav,
                                 GPtrArray *screenshots, char **reply_out, char **error);
/* GET /models → ids (char*). */
GPtrArray *vv_provider_list_models(const VvProvider *p, const char *api_key, char **error);
/* Connectivity test against the endpoint the key is scoped to. */
bool vv_provider_test(const VvProvider *p, const char *api_key, char **error);

/* Low-level: JSON POST / multipart POST / GET, returning body or error. */
bool vv_http_post_json(const char *url, GPtrArray *headers, const char *body, char **response, char **error);
bool vv_http_get(const char *url, GPtrArray *headers, char **response, char **error);
