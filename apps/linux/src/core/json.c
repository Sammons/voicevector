#include "core/json.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static VvJson *make(VvJsonType type) {
    VvJson *j = g_new0(VvJson, 1);
    j->type = type;
    return j;
}

VvJson *vv_json_null(void) { return make(VV_JSON_NULL); }
VvJson *vv_json_bool(bool v) { VvJson *j = make(VV_JSON_BOOL); j->boolean = v; return j; }
VvJson *vv_json_number(double v) { VvJson *j = make(VV_JSON_NUMBER); j->number = v; return j; }
VvJson *vv_json_string(const char *v) {
    if (!v) return vv_json_null();
    VvJson *j = make(VV_JSON_STRING); j->string = g_strdup(v); return j;
}
VvJson *vv_json_array(void) {
    VvJson *j = make(VV_JSON_ARRAY);
    j->array = g_ptr_array_new_with_free_func((GDestroyNotify)vv_json_free);
    return j;
}
VvJson *vv_json_object(void) {
    VvJson *j = make(VV_JSON_OBJECT);
    j->keys = g_ptr_array_new_with_free_func(g_free);
    j->values = g_ptr_array_new_with_free_func((GDestroyNotify)vv_json_free);
    return j;
}

void vv_json_free(VvJson *j) {
    if (!j) return;
    g_free(j->string);
    if (j->array) g_ptr_array_unref(j->array);
    if (j->keys) g_ptr_array_unref(j->keys);
    if (j->values) g_ptr_array_unref(j->values);
    g_free(j);
}

void vv_json_array_add(VvJson *a, VvJson *v) { g_ptr_array_add(a->array, v ? v : vv_json_null()); }

static int find_key(const VvJson *o, const char *key) {
    if (!o || o->type != VV_JSON_OBJECT) return -1;
    for (guint i = 0; i < o->keys->len; i++)
        if (strcmp((const char *)g_ptr_array_index(o->keys, i), key) == 0) return (int)i;
    return -1;
}

void vv_json_object_set(VvJson *o, const char *key, VvJson *v) {
    if (!v) v = vv_json_null();
    int i = find_key(o, key);
    if (i >= 0) {
        vv_json_free((VvJson *)g_ptr_array_index(o->values, i));
        g_ptr_array_index(o->values, i) = v;
        return;
    }
    g_ptr_array_add(o->keys, g_strdup(key));
    g_ptr_array_add(o->values, v);
}

VvJson *vv_json_object_get(const VvJson *o, const char *key) {
    int i = find_key(o, key);
    return i >= 0 ? (VvJson *)g_ptr_array_index(o->values, i) : NULL;
}

guint vv_json_array_length(const VvJson *a) { return (a && a->type == VV_JSON_ARRAY) ? a->array->len : 0; }
VvJson *vv_json_array_get(const VvJson *a, guint i) {
    return (a && a->type == VV_JSON_ARRAY && i < a->array->len) ? (VvJson *)g_ptr_array_index(a->array, i) : NULL;
}

const char *vv_json_get_string(const VvJson *o, const char *key, const char *fallback) {
    VvJson *v = vv_json_object_get(o, key);
    return (v && v->type == VV_JSON_STRING) ? v->string : fallback;
}
double vv_json_get_number(const VvJson *o, const char *key, double fallback) {
    VvJson *v = vv_json_object_get(o, key);
    return (v && v->type == VV_JSON_NUMBER) ? v->number : fallback;
}
bool vv_json_get_bool(const VvJson *o, const char *key, bool fallback) {
    VvJson *v = vv_json_object_get(o, key);
    return (v && v->type == VV_JSON_BOOL) ? v->boolean : fallback;
}
VvJson *vv_json_get_object(const VvJson *o, const char *key) {
    VvJson *v = vv_json_object_get(o, key);
    return (v && v->type == VV_JSON_OBJECT) ? v : NULL;
}
VvJson *vv_json_get_array(const VvJson *o, const char *key) {
    VvJson *v = vv_json_object_get(o, key);
    return (v && v->type == VV_JSON_ARRAY) ? v : NULL;
}

/* ---------------------------------------------------------------- parser */

