#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "xtensa_context.h"
#include "link_crash.h"
#include "link_crash_elf.h"
#include "link_proto.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define REPORT_MS 60000
#define US(ms) ((int64_t)(ms) * 1000)
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2

static uint8_t s_image[1024];
static const esp_partition_t s_partition = {.address = 0x1f00000, .size = 0x100000};
static unsigned s_reads, s_checks;
static esp_err_t s_check_error, s_read_error;
static bool s_panic, s_fail_send, s_fail_alloc;
static cJSON *s_received;
static bool s_have_power, s_end;
static link_power_t s_power;
static int s_ws;
static int64_t s_report_at;

static void put32(unsigned offset, uint32_t value)
{
    CHECK(offset + 4 <= sizeof(s_image));
    for (unsigned i = 0; i < 4; i++) s_image[offset + i] = value >> (8 * i);
}

static void program(unsigned index, uint32_t type, uint32_t offset, uint32_t address, uint32_t size)
{
    unsigned ph = 12 + 52 + 32 * index;
    put32(ph, type); put32(ph + 4, offset); put32(ph + 8, address); put32(ph + 16, size);
}

static void fixture(void)
{
    memset(s_image, 0, sizeof(s_image));
    put32(0, sizeof(s_image));
    memcpy(s_image + 12, "\177ELF\1\1\1", 7);
    s_image[12 + 16] = 4; s_image[12 + 18] = 94;
    put32(12 + 28, 52); s_image[12 + 42] = 32; s_image[12 + 44] = 3;
    program(0, 4, 148, 0, 36);
    put32(160, 4); put32(164, 20); put32(168, 677);
    memcpy(s_image + 172, "ESP", 4);
    put32(176, 0x3fc80000); put32(184, 7); put32(192, 0xdeadbeef);
    program(1, 1, 184, 0x3fc80000, 64);
    memcpy(s_image + 196 + offsetof(StaticTask_t, ucDummy7), "sound_prefs", 12);
    program(2, 1, 248, 0x3c010000, 256);
    put32(260 + offsetof(XtExcFrame, pc), 0x42000003);
    put32(260 + offsetof(XtExcFrame, a0), 0x82001003);
    put32(260 + offsetof(XtExcFrame, a1), 0x3c010040);
    put32(260 + 0x40 - 16, 0x82002003); put32(260 + 0x40 - 12, 0x3c010060);
    put32(260 + 0x60 - 16, 0); put32(260 + 0x60 - 12, 0x3c010080);
    s_check_error = s_read_error = ESP_OK;
}

esp_err_t esp_core_dump_image_check(void) { s_checks++; return s_check_error; }
esp_err_t esp_core_dump_image_get(size_t *address, size_t *size)
{
    *address = s_partition.address; *size = sizeof(s_image); return ESP_OK;
}
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    CHECK(type == 1 && subtype == 3 && !label); return &s_partition;
}
esp_err_t esp_partition_read(const esp_partition_t *part, size_t offset, void *out, size_t size)
{
    CHECK(part == &s_partition && part->address > 0x1000000);
    CHECK(offset <= sizeof(s_image) && size <= sizeof(s_image) - offset);
    s_reads++;
    if (s_read_error) return s_read_error;
    memcpy(out, s_image + offset, size); return ESP_OK;
}
bool esp_ptr_executable(const void *pc)
{
    uintptr_t address = (uintptr_t)pc;
    return address >= 0x40000000 && address < 0x44000000;
}
int esp_reset_reason(void) { return s_panic ? 4 : 1; }
const char *esp_err_to_name(esp_err_t error)
{
    return error == ESP_ERR_NOT_FOUND ? "ESP_ERR_NOT_FOUND" : "ESP_ERR_INVALID_CRC";
}
static int rssi(void) { return -50; }
static unsigned heap_caps_get_free_size(int cap) { (void)cap; return 10000; }
static unsigned uptime_s(void) { return 1; }
static int64_t esp_timer_get_time(void) { return 1000000; }
static bool link_radio_report(cJSON *report) { (void)report; return true; }
static void link_radio_report_sent(void) {}
static unsigned link_radio_ping_s(void) { return 15; }
static void esp_websocket_client_set_ping_interval_sec(int ws, unsigned interval) { (void)ws; (void)interval; }
static bool send_event(const char *name, cJSON *params)
{
    CHECK(!strcmp(name, "state.report"));
    cJSON_Delete(s_received); s_received = cJSON_Duplicate(params, true);
    cJSON_Delete(params); return !s_fail_send;
}
static void *test_malloc(size_t size) { return s_fail_alloc ? NULL : malloc(size); }

