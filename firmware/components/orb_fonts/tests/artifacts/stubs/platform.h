#pragma once
/* Host adapters only. SBv2 trust/CRC/RSA encoding comes from installed ESP-IDF. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include "sdkconfig.h"
#include "rom/secure_boot.h"

typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL, ESP_ERR_INVALID_SIZE, ESP_ERR_INVALID_CRC,
       ESP_ERR_INVALID_STATE, ESP_ERR_NO_MEM, ESP_ERR_INVALID_ARG,
       ESP_ERR_NOT_FOUND, ESP_ERR_IMAGE_INVALID };
#define SOC_EFUSE_SECURE_BOOT_KEY_DIGESTS 3
#define ESP_SECURE_BOOT_DIGEST_LEN 32
#define ESP_SECURE_BOOT_KEY_DIGEST_SHA_256_LEN 32
#define ESP_SECURE_BOOT_KEY_DIGEST_LEN 32
#define ESP_SECURE_BOOT_SCHEME 2
#define FLASH_SECTOR_SIZE 4096
#define FLASH_MMAP_ADDR_24BIT_MAX 0x1000000u
#define SPI_FLASH_MMU_PAGE_SIZE 65536u
#define MMAP_ALIGNED_MASK (SPI_FLASH_MMU_PAGE_SIZE - 1)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
typedef enum { SHA2_256 } esp_sha_type;
typedef EVP_MD_CTX *bootloader_sha_handle_t;
#define ESP_LOG_ATTR_TAG(name, value) static const char *name = value
#define ESP_LOGE(tag, ...) do { fprintf(stderr, "%s: ", tag); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define ESP_LOGW ESP_LOGE
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGD ESP_LOGI
#define ESP_LOGV ESP_LOGI
enum { ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY };
typedef struct { uint32_t address, size; } esp_partition_t;
typedef struct { uint32_t offset, size; } esp_partition_pos_t;
typedef struct { uint32_t start_addr, image_len; } esp_image_metadata_t;
typedef struct { uint8_t key_digests[3][32]; unsigned num_digests; } esp_image_sig_public_key_digests_t;
typedef struct { const uint8_t *key_digests[3]; } esp_secure_boot_key_digests_t;
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label);
const esp_partition_t *esp_ota_get_running_partition(void);
esp_err_t esp_partition_read(const esp_partition_t *, size_t, void *, size_t);
esp_err_t esp_image_get_metadata(const esp_partition_pos_t *, esp_image_metadata_t *);
esp_err_t esp_secure_boot_verify_signature(uint32_t, uint32_t);
esp_err_t esp_secure_boot_verify_sbv2_signature_block(const ets_secure_boot_signature_t *, const uint8_t *, uint8_t *);
esp_err_t esp_secure_boot_read_key_digests(esp_secure_boot_key_digests_t *);
static inline bool esp_secure_boot_enabled(void) { return false; }
static inline const char *esp_secure_boot_get_scheme_name(int scheme) { (void)scheme; return "RSA"; }
uint32_t esp_rom_crc32_le(uint32_t, const uint8_t *, size_t);
esp_err_t bootloader_flash_read(size_t, void *, size_t, bool);
const void *bootloader_mmap(uint32_t, uint32_t);
void bootloader_munmap(const void *);
typedef EVP_MD_CTX *bootloader_sha256_handle_t;
bootloader_sha256_handle_t bootloader_sha256_start(void);
void bootloader_sha256_data(bootloader_sha256_handle_t, const void *, size_t);
void bootloader_sha256_finish(bootloader_sha256_handle_t, uint8_t *);
esp_err_t bootloader_sha256_flash_contents(uint32_t, uint32_t, uint8_t *);
uint32_t bootloader_mmap_get_free_pages(void);
esp_err_t artifact_mmap_address_guard(size_t, size_t);

typedef struct { int available; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define portMAX_DELAY UINT32_MAX
#define pdPASS 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DMA 8
typedef void *TaskHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *);
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *);
int xSemaphoreTake(SemaphoreHandle_t, unsigned);
int xSemaphoreGive(SemaphoreHandle_t);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, void *);
void vTaskDelete(void *);
int xTaskCreateWithCaps(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *, unsigned);
void vTaskSuspend(void *);
void vTaskDeleteWithCaps(TaskHandle_t);
static inline unsigned uxTaskGetStackHighWaterMark(void *task) { (void)task; return 0; }
static inline const char *esp_err_to_name(esp_err_t e) { (void)e; return "verification failed"; }
void *heap_caps_malloc(size_t, unsigned);
void heap_caps_free(void *);

typedef int psa_status_t;
typedef EVP_PKEY *psa_key_id_t;
typedef struct { int unused; } psa_key_attributes_t;
typedef struct { EVP_MD_CTX *ctx; } psa_hash_operation_t;
#define PSA_KEY_ATTRIBUTES_INIT {0}
#define PSA_HASH_OPERATION_INIT {0}
#define PSA_SUCCESS 0
#define PSA_ALG_SHA_256 1
#define PSA_ALG_RSA_PSS(hash) (hash)
#define PSA_KEY_USAGE_VERIFY_HASH 1
#define PSA_KEY_TYPE_RSA_PUBLIC_KEY 1
#define PSA_KEY_EXPORT_RSA_PUBLIC_KEY_MAX_SIZE(bits) ((bits) / 8 + 38)
static inline void psa_set_key_usage_flags(psa_key_attributes_t *a, int v) { (void)a; (void)v; }
static inline void psa_set_key_algorithm(psa_key_attributes_t *a, int v) { (void)a; (void)v; }
static inline void psa_set_key_type(psa_key_attributes_t *a, int v) { (void)a; (void)v; }
static inline void psa_reset_key_attributes(psa_key_attributes_t *a) { (void)a; }
psa_status_t psa_import_key(const psa_key_attributes_t *, const uint8_t *, size_t, psa_key_id_t *);
psa_status_t psa_verify_hash(psa_key_id_t, int, const uint8_t *, size_t, const uint8_t *, size_t);
psa_status_t psa_destroy_key(psa_key_id_t);
psa_status_t psa_hash_setup(psa_hash_operation_t *, int);
psa_status_t psa_hash_update(psa_hash_operation_t *, const void *, size_t);
psa_status_t psa_hash_finish(psa_hash_operation_t *, void *, size_t, size_t *);
psa_status_t psa_hash_abort(psa_hash_operation_t *);

#define MBEDTLS_ERR_X509_BAD_INPUT_DATA (-1)
#define MBEDTLS_ASN1_INTEGER 2
#define MBEDTLS_ASN1_CONSTRUCTED 32
#define MBEDTLS_ASN1_SEQUENCE 16
#define MBEDTLS_ASN1_CHK_ADD(length, call) do { int n = (call); if (n < 0) return n; length += n; } while (0)
int mbedtls_asn1_write_raw_buffer(unsigned char **, unsigned char *, const unsigned char *, size_t);
int mbedtls_asn1_write_len(unsigned char **, unsigned char *, size_t);
int mbedtls_asn1_write_tag(unsigned char **, unsigned char *, unsigned char);
