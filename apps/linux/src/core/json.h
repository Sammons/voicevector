/* Minimal JSON value tree on GLib — parse, build, write. Tolerant readers
 * (missing keys → defaults) live on top via the vv_json_get_* helpers. */
#pragma once
#include <glib.h>
#include <stdbool.h>

typedef enum { VV_JSON_NULL, VV_JSON_BOOL, VV_JSON_NUMBER, VV_JSON_STRING, VV_JSON_ARRAY, VV_JSON_OBJECT } VvJsonType;

typedef struct VvJson VvJson;
struct VvJson {
    VvJsonType type;
    bool boolean;
    double number;
    char *string;
    GPtrArray *array;      /* VvJson* */
    GPtrArray *keys;       /* char* — insertion order preserved */
    GPtrArray *values;     /* VvJson* */
};

VvJson *vv_json_null(void);
VvJson *vv_json_bool(bool value);
VvJson *vv_json_number(double value);
VvJson *vv_json_string(const char *value);   /* NULL → JSON null */
VvJson *vv_json_array(void);
VvJson *vv_json_object(void);
void vv_json_free(VvJson *json);

void vv_json_array_add(VvJson *array, VvJson *value);          /* takes ownership */
void vv_json_object_set(VvJson *object, const char *key, VvJson *value); /* takes ownership; replaces */
VvJson *vv_json_object_get(const VvJson *object, const char *key);      /* borrowed; NULL if absent */
guint vv_json_array_length(const VvJson *array);
VvJson *vv_json_array_get(const VvJson *array, guint index);            /* borrowed */

/* Tolerant accessors: wrong type or missing → fallback. */
const char *vv_json_get_string(const VvJson *object, const char *key, const char *fallback);
double vv_json_get_number(const VvJson *object, const char *key, double fallback);
bool vv_json_get_bool(const VvJson *object, const char *key, bool fallback);
VvJson *vv_json_get_object(const VvJson *object, const char *key);   /* borrowed or NULL */
VvJson *vv_json_get_array(const VvJson *object, const char *key);    /* borrowed or NULL */

/* Parse; returns NULL and sets *error (g_free) on malformed input. */
VvJson *vv_json_parse(const char *text, char **error);
/* Serialize. Objects keep insertion order; `indent` > 0 pretty-prints. */
char *vv_json_write(const VvJson *json, int indent);
