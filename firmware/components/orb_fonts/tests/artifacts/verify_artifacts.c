/* Execute Orb's device port and installed IDF SBv2 trust/CRC/RSA code unchanged.
 * OpenSSL replaces hardware/PSA primitives; fake flash contains exact input files.
 * The metadata adapter mirrors the ESP app header/segment/checksum/hash lengths.
 */
#include "platform.h"
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include "orb_fonts_private.h"
#include "orb_assets.h"

static uint8_t *s_flash;
static const size_t s_flash_size = 32u * 1024u * 1024u;
static const esp_partition_t s_app = {.address = 0x20000, .size = 0x600000};
static const esp_partition_t s_assets = {.address = 0xc20000, .size = 0x800000};
static const void *s_mapping;
const orb_fonts_table_t orb_fonts_table = {0};

const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    (void)type; (void)subtype;
    return strcmp(label, "assets") == 0 ? &s_assets : NULL;
}
const esp_partition_t *esp_ota_get_running_partition(void) { return &s_app; }
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t len)
{
    if (off > p->size || len > p->size - off) return ESP_ERR_INVALID_SIZE;
    memcpy(dst, s_flash + p->address + off, len);
    return ESP_OK;
}
esp_err_t bootloader_flash_read(size_t addr, void *dst, size_t len, bool decrypt)
{
    (void)decrypt;
    if (addr > s_flash_size || len > s_flash_size - addr) return ESP_ERR_INVALID_SIZE;
    memcpy(dst, s_flash + addr, len);
    return ESP_OK;
}
const void *bootloader_mmap(uint32_t addr, uint32_t len)
{
    if (s_mapping || addr > s_flash_size || len > s_flash_size - addr ||
        artifact_mmap_address_guard(addr, len) != ESP_OK) return NULL;
    s_mapping = s_flash + addr;
    return s_mapping;
}
void bootloader_munmap(const void *ptr) { assert(ptr == s_mapping); s_mapping = NULL; }
esp_err_t esp_image_get_metadata(const esp_partition_pos_t *p, esp_image_metadata_t *m)
{
    uint8_t *d = s_flash + p->offset;
    if (d[0] != 0xe9 || d[1] > 16) return ESP_ERR_IMAGE_INVALID;
    size_t pos = 24;
    for (unsigned i = 0; i < d[1]; i++) {
        if (pos > p->size - 8) return ESP_ERR_IMAGE_INVALID;
        uint32_t len = orb_assets_rd32(d + pos + 4);
        if (len > p->size - pos - 8) return ESP_ERR_IMAGE_INVALID;
        pos += 8 + len;
    }
    pos = (pos + 1 + 15) & ~15u;
    if (d[23]) pos += 32;
    if (pos > p->size) return ESP_ERR_IMAGE_INVALID;
    *m = (esp_image_metadata_t){.start_addr = p->offset, .image_len = pos};
    return ESP_OK;
}
esp_err_t esp_secure_boot_read_key_digests(esp_secure_boot_key_digests_t *d)
{
    (void)d;
    return ESP_ERR_NOT_FOUND; /* This board has no fused trust; use running app. */
}
uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (unsigned j = 0; j < 8; j++) crc = (crc >> 1) ^ ((0u - (crc & 1)) & 0xedb88320);
    }
    return ~crc;
}
bootloader_sha256_handle_t bootloader_sha256_start(void)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    assert(ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1);
    return ctx;
}
void bootloader_sha256_data(bootloader_sha256_handle_t ctx, const void *data, size_t len)
{
    assert(EVP_DigestUpdate(ctx, data, len) == 1);
}
void bootloader_sha256_finish(bootloader_sha256_handle_t ctx, uint8_t *digest)
{
    if (digest) { unsigned len; assert(EVP_DigestFinal_ex(ctx, digest, &len) == 1 && len == 32); }
    EVP_MD_CTX_free(ctx);
}
uint32_t bootloader_mmap_get_free_pages(void) { return 128; }
psa_status_t psa_import_key(const psa_key_attributes_t *a, const uint8_t *data, size_t len, psa_key_id_t *key)
{
    (void)a;
    const uint8_t *end = data;
    *key = d2i_PublicKey(EVP_PKEY_RSA, NULL, &end, len);
    return *key && end == data + len ? PSA_SUCCESS : -1;
}
psa_status_t psa_verify_hash(psa_key_id_t key, int alg, const uint8_t *hash, size_t hash_len,
                              const uint8_t *sig, size_t sig_len)
{
    assert(alg == PSA_ALG_RSA_PSS(PSA_ALG_SHA_256));
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(key, NULL);
    int ok = ctx && EVP_PKEY_verify_init(ctx) == 1 &&
             EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PSS_PADDING) == 1 &&
             EVP_PKEY_CTX_set_signature_md(ctx, EVP_sha256()) == 1 &&
             EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha256()) == 1 &&
             EVP_PKEY_CTX_set_rsa_pss_saltlen(ctx, 32) == 1 &&
             EVP_PKEY_verify(ctx, sig, sig_len, hash, hash_len) == 1;
    EVP_PKEY_CTX_free(ctx);
    return ok ? PSA_SUCCESS : -1;
}
psa_status_t psa_destroy_key(psa_key_id_t key) { EVP_PKEY_free(key); return PSA_SUCCESS; }
bool ets_rsa_pss_verify(const ets_rsa_pubkey_t *key, const uint8_t *sig,
                       const uint8_t *digest, uint8_t *verified)
{
    /* calculate_image_public_key_digests(false, ...) never uses ROM RSA. */
    (void)key; (void)sig; (void)digest; (void)verified;
    assert(false);
    return false;
}
psa_status_t psa_hash_setup(psa_hash_operation_t *h, int alg)
{
    assert(alg == PSA_ALG_SHA_256);
    h->ctx = bootloader_sha256_start();
    return PSA_SUCCESS;
}
psa_status_t psa_hash_update(psa_hash_operation_t *h, const void *data, size_t len)
{
    return EVP_DigestUpdate(h->ctx, data, len) == 1 ? PSA_SUCCESS : -1;
}
psa_status_t psa_hash_finish(psa_hash_operation_t *h, void *digest, size_t len, size_t *actual)
{
    assert(len >= 32);
    bootloader_sha256_finish(h->ctx, digest);
    h->ctx = NULL;
    *actual = 32;
    return PSA_SUCCESS;
}
psa_status_t psa_hash_abort(psa_hash_operation_t *h)
{
    EVP_MD_CTX_free(h->ctx); h->ctx = NULL; return PSA_SUCCESS;
}
int mbedtls_asn1_write_raw_buffer(unsigned char **p, unsigned char *start,
                                  const unsigned char *src, size_t len)
{
    if ((size_t)(*p - start) < len) return -1;
    *p -= len;
    memcpy(*p, src, len);
    return len;
}
int mbedtls_asn1_write_len(unsigned char **p, unsigned char *start, size_t len)
{
    if (len < 128) return mbedtls_asn1_write_tag(p, start, len);
    unsigned char buf[sizeof(size_t) + 1];
    unsigned count = 0;
    for (size_t n = len; n; n >>= 8) buf[sizeof(buf) - 1 - count++] = n;
    buf[sizeof(buf) - count - 1] = 128 | count;
    return mbedtls_asn1_write_raw_buffer(p, start, buf + sizeof(buf) - count - 1, count + 1);
}
int mbedtls_asn1_write_tag(unsigned char **p, unsigned char *start, unsigned char tag)
{
    return mbedtls_asn1_write_raw_buffer(p, start, &tag, 1);
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) { s->available = 1; return s; }
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *s) { s->available = 0; return s; }
int xSemaphoreTake(SemaphoreHandle_t s, unsigned timeout) { (void)timeout; assert(s->available); s->available = 0; return 1; }
int xSemaphoreGive(SemaphoreHandle_t s) { assert(!s->available); s->available = 1; return 1; }
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle)
{
    (void)name; (void)stack; (void)priority; (void)handle; fn(arg); return pdPASS;
}
void vTaskDelete(void *handle) { assert(!handle); }
int xTaskCreateWithCaps(void (*fn)(void *), const char *name, unsigned stack, void *arg,
                        unsigned priority, TaskHandle_t *handle, unsigned caps)
{
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    *handle = (void *)1;
    return xTaskCreate(fn, name, stack, arg, priority, NULL);
}
void vTaskSuspend(void *handle) { assert(!handle); }
void vTaskDeleteWithCaps(TaskHandle_t handle) { assert(handle == (void *)1); }
void *heap_caps_malloc(size_t len, unsigned caps) { assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); return malloc(len); }
void heap_caps_free(void *ptr) { free(ptr); }

