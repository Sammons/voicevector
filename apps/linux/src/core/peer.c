#include "core/peer.h"
#include <string.h>

GBytes *vv_peer_frame(VvJson *obj_take) {
    char *body = vv_json_write(obj_take, 0);
    vv_json_free(obj_take);
    gsize n = strlen(body);
    GByteArray *out = g_byte_array_sized_new(4 + n);
    uint8_t head[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
    g_byte_array_append(out, head, 4);
    g_byte_array_append(out, (const uint8_t *)body, n);
    g_free(body);
    return g_byte_array_free_to_bytes(out);
}

VvJson *vv_peer_parse_frame(GByteArray *buffer, bool *bad) {
    *bad = false;
    if (buffer->len < 4) return NULL;
    gsize n = ((gsize)buffer->data[0] << 24) | ((gsize)buffer->data[1] << 16)
            | ((gsize)buffer->data[2] << 8) | buffer->data[3];
    if (n > VV_PEER_MAX_FRAME) { *bad = true; return NULL; }
    if (buffer->len < 4 + n) return NULL;
    char *body = g_strndup((const char *)buffer->data + 4, n);
    g_byte_array_remove_range(buffer, 0, 4 + n);
    VvJson *obj = vv_json_parse(body, NULL);
    g_free(body);
    if (!obj || obj->type != VV_JSON_OBJECT) { vv_json_free(obj); *bad = true; return NULL; }
    return obj;
}

char *vv_peer_pairing_code(const uint8_t *fp_client, const uint8_t *fp_server,
                           const uint8_t *nonce_client, const uint8_t *nonce_server) {
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(sum, fp_client, 32);
    g_checksum_update(sum, fp_server, 32);
    g_checksum_update(sum, nonce_client, 32);
    g_checksum_update(sum, nonce_server, 32);
    uint8_t digest[32]; gsize len = 32;
    g_checksum_get_digest(sum, digest, &len);
    g_checksum_free(sum);
    guint32 value = ((guint32)digest[0] << 24) | ((guint32)digest[1] << 16)
                  | ((guint32)digest[2] << 8) | digest[3];
    return g_strdup_printf("%06u", value % 1000000u);
}

char *vv_peer_commitment(const uint8_t *nonce, gsize n) {
    return g_compute_checksum_for_data(G_CHECKSUM_SHA256, nonce, n);
}

GBytes *vv_peer_fingerprint(GBytes *der) {
    gsize n; const uint8_t *data = g_bytes_get_data(der, &n);
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(sum, data, n);
    uint8_t digest[32]; gsize len = 32;
    g_checksum_get_digest(sum, digest, &len);
    g_checksum_free(sum);
    return g_bytes_new(digest, 32);
}

char *vv_peer_hex(const uint8_t *bytes, gsize n) {
    GString *s = g_string_sized_new(n * 2);
    for (gsize i = 0; i < n; i++) g_string_append_printf(s, "%02x", bytes[i]);
    return g_string_free(s, FALSE);
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

GBytes *vv_peer_unhex(const char *hex) {
    if (!hex) return NULL;
    gsize n = strlen(hex);
    if (n % 2) return NULL;
    GByteArray *out = g_byte_array_sized_new(n / 2);
    for (gsize i = 0; i < n; i += 2) {
        int hi = hex_digit(hex[i]), lo = hex_digit(hex[i + 1]);
        if (hi < 0 || lo < 0) { g_byte_array_unref(out); return NULL; }
        uint8_t b = (uint8_t)((hi << 4) | lo);
        g_byte_array_append(out, &b, 1);
    }
    return g_byte_array_free_to_bytes(out);
}
