/* The pairing commitment and code against docs/protocol.md's test vector, hashed with CommonCrypto. */
#include <CommonCrypto/CommonDigest.h>
#include <stdint.h>

#include "link_proto.h"
#include "link_sas.h"
#include "test.h"

static void sha256(const void *data, size_t len, uint8_t out[32])
{
    CC_SHA256(data, (CC_LONG)len, out);
}

/* A stand-in hash whose first four bytes are chosen, to pin down the arithmetic. */
static uint8_t s_fixed[32];

static void fixed_hash(const void *data, size_t len, uint8_t out[32])
{
    (void)data;
    (void)len;
    memcpy(out, s_fixed, 32);
}

void test_sas(void)
{
    uint8_t fp[32], nonce_d[16], nonce_h[16];
    char hex[65], code[7];

    sha256("test-cert", 9, fp);
    link_hex(fp, 32, hex);
    CHECK_STR(hex, "ac978e63f273ba9ee1132bfecc103c767c52512fd5b065cc0ee224f9ec49d6a2");
    CHECK(link_unhex("000102030405060708090a0b0c0d0e0f", nonce_d, 16));
    CHECK(link_unhex("101112131415161718191a1b1c1d1e1f", nonce_h, 16));

    uint8_t commit[LINK_COMMIT_LEN];
    link_commit(sha256, nonce_d, commit);
    link_hex(commit, sizeof(commit), hex);
    CHECK_STR(hex, "be45cb2605bf36bebde684841a28f0fd43c69850a3dce5fedba69928ee3a8991");

    /* The vector's intermediate hash, built the way the spec spells it out. */
    uint8_t msg[11 + 32 + 16 + 16], hash[32];
    memcpy(msg, "orb-pair-v1", 11);
    memcpy(msg + 11, fp, 32);
    memcpy(msg + 43, nonce_d, 16);
    memcpy(msg + 59, nonce_h, 16);
    sha256(msg, sizeof(msg), hash);
    link_hex(hash, 32, hex);
    CHECK_STR(hex, "acbc0186c13758df7488335a8e95f3eba38e7818a36dad14e3be0940ebc3f554");

    link_sas(sha256, fp, nonce_d, nonce_h, code);
    CHECK_STR(code, "002310");

    /* A man in the middle's certificate, or either nonce changed, gives another code. */
    char other[7];
    fp[31] ^= 1;
    link_sas(sha256, fp, nonce_d, nonce_h, other);
    CHECK(strcmp(other, code) != 0);
    fp[31] ^= 1;
    nonce_h[0] ^= 1;
    link_sas(sha256, fp, nonce_d, nonce_h, other);
    CHECK(strcmp(other, code) != 0);

    /* Big-endian, modulo a million, zero-padded to six digits. */
    memcpy(s_fixed, (uint8_t[]){ 0x00, 0x00, 0x00, 0x07, 0xff }, 5);
    link_sas(fixed_hash, fp, nonce_d, nonce_h, code);
    CHECK_STR(code, "000007");
    memcpy(s_fixed, (uint8_t[]){ 0xff, 0xff, 0xff, 0xff }, 4);
    link_sas(fixed_hash, fp, nonce_d, nonce_h, code);
    CHECK_STR(code, "967295");   /* 4294967295 mod 1000000 */
    memcpy(s_fixed, (uint8_t[]){ 0x00, 0x0f, 0x42, 0x40 }, 4);
    link_sas(fixed_hash, fp, nonce_d, nonce_h, code);
    CHECK_STR(code, "000000");   /* exactly 1000000 */
}
