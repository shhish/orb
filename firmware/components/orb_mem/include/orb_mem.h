#pragma once

#include <stdbool.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* Task-context windows, inclusive of concurrent display/radio allocations.
 * No payload, certificate, transcript or allocation address is recorded. */
typedef enum {
    ORB_MEM_LINK_CONNECT,
    ORB_MEM_TLS_HANDSHAKE,
    ORB_MEM_HELLO,
    ORB_MEM_MESSAGE_PARSE,
    ORB_MEM_SNAPSHOT_PARSE,
    ORB_MEM_CARD_SHOW,
    ORB_MEM_VOICE_SERVICE,
    ORB_MEM_VOICE_START,
    ORB_MEM_OTA,
    ORB_MEM_SCROLL,
    ORB_MEM_NAV,
    ORB_MEM_WIFI_START,
    ORB_MEM_UI_REFRESH,
    ORB_MEM_BOARD_INIT,
    ORB_MEM_NAV_RESERVE,
    ORB_MEM_ASSETS_VERIFY,
    ORB_MEM_STORAGE_MOUNT,
    ORB_MEM_FONT_PRELOAD,
    ORB_MEM_GLASS_PREWARM,
    ORB_MEM_PHASE_COUNT,
} orb_mem_phase_t;

#if CONFIG_ORB_DEVTOOLS
void orb_mem_init(void);
void orb_mem_begin(orb_mem_phase_t phase);
void orb_mem_end(orb_mem_phase_t phase);
/* Bracket APIs that add heap regions, such as Bluetooth memory release. IDF
 * stores local-monitor minima by heap-list ordinal; inserting a heap while
 * monitoring would restore unrelated regions' minima. This exclusive scope
 * must be balanced on one task and must not call another orb_mem API. Lifetime
 * minima remain active; local phase monitoring resumes after the mutation. */
void orb_mem_heap_regions_begin(void);
void orb_mem_heap_regions_end(void);
/* Attribute the complete parse window after decoding its envelope, including
 * DOM allocations made before its event name was known. */
void orb_mem_end_as(orb_mem_phase_t phase, orb_mem_phase_t attributed);
/* These suspend local monitoring while reading the historical low water mark,
 * then resume any active windows. Do not read IDF's minimum directly mid-window. */
void orb_mem_heap_info(multi_heap_info_t *info, uint32_t caps);
/* Active invocation minimum for exactly INTERNAL, INTERNAL|DMA or SPIRAM;
 * free/largest remain live. Other capability sets retain historical minima. */
void orb_mem_phase_info(orb_mem_phase_t phase, multi_heap_info_t *info, uint32_t caps);
void orb_mem_report(void);
void orb_mem_report_to(FILE *out);
#else
static inline void orb_mem_init(void) {}
static inline void orb_mem_begin(orb_mem_phase_t phase) { (void)phase; }
static inline void orb_mem_end(orb_mem_phase_t phase) { (void)phase; }
static inline void orb_mem_heap_regions_begin(void) {}
static inline void orb_mem_heap_regions_end(void) {}
static inline void orb_mem_end_as(orb_mem_phase_t phase, orb_mem_phase_t attributed)
{
    (void)phase;
    (void)attributed;
}
static inline void orb_mem_heap_info(multi_heap_info_t *info, uint32_t caps)
{
    heap_caps_get_info(info, caps);
}
static inline void orb_mem_report(void) {}
static inline void orb_mem_report_to(FILE *out) { (void)out; }
static inline void orb_mem_phase_info(orb_mem_phase_t phase, multi_heap_info_t *info, uint32_t caps)
{
    (void)phase;
    heap_caps_get_info(info, caps);
}
#endif
