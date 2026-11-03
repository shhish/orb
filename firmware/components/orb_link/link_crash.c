#include "link_crash.h"
#include "link_crash_elf.h"

#include "esp_core_dump.h"
#include "esp_memory_utils.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "xtensa_context.h"

static bool s_report_sent;

static esp_err_t read_partition(void *ctx, uint32_t offset, void *out, size_t size)
{
    return esp_partition_read(ctx, offset, out, size);
}

static bool executable(uint32_t pc)
{
    return esp_ptr_executable((const void *)(uintptr_t)pc);
}

esp_err_t orb_link_crash_read(orb_link_crash_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = (orb_link_crash_t){0};
    esp_err_t err = esp_core_dump_image_check();
    if (err != ESP_OK) return err;
    size_t address, size;
    err = esp_core_dump_image_get(&address, &size);
    if (err != ESP_OK) return err;
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                         ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part || part->address != address || size > part->size) return ESP_ERR_INVALID_SIZE;
    const link_crash_layout_t layout = {
        .task_name = offsetof(StaticTask_t, ucDummy7),
        .pc = offsetof(XtExcFrame, pc),
        .a0 = offsetof(XtExcFrame, a0),
        .a1 = offsetof(XtExcFrame, a1),
    };
    /* IDF get_summary mmaps the image. Our coredump is at 31 MiB, beyond
     * this board's 24-bit cache mapping. Partition reads work above 16 MiB. */
    return link_crash_elf_read(read_partition, (void *)part, size, &layout, executable, out);
}

bool link_crash_report(cJSON *report)
{
    if (s_report_sent || esp_reset_reason() != ESP_RST_PANIC) return true;
    orb_link_crash_t summary;
    esp_err_t err = orb_link_crash_read(&summary);
    cJSON *crash = cJSON_AddObjectToObject(report, "crash");
    if (!crash) return false;
    if (err != ESP_OK) return cJSON_AddStringToObject(crash, "error", esp_err_to_name(err)) != NULL;
    if (!cJSON_AddStringToObject(crash, "task", summary.task) ||
        !cJSON_AddNumberToObject(crash, "pc", summary.pc) ||
        !cJSON_AddBoolToObject(crash, "corrupted", summary.corrupted)) return false;
    cJSON *bt = cJSON_AddArrayToObject(crash, "backtrace");
    if (!bt) return false;
    for (unsigned i = 0; i < summary.depth; i++) {
        cJSON *pc = cJSON_CreateNumber(summary.backtrace[i]);
        if (!pc) return false;
        if (!cJSON_AddItemToArray(bt, pc)) { cJSON_Delete(pc); return false; }
    }
    return true;
}

void link_crash_report_sent(void)
{
    s_report_sent = true;
}
