/* Pure logic for multi-machine peering (docs/multi-machine.md): frame codec
 * and the pairing code. Mirrors macOS/Windows PeerCrypto; sockets and the
 * identity live in src/platform/peerservice.c. */
#pragma once
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include "core/json.h"

#define VV_PEER_MAX_FRAME (32 * 1024 * 1024)
#define VV_PEER_DEFAULT_PORT 47800

/* 4-byte big-endian length + UTF-8 JSON. */
GBytes *vv_peer_frame(VvJson *obj_take);

/* Parses one complete frame from the front of `buffer` and removes it.
 * NULL with *bad=false while incomplete; NULL with *bad=true on garbage. */
VvJson *vv_peer_parse_frame(GByteArray *buffer, bool *bad);

/* 6-digit pairing code over both certificate digests (32 bytes each) and
 * both revealed nonces (32 bytes each). Test vector: docs/multi-machine.md. */
char *vv_peer_pairing_code(const uint8_t *fp_client, const uint8_t *fp_server,
                           const uint8_t *nonce_client, const uint8_t *nonce_server);

char *vv_peer_commitment(const uint8_t *nonce, gsize n);      /* hex SHA-256 */
GBytes *vv_peer_fingerprint(GBytes *certificate_der);          /* 32-byte SHA-256 */
/* 32 cryptographically-random bytes (getrandom/urandom), for pairing nonces. */
void vv_peer_random_nonce(uint8_t out[32]);
char *vv_peer_hex(const uint8_t *bytes, gsize n);
GBytes *vv_peer_unhex(const char *hex);                        /* NULL on bad input */