typedef struct { const char *s; const char *p; char *error; } Parser;

static void fail(Parser *ps, const char *what) {
    if (!ps->error) ps->error = g_strdup_printf("%s at offset %ld", what, (long)(ps->p - ps->s));
}
static void skip_ws(Parser *ps) { while (*ps->p == ' ' || *ps->p == '\n' || *ps->p == '\r' || *ps->p == '\t') ps->p++; }
static VvJson *parse_value(Parser *ps);

static bool parse_hex4(Parser *ps, guint32 *out) {
    guint32 v = 0;
    for (int i = 0; i < 4; i++) {
        char c = ps->p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (guint32)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (guint32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (guint32)(c - 'A' + 10);
        else return false;
    }
    ps->p += 4;
    *out = v;
    return true;
}

static char *parse_string_raw(Parser *ps) {
    if (*ps->p != '"') { fail(ps, "expected string"); return NULL; }
    ps->p++;
    GString *out = g_string_new(NULL);
    while (*ps->p && *ps->p != '"') {
        if (*ps->p == '\\') {
            ps->p++;
            char c = *ps->p++;
            switch (c) {
            case '"': g_string_append_c(out, '"'); break;
            case '\\': g_string_append_c(out, '\\'); break;
            case '/': g_string_append_c(out, '/'); break;
            case 'b': g_string_append_c(out, '\b'); break;
            case 'f': g_string_append_c(out, '\f'); break;
            case 'n': g_string_append_c(out, '\n'); break;
            case 'r': g_string_append_c(out, '\r'); break;
            case 't': g_string_append_c(out, '\t'); break;
            case 'u': {
                guint32 cp;
                if (!parse_hex4(ps, &cp)) { fail(ps, "bad \\u escape"); g_string_free(out, TRUE); return NULL; }
                if (cp >= 0xD800 && cp <= 0xDBFF && ps->p[0] == '\\' && ps->p[1] == 'u') {
                    ps->p += 2;
                    guint32 lo;
                    if (!parse_hex4(ps, &lo)) { fail(ps, "bad surrogate"); g_string_free(out, TRUE); return NULL; }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                g_string_append_unichar(out, (gunichar)cp);
                break;
            }
            default: fail(ps, "bad escape"); g_string_free(out, TRUE); return NULL;
            }
        } else {
            g_string_append_c(out, *ps->p++);
        }
    }
    if (*ps->p != '"') { fail(ps, "unterminated string"); g_string_free(out, TRUE); return NULL; }
    ps->p++;
    return g_string_free(out, FALSE);
}

static VvJson *parse_value(Parser *ps) {
    skip_ws(ps);
    switch (*ps->p) {
    case '{': {
        ps->p++;
        VvJson *o = vv_json_object();
        skip_ws(ps);
        if (*ps->p == '}') { ps->p++; return o; }
        for (;;) {
            skip_ws(ps);
            char *key = parse_string_raw(ps);
            if (!key) { vv_json_free(o); return NULL; }
            skip_ws(ps);
            if (*ps->p != ':') { fail(ps, "expected ':'"); g_free(key); vv_json_free(o); return NULL; }
            ps->p++;
            VvJson *v = parse_value(ps);
            if (!v) { g_free(key); vv_json_free(o); return NULL; }
            vv_json_object_set(o, key, v);
            g_free(key);
            skip_ws(ps);
            if (*ps->p == ',') { ps->p++; continue; }
            if (*ps->p == '}') { ps->p++; return o; }
            fail(ps, "expected ',' or '}'"); vv_json_free(o); return NULL;
        }
    }
    case '[': {
        ps->p++;
        VvJson *a = vv_json_array();
        skip_ws(ps);
        if (*ps->p == ']') { ps->p++; return a; }
        for (;;) {
            VvJson *v = parse_value(ps);
            if (!v) { vv_json_free(a); return NULL; }
            vv_json_array_add(a, v);
            skip_ws(ps);
            if (*ps->p == ',') { ps->p++; continue; }
            if (*ps->p == ']') { ps->p++; return a; }
            fail(ps, "expected ',' or ']'"); vv_json_free(a); return NULL;
        }
    }
    case '"': {
        char *s = parse_string_raw(ps);
        if (!s) return NULL;
        VvJson *v = vv_json_string(s);
        g_free(s);
        return v;
    }
    case 't': if (strncmp(ps->p, "true", 4) == 0) { ps->p += 4; return vv_json_bool(true); } break;
    case 'f': if (strncmp(ps->p, "false", 5) == 0) { ps->p += 5; return vv_json_bool(false); } break;
    case 'n': if (strncmp(ps->p, "null", 4) == 0) { ps->p += 4; return vv_json_null(); } break;
    default: {
        if (*ps->p == '-' || (*ps->p >= '0' && *ps->p <= '9')) {
            char *end = NULL;
            double d = g_ascii_strtod(ps->p, &end);
            if (end == ps->p) break;
            ps->p = end;
            return vv_json_number(d);
        }
    }
    }
    fail(ps, "unexpected token");
    return NULL;
}

VvJson *vv_json_parse(const char *text, char **error) {
    Parser ps = { text, text, NULL };
    VvJson *v = parse_value(&ps);
    if (v) {
        skip_ws(&ps);
        if (*ps.p) { fail(&ps, "trailing characters"); vv_json_free(v); v = NULL; }
    }
    if (error) *error = ps.error; else g_free(ps.error);
    return v;
}

/* ---------------------------------------------------------------- writer */

static void write_string(GString *out, const char *s) {
    g_string_append_c(out, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': g_string_append(out, "\\\""); break;
        case '\\': g_string_append(out, "\\\\"); break;
        case '\n': g_string_append(out, "\\n"); break;
        case '\r': g_string_append(out, "\\r"); break;
        case '\t': g_string_append(out, "\\t"); break;
        case '\b': g_string_append(out, "\\b"); break;
        case '\f': g_string_append(out, "\\f"); break;
        default:
            if (*p < 0x20) g_string_append_printf(out, "\\u%04x", *p);
            else g_string_append_c(out, (char)*p);
        }
    }
    g_string_append_c(out, '"');
}

static void newline(GString *out, int indent, int depth) {
    if (indent <= 0) return;
    g_string_append_c(out, '\n');
    for (int i = 0; i < indent * depth; i++) g_string_append_c(out, ' ');
}

static void write_value(GString *out, const VvJson *j, int indent, int depth) {
    switch (j->type) {
    case VV_JSON_NULL: g_string_append(out, "null"); break;
    case VV_JSON_BOOL: g_string_append(out, j->boolean ? "true" : "false"); break;
    case VV_JSON_NUMBER: {
        if (j->number == floor(j->number) && fabs(j->number) < 1e15) {
            g_string_append_printf(out, "%.0f", j->number);
        } else {
            char buf[G_ASCII_DTOSTR_BUF_SIZE];
            g_ascii_dtostr(buf, sizeof buf, j->number);
            g_string_append(out, buf);
        }
        break;
    }
    case VV_JSON_STRING: write_string(out, j->string); break;
    case VV_JSON_ARRAY:
        g_string_append_c(out, '[');
        for (guint i = 0; i < j->array->len; i++) {
            if (i) g_string_append_c(out, ',');
            newline(out, indent, depth + 1);
            write_value(out, (VvJson *)g_ptr_array_index(j->array, i), indent, depth + 1);
        }
        if (j->array->len) newline(out, indent, depth);
        g_string_append_c(out, ']');
        break;
    case VV_JSON_OBJECT:
        g_string_append_c(out, '{');
        for (guint i = 0; i < j->keys->len; i++) {
            if (i) g_string_append_c(out, ',');
            newline(out, indent, depth + 1);
            write_string(out, (const char *)g_ptr_array_index(j->keys, i));
            g_string_append(out, indent > 0 ? ": " : ":");
            write_value(out, (VvJson *)g_ptr_array_index(j->values, i), indent, depth + 1);
        }
        if (j->keys->len) newline(out, indent, depth);
        g_string_append_c(out, '}');
        break;
    }
}

char *vv_json_write(const VvJson *j, int indent) {
    GString *out = g_string_new(NULL);
    write_value(out, j, indent, 0);
    return g_string_free(out, FALSE);
}
