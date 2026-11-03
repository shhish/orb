#include "link_sas.h"

#include <stdio.h>
#include <string.h>

#define LABEL "orb-pair-v1"

void link_commit(link_sha256_fn sha256, const uint8_t nonce_d[LINK_NONCE_LEN], uint8_t out[LINK_COMMIT_LEN])
{
    sha256(nonce_d, LINK_NONCE_LEN, out);
}

void link_sas(link_sha256_fn sha256, const uint8_t cert_fp[LINK_FP_LEN], const uint8_t nonce_d[LINK_NONCE_LEN],
              const uint8_t nonce_h[LINK_NONCE_LEN], char out[7])
{
    /* "orb-pair-v1" ‖ cert_fp ‖ nonce_d ‖ nonce_h, all raw bytes, no terminator. */
    uint8_t msg[sizeof(LABEL) - 1 + LINK_FP_LEN + 2 * LINK_NONCE_LEN];
    uint8_t *p = msg;
    memcpy(p, LABEL, sizeof(LABEL) - 1);
    p += sizeof(LABEL) - 1;
    memcpy(p, cert_fp, LINK_FP_LEN);
    p += LINK_FP_LEN;
    memcpy(p, nonce_d, LINK_NONCE_LEN);
    p += LINK_NONCE_LEN;
    memcpy(p, nonce_h, LINK_NONCE_LEN);

    uint8_t hash[32];
    sha256(msg, sizeof(msg), hash);
    uint32_t v = (uint32_t)hash[0] << 24 | (uint32_t)hash[1] << 16 | (uint32_t)hash[2] << 8 | hash[3];
    snprintf(out, 7, "%06u", (unsigned)(v % 1000000));
}
