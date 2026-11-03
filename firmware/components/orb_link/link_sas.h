#pragma once

/*
 * The pairing commitment and code (docs/protocol.md, "Pairing", steps 2 and
 * 5). ESP-free so the host tests can check them against the spec's test
 * vector; the SHA-256 comes from the caller (PSA on the device, CommonCrypto
 * on the Mac).
 */

#include <stddef.h>
#include <stdint.h>

#define LINK_FP_LEN 32      /* SHA-256 of the hub certificate (DER) */
#define LINK_NONCE_LEN 16
#define LINK_COMMIT_LEN 32  /* SHA-256 of nonce_d */

typedef void (*link_sha256_fn)(const void *data, size_t len, uint8_t out[32]);

/* commit_d, which pair.begin carries so that the hub picks nonce_h before it
 * learns nonce_d (pair.reveal). */
void link_commit(link_sha256_fn sha256, const uint8_t nonce_d[LINK_NONCE_LEN], uint8_t out[LINK_COMMIT_LEN]);

/* Writes the 6-digit code, with leading zeros, to `out`. */
void link_sas(link_sha256_fn sha256, const uint8_t cert_fp[LINK_FP_LEN], const uint8_t nonce_d[LINK_NONCE_LEN],
              const uint8_t nonce_h[LINK_NONCE_LEN], char out[7]);
