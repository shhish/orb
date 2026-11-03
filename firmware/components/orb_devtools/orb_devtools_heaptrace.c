#include "orb_devtools_heaptrace.h"

#include "sdkconfig.h"

#if CONFIG_HEAP_USE_HOOKS
#include <string.h>
#include "esp_attr.h"
#include "esp_debug_helpers.h"
#include "esp_heap_caps.h"
#include "esp_cpu_utils.h"
#include "esp_memory_utils.h"
#include "esp_private/cache_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Attribution-only build (CONFIG_HEAP_USE_HOOKS). IDF's historical internal
 * minimum adds each region's own low water mark, reached at different times,
 * so it can sit below any simultaneous free total. After every internal
 * allocation this samples the actual simultaneous free total and records each
 * new low beneath TRACE_WATCH with its task and a short backtrace. Fixed DRAM
 * slots; the hook never allocates and stays silent in ISRs or while the flash
 * cache is disabled. */
#define TRACE_GOAL (24u * 1024u)
#define TRACE_WATCH (40u * 1024u)
#define TRACE_SLOTS 12u
#define TRACE_DEPTH 8u

typedef struct {
    uint32_t free_after, size, caps, uptime_ms;
    char task[12];
    uint32_t pc[TRACE_DEPTH];
} trace_event_t;

static DRAM_ATTR trace_event_t s_events[TRACE_SLOTS];
static DRAM_ATTR uint32_t s_count, s_floor = UINT32_MAX, s_crossings, s_samples, s_lowest = UINT32_MAX;
static DRAM_ATTR portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* IRAM placement comes from the weak HEAP_IRAM_ATTR declaration in
 * esp_heap_caps.h; repeating the attribute would name a second section. */
void esp_heap_trace_alloc_hook(void *ptr, size_t size, uint32_t caps)
{
    if (!ptr || !esp_ptr_internal(ptr) || xPortInIsrContext() || !spi_flash_cache_enabled()) return;
    uint32_t free_after = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ++s_samples;
    if (free_after < s_lowest) s_lowest = free_after;
    if (free_after >= TRACE_WATCH || free_after >= s_floor) return;
    trace_event_t event = {.free_after = free_after, .size = (uint32_t)size, .caps = caps,
        .uptime_ms = (uint32_t)(esp_timer_get_time() / 1000)};
    strlcpy(event.task, pcTaskGetName(NULL), sizeof(event.task));
    esp_backtrace_frame_t frame = {0};
    esp_backtrace_get_start(&frame.pc, &frame.sp, &frame.next_pc);
    for (unsigned i = 0; i < TRACE_DEPTH && esp_backtrace_get_next_frame(&frame); ++i)
        event.pc[i] = esp_cpu_process_stack_pc(frame.pc);
    portENTER_CRITICAL(&s_lock);
    if (free_after < s_floor) {
        s_floor = free_after;
        ++s_crossings;
        /* Keep the earliest lows; the last slot always holds the deepest. */
        unsigned slot = s_count < TRACE_SLOTS ? s_count++ : TRACE_SLOTS - 1;
        s_events[slot] = event;
    }
    portEXIT_CRITICAL(&s_lock);
}

void orb_devtools_heaptrace_report(orb_devtools_heaptrace_reply_t reply)
{
    trace_event_t events[TRACE_SLOTS];
    portENTER_CRITICAL(&s_lock);
    uint32_t count = s_count, crossings = s_crossings, samples = s_samples, lowest = s_lowest;
    memcpy(events, s_events, sizeof(events));
    portEXIT_CRITICAL(&s_lock);
    reply("@@ORBDEV HEAPTRACE_META goal=%u watch=%u samples=%u simultaneous_low=%u new_lows=%u "
          "recorded=%u historical_region_sum=%u pass=%d\n",
          (unsigned)TRACE_GOAL, (unsigned)TRACE_WATCH, (unsigned)samples,
          (unsigned)(lowest == UINT32_MAX ? 0 : lowest), (unsigned)crossings, (unsigned)count,
          (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
          samples && lowest >= TRACE_GOAL);
    for (uint32_t i = 0; i < count; ++i) {
        const trace_event_t *e = &events[i];
        reply("@@ORBDEV HEAPTRACE slot=%u free_after=%u size=%u caps=0x%x uptime_ms=%u task=%s "
              "pcs=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x\n",
              (unsigned)i, (unsigned)e->free_after, (unsigned)e->size, (unsigned)e->caps,
              (unsigned)e->uptime_ms, e->task, (unsigned)e->pc[0], (unsigned)e->pc[1],
              (unsigned)e->pc[2], (unsigned)e->pc[3], (unsigned)e->pc[4], (unsigned)e->pc[5],
              (unsigned)e->pc[6], (unsigned)e->pc[7]);
    }
}
#else
void orb_devtools_heaptrace_report(orb_devtools_heaptrace_reply_t reply)
{
    (void)reply;
}
#endif