/* REPORT */

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    s_panic = strcmp(argv[1], "normal") != 0;
    fixture();
    orb_link_crash_t summary;
    CHECK(orb_link_crash_read(&summary) == ESP_OK);
    CHECK(!strcmp(summary.task, "sound_prefs") && summary.pc == 0x42000000);
    CHECK(summary.depth == 3 && summary.backtrace[1] == 0x42001000 && summary.backtrace[2] == 0x42002000);
    CHECK(!summary.corrupted && summary.cause == 7 && summary.vaddr == 0xdeadbeef && s_reads);
    unsigned task_offset = 196 + offsetof(StaticTask_t, ucDummy7);
    memcpy(s_image + task_offset, "a\n\xff", 4);
    CHECK(orb_link_crash_read(&summary) == ESP_OK && !strcmp(summary.task, "a??"));
    s_image[task_offset] = 0;
    CHECK(orb_link_crash_read(&summary) == ESP_OK && !strcmp(summary.task, "unknown"));
    fixture(); put32(184, 20); put32(260 + offsetof(XtExcFrame, pc), 3);
    CHECK(orb_link_crash_read(&summary) == ESP_OK && !summary.corrupted && summary.pc == 0 && summary.depth == 3);
    fixture();
    put32(260 + offsetof(XtExcFrame, a1), 0x3c00fff0);
    CHECK(orb_link_crash_read(&summary) == ESP_OK && summary.corrupted && summary.depth == 1);
    fixture(); put32(12 + 28, UINT32_MAX);
    CHECK(orb_link_crash_read(&summary) == ESP_ERR_INVALID_SIZE);
    fixture(); put32(164, UINT32_MAX);
    CHECK(orb_link_crash_read(&summary) == ESP_ERR_INVALID_SIZE);
    fixture(); s_image[12] = 0;
    CHECK(orb_link_crash_read(&summary) == ESP_ERR_INVALID_RESPONSE);
    fixture(); s_read_error = ESP_FAIL;
    CHECK(orb_link_crash_read(&summary) == ESP_FAIL);
    fixture(); s_check_error = ESP_ERR_INVALID_CRC;
    unsigned reads = s_reads;
    CHECK(orb_link_crash_read(&summary) == ESP_ERR_INVALID_CRC && s_reads == reads);
    CHECK(orb_link_crash_read(NULL) == ESP_ERR_INVALID_ARG);
    fixture();
    bool unavailable = !strcmp(argv[1], "missing") || !strcmp(argv[1], "corrupt");
    if (unavailable) s_check_error = !strcmp(argv[1], "missing") ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_CRC;
    cJSON_Hooks hooks = {test_malloc, free}; cJSON_InitHooks(&hooks);
    s_fail_alloc = true; send_report(); s_fail_alloc = false;
    CHECK(s_end); s_end = false;
    s_fail_send = true; send_report(); s_fail_send = false;
    CHECK(s_end); s_end = false;
    CHECK(!!cJSON_GetObjectItemCaseSensitive(s_received, "crash") == s_panic);
    send_report(); CHECK(!s_end);
    cJSON *crash = cJSON_GetObjectItemCaseSensitive(s_received, "crash");
    CHECK(!!crash == s_panic);
    if (unavailable) {
        CHECK(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(crash, "error")));
        CHECK(!cJSON_GetObjectItemCaseSensitive(crash, "pc"));
    } else if (s_panic) CHECK(cJSON_GetObjectItemCaseSensitive(crash, "pc")->valuedouble == 0x42000000);
    unsigned checks = s_checks;
    send_report(); CHECK(!cJSON_GetObjectItemCaseSensitive(s_received, "crash") && s_checks == checks);
    cJSON_Delete(s_received);
    puts("crash ELF and report tests passed");
    return 0;
}
