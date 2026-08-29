#pragma once
#include <glib.h>
void vv_log_info(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
void vv_log_error(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
/* Last ~200 error lines, newest last (for Settings → General). */
GPtrArray *vv_log_recent_errors(void);