static size_t load(const char *path, uint32_t address, size_t limit)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long len = ftell(file);
    assert(len > 0 && (size_t)len <= limit && fseek(file, 0, SEEK_SET) == 0);
    assert(fread(s_flash + address, 1, len, file) == (size_t)len);
    assert(fclose(file) == 0);
    return len;
}
int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4) { fprintf(stderr, "usage: verify_artifacts app.signed.bin assets.signed.bin [legacy|app]\n"); return 2; }
    s_flash = malloc(s_flash_size);
    assert(s_flash);
    memset(s_flash, 255, s_flash_size);
    size_t app_len = load(argv[1], s_app.address, s_app.size);
    size_t len = load(argv[2], s_assets.address, s_assets.size);
    orb_assets_info_t info;
    esp_err_t result;
    if (argc == 4 && strcmp(argv[3], "app") == 0) {
        result = esp_secure_boot_verify_signature(s_app.address, app_len - 4096);
    } else if (argc == 4) {
        result = esp_secure_boot_verify_signature(s_assets.address, orb_assets_rd32(s_flash + s_assets.address + 20));
    } else {
        result = orb_assets_verify_partition(&s_assets, len, &info);
    }
    printf("device assets verification: %d\n", result);
    free(s_flash);
    return result == ESP_OK ? 0 : 1;
}
