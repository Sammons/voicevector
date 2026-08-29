/* Folder webhook delivery — payload per docs/webhook-payload.md. */
#pragma once
#include <glib.h>
#include <stdbool.h>
#include "core/config.h"
#include "core/library.h"

char *vv_webhook_payload(const VvEntry *entry, const char *app);
/* Blocking, with retries; run on a worker thread. */
bool vv_webhook_send(const VvEntry *entry, const char *audio_path, const VvWebhook *config);
