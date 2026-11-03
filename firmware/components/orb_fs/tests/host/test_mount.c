#include "orb_fs_port.h"
#include "esp_littlefs.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned s_checks, s_registers, s_formats, s_reads, s_locks, s_heap_reads, s_logs;
static esp_err_t s_mount_error, s_format_error;
static bool s_no_partition, s_no_mutex;
static size_t s_bad_byte = SIZE_MAX, s_read_failure = SIZE_MAX;
static const esp_partition_t s_partition = { .size = 1025 };
#define CHECK(value) do { ++s_checks; assert(value); } while (0)

void orb_test_log(const char *tag, const char *format, ...)
{
    CHECK(!strcmp(tag, "orb_fs") && format); ++s_logs;
}
size_t heap_caps_get_free_size(uint32_t caps)
{
    CHECK(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) ||
          caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    ++s_heap_reads;
    return 50000;
}
size_t heap_caps_get_minimum_free_size(uint32_t caps) { return heap_caps_get_free_size(caps); }
size_t heap_caps_get_largest_free_block(uint32_t caps) { return heap_caps_get_free_size(caps); }

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    return s_no_mutex ? NULL : storage;
}
int xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t ticks)
{
    CHECK(semaphore != NULL && ticks == UINT32_MAX); ++s_locks; return 1;
}
int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    CHECK(semaphore != NULL && s_locks == 1); --s_locks; return 1;
}
esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *config)
{
    CHECK(!strcmp(config->base_path, "/storage"));
    CHECK(!strcmp(config->partition_label, "storage"));
    CHECK(!config->format_if_mount_failed && !config->grow_on_mount);
    ++s_registers;
    return s_registers == 1 ? s_mount_error : ESP_OK;
}
esp_err_t esp_littlefs_format(const char *label)
{
    CHECK(!strcmp(label, "storage")); ++s_formats; return s_format_error;
}
esp_err_t esp_littlefs_info(const char *label, size_t *total, size_t *used)
{
    CHECK(!strcmp(label, "storage")); *total = s_partition.size; *used = 100; return ESP_OK;
}
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    CHECK(type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_ANY);
    CHECK(!strcmp(label, "storage"));
    return s_no_partition ? NULL : &s_partition;
}
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t at, void *out, size_t size)
{
    CHECK(partition == &s_partition && size <= 256 && at + size <= partition->size);
    ++s_reads;
    if (at == s_read_failure) return ESP_FAIL;
    memset(out, 0xff, size);
    if (s_bad_byte >= at && s_bad_byte < at + size) ((unsigned char *)out)[s_bad_byte - at] = 0;
    return ESP_OK;
}

static void reset(esp_err_t mount_error)
{
    s_registers = s_formats = s_reads = 0;
    s_mount_error = mount_error;
    s_format_error = ESP_OK;
    s_no_partition = false;
    s_bad_byte = s_read_failure = SIZE_MAX;
}

int main(void)
{
    s_no_mutex = true;
    CHECK(orb_fs_port_mount() == ESP_ERR_NO_MEM);
    s_no_mutex = false;
    reset(ESP_OK);
    CHECK(orb_fs_port_mount() == ESP_OK);
    CHECK(s_registers == 1 && !s_formats && !s_reads);
    orb_fs_port_lock(); orb_fs_port_unlock();
    CHECK(!s_locks);
    size_t total, used;
    CHECK(orb_fs_port_usage(&total, &used) == ESP_OK && total == 1025 && used == 100);
    reset(ESP_ERR_NO_MEM);
    CHECK(orb_fs_port_mount() == ESP_ERR_NO_MEM);
    CHECK(s_registers == 1 && !s_formats && !s_reads);
    reset(ESP_FAIL);
    s_no_partition = true;
    CHECK(orb_fs_port_mount() == ESP_ERR_NOT_FOUND);
    CHECK(!s_formats && !s_reads);
    reset(ESP_FAIL);
    s_bad_byte = s_partition.size - 1;
    CHECK(orb_fs_port_mount() == ESP_ERR_INVALID_STATE);
    CHECK(s_reads == 5 && !s_formats && s_registers == 1);
    reset(ESP_FAIL);
    s_read_failure = 256;
    CHECK(orb_fs_port_mount() == ESP_FAIL);
    CHECK(s_reads == 2 && !s_formats);
    reset(ESP_FAIL);
    s_format_error = ESP_FAIL;
    CHECK(orb_fs_port_mount() == ESP_FAIL);
    CHECK(s_reads == 5 && s_formats == 1 && s_registers == 1);
    reset(ESP_FAIL);
    CHECK(orb_fs_port_mount() == ESP_OK);
    CHECK(s_reads == 5 && s_formats == 1 && s_registers == 2);
    CHECK(s_heap_reads == 12 * 8 && s_logs == 2 * 8 + 2);
    printf("orb_fs mount: %u checks passed\n", s_checks);
    return 0;
}
