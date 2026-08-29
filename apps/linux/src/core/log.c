#include "core/log.h"
#include <stdio.h>
#include <stdarg.h>

static GPtrArray *ring;
G_LOCK_DEFINE_STATIC(ring_lock);

void vv_log_info(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    g_log("voicevector", G_LOG_LEVEL_INFO, "%s", msg);
    g_free(msg);
}

void vv_log_error(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    fprintf(stderr, "voicevector: %s\n", msg);
    G_LOCK(ring_lock);
    if (!ring) ring = g_ptr_array_new_with_free_func(g_free);
    GDateTime *now = g_date_time_new_now_utc();
    char *stamp = g_date_time_format_iso8601(now);
    g_date_time_unref(now);
    g_ptr_array_add(ring, g_strdup_printf("%s  %s", stamp, msg));
    g_free(stamp);
    while (ring->len > 200) g_ptr_array_remove_index(ring, 0);
    G_UNLOCK(ring_lock);
    g_free(msg);
}

GPtrArray *vv_log_recent_errors(void) {
    GPtrArray *copy = g_ptr_array_new_with_free_func(g_free);
    G_LOCK(ring_lock);
    if (ring) for (guint i = 0; i < ring->len; i++) g_ptr_array_add(copy, g_strdup(g_ptr_array_index(ring, i)));
    G_UNLOCK(ring_lock);
    return copy;
}
