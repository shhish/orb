/* ORB PATCH: bounded draw-task attribution for lead-only benchmark builds. */
#ifndef LV_DRAW_SW_PROFILE_H
#define LV_DRAW_SW_PROFILE_H

#include "lv_draw.h"

/* Separate pipeline attribution from draw-task classification. Preparation can
 * precede DRAW, and GLASS_PREPARE includes its downsample/filter children; these
 * counters must never be added to task time or one another as a frame total. */
typedef enum {
    LV_DRAW_SW_PROFILE_ROWS_PREPARE,
    LV_DRAW_SW_PROFILE_ROWS_DRAW,
    LV_DRAW_SW_PROFILE_GLASS_PREPARE,
    LV_DRAW_SW_PROFILE_GLASS_DOWNSAMPLE,
    LV_DRAW_SW_PROFILE_GLASS_FILTER,
    LV_DRAW_SW_PROFILE_GLASS_COMPOSE,
    /* Inclusive synchronous DRAW_MAIN/POST event wall time. Callbacks may
     * drain preceding draw tasks; this overlaps task/stage counters. */
    LV_DRAW_SW_PROFILE_DRAW_EVENTS,
    LV_DRAW_SW_PROFILE_STAGE_COUNT
} lv_draw_sw_profile_stage_t;

#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
#define LV_DRAW_SW_PROFILE_TYPES 17
#define LV_DRAW_SW_PROFILE_CLASSES 5
#define LV_DRAW_SW_PROFILE_SLOTS (LV_DRAW_SW_PROFILE_TYPES * LV_DRAW_SW_PROFILE_CLASSES * 2)

typedef struct {
    uint32_t calls;
    uint32_t elapsed_us;
} lv_draw_sw_profile_counter_t;

typedef struct {
    uint32_t calls;
    uint32_t elapsed_us;
    uint32_t pixels;
} lv_draw_sw_profile_stage_counter_t;

typedef struct {
    lv_draw_sw_profile_counter_t rows[LV_DRAW_SW_PROFILE_SLOTS];
    lv_draw_sw_profile_stage_counter_t stages[LV_DRAW_SW_PROFILE_STAGE_COUNT];
} lv_draw_sw_profile_snapshot_t;

/* Cumulative modulo-32-bit counters. Take differences over <=60 s; no reset
 * races with worker completion. Times include preemption, not scheduler CPU. */
void lv_draw_sw_profile_snapshot(lv_draw_sw_profile_snapshot_t * out);
uint64_t lv_draw_sw_profile_time_us(void);
void lv_draw_sw_profile_record(const lv_draw_task_t * task, uint32_t elapsed_us);
const char * lv_draw_sw_profile_type(unsigned slot);
const char * lv_draw_sw_profile_class(unsigned slot);
bool lv_draw_sw_profile_transformed(unsigned slot);
uint64_t lv_draw_sw_profile_stage_begin(void);
void lv_draw_sw_profile_stage_end(lv_draw_sw_profile_stage_t stage, uint64_t started, uint32_t pixels);
void lv_draw_sw_profile_stage_record(lv_draw_sw_profile_stage_t stage, uint32_t elapsed_us, uint32_t pixels);
const char * lv_draw_sw_profile_stage_name(unsigned stage);
#else
/* Keep the ordinary A/B image free of clocks, atomic updates and profiler
 * storage. Call sites use the same interface in either build. */
static inline uint64_t lv_draw_sw_profile_stage_begin(void) { return 0; }
static inline void lv_draw_sw_profile_stage_end(lv_draw_sw_profile_stage_t stage, uint64_t started,
                                               uint32_t pixels)
{
    (void)stage;
    (void)started;
    (void)pixels;
}
static inline void lv_draw_sw_profile_stage_record(lv_draw_sw_profile_stage_t stage, uint32_t elapsed_us,
                                                  uint32_t pixels)
{
    (void)stage;
    (void)elapsed_us;
    (void)pixels;
}
#endif
#endif
