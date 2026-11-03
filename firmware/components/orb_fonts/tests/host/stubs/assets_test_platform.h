#pragma once

#include <stddef.h>
#include <stdint.h>

#define CONFIG_SECURE_SIGNED_ON_UPDATE 1
#define CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME 1
#define CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM 1
#define CONFIG_SPIRAM_XIP_FROM_PSRAM 1
#define CONFIG_ORB_FONTS_PACK_PARTITION "assets"
typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL, ESP_ERR_INVALID_SIZE, ESP_ERR_INVALID_CRC,
       ESP_ERR_INVALID_ARG, ESP_ERR_NOT_FOUND,
       ESP_ERR_INVALID_STATE, ESP_ERR_NO_MEM };
enum { ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY };
typedef struct { uint32_t address, size; } esp_partition_t;
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label);
esp_err_t esp_partition_read(const esp_partition_t *part, size_t offset, void *dst, size_t size);
typedef unsigned esp_partition_mmap_handle_t;
enum { ESP_PARTITION_MMAP_DATA };
esp_err_t esp_partition_mmap(const esp_partition_t *part, size_t offset, size_t size,
                            int flags, const void **data, esp_partition_mmap_handle_t *handle);
void esp_partition_munmap(esp_partition_mmap_handle_t handle);
esp_err_t esp_secure_boot_verify_signature(uint32_t address, uint32_t length);
typedef struct { uint8_t bytes[4096]; } ets_secure_boot_signature_t;
esp_err_t esp_secure_boot_verify_sbv2_signature_block(const ets_secure_boot_signature_t *signature,
                                                     const uint8_t *digest, uint8_t *verified);
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DMA 8
void *heap_caps_malloc(size_t size, unsigned caps);
void heap_caps_free(void *pointer);
size_t heap_caps_get_free_size(unsigned caps);
size_t heap_caps_get_largest_free_block(unsigned caps);
size_t heap_caps_get_minimum_free_size(unsigned caps);
void assets_test_log(const char *tag, const char *format, ...);
#define ESP_LOGI assets_test_log
#define ESP_LOGW(tag, ...) ((void)(tag))
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
typedef struct { int available; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer);
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *buffer);
int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned timeout);
int xSemaphoreGive(SemaphoreHandle_t semaphore);
typedef void *TaskHandle_t;
int xTaskCreateWithCaps(void (*function)(void *), const char *name, unsigned stack,
                        void *context, unsigned priority, TaskHandle_t *handle, unsigned caps);
void vTaskDeleteWithCaps(TaskHandle_t handle);
void vTaskSuspend(TaskHandle_t handle);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t handle);
#define portMAX_DELAY UINT32_MAX
#define pdPASS 1
typedef struct { int active; } psa_hash_operation_t;
#define PSA_HASH_OPERATION_INIT {0}
#define PSA_ALG_SHA_256 1
#define PSA_SUCCESS 0
int psa_hash_setup(psa_hash_operation_t *hash, int algorithm);
int psa_hash_update(psa_hash_operation_t *hash, const void *data, size_t size);
int psa_hash_finish(psa_hash_operation_t *hash, void *digest, size_t size, size_t *actual);
int psa_hash_abort(psa_hash_operation_t *hash);
