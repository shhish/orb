#pragma once
/* SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
/* Layout from ESP-IDF esp32s3/rom/{rsa_pss,secure_boot}.h (Apache-2.0).
 * Only ABI types are mirrored; all signature/trust decisions use IDF sources. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct { uint8_t n[384]; uint32_t e; uint8_t rinv[384]; uint32_t mdash; } ets_rsa_pubkey_t;
typedef struct { uint8_t magic_byte, version, _reserved1, _reserved2;
 uint8_t image_digest[32]; ets_rsa_pubkey_t key; uint8_t signature[384];
 uint32_t block_crc; uint8_t _padding[16]; } ets_secure_boot_sig_block_t;
#define SECURE_BOOT_NUM_BLOCKS 3
#define ETS_SECURE_BOOT_V2_SIGNATURE_MAGIC 0xe7
#define CRC_SIGN_BLOCK_LEN 1196
typedef struct { ets_secure_boot_sig_block_t block[3]; uint8_t _padding[448]; } ets_secure_boot_signature_t;
_Static_assert(sizeof(ets_secure_boot_sig_block_t) == 1216, "SBv2 block ABI");
_Static_assert(sizeof(ets_secure_boot_signature_t) == 4096, "SBv2 sector ABI");
bool ets_rsa_pss_verify(const ets_rsa_pubkey_t *, const uint8_t *, const uint8_t *, uint8_t *);
