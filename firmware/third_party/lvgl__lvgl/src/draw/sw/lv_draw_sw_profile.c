/* ORB PATCH: no allocations, logging, or locks around a draw primitive. */
#include "../../../include/lvgl/draw/lv_draw_sw_profile.h"

#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
#include "../lv_draw_private.h"
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#else
#include <time.h>
#endif

static lv_draw_sw_profile_snapshot_t s_totals;

uint64_t lv_draw_sw_profile_time_us(void)
{
#ifdef ESP_PLATFORM
    return (uint64_t)esp_timer_get_time();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
#endif
}

void lv_draw_sw_profile_record(const lv_draw_task_t * task, uint32_t elapsed_us)
{
    const lv_draw_dsc_base_t * base = task->draw_dsc;
    const lv_obj_class_t * cls = base && base->obj ? lv_obj_get_class(base->obj) : NULL;
    unsigned widget = 0;
    if(cls == &lv_obj_class) widget = 1;
#if LV_USE_LABEL
    else if(cls == &lv_label_class) widget = 2;
#endif
#if LV_USE_IMAGE
    else if(cls == &lv_image_class) widget = 3;
#endif
    else if(cls) widget = 4;

    unsigned type = (unsigned)task->type < LV_DRAW_SW_PROFILE_TYPES ? (unsigned)task->type : 0;
    bool transformed = false;
    if(base && (task->type == LV_DRAW_TASK_TYPE_IMAGE || task->type == LV_DRAW_TASK_TYPE_LAYER)) {
        const lv_draw_image_dsc_t * image = task->draw_dsc;
        transformed = image->rotation != 0 || image->scale_x != LV_SCALE_NONE || image->scale_y != LV_SCALE_NONE;
    }
    unsigned slot = (type * LV_DRAW_SW_PROFILE_CLASSES + widget) * 2 + transformed;
    /* Native 32-bit atomics avoid a cross-core spinlock/64-bit libatomic helper.
     * Each <=60 s phase is far shorter than a 32-bit microsecond wrap. */
    __atomic_fetch_add(&s_totals.rows[slot].elapsed_us, elapsed_us, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_totals.rows[slot].calls, 1, __ATOMIC_RELAXED);
}

void lv_draw_sw_profile_snapshot(lv_draw_sw_profile_snapshot_t * out)
{
    /* Benchmark timer callbacks execute between refreshes, after software
     * workers finish. Atomic loads also make other diagnostic callers safe. */
    for(unsigned i = 0; i < LV_DRAW_SW_PROFILE_SLOTS; i++) {
        out->rows[i].elapsed_us = __atomic_load_n(&s_totals.rows[i].elapsed_us, __ATOMIC_RELAXED);
        out->rows[i].calls = __atomic_load_n(&s_totals.rows[i].calls, __ATOMIC_RELAXED);
    }
    for(unsigned i = 0; i < LV_DRAW_SW_PROFILE_STAGE_COUNT; i++) {
        out->stages[i].elapsed_us = __atomic_load_n(&s_totals.stages[i].elapsed_us, __ATOMIC_RELAXED);
        out->stages[i].calls = __atomic_load_n(&s_totals.stages[i].calls, __ATOMIC_RELAXED);
        out->stages[i].pixels = __atomic_load_n(&s_totals.stages[i].pixels, __ATOMIC_RELAXED);
    }
}

uint64_t lv_draw_sw_profile_stage_begin(void)
{
    return lv_draw_sw_profile_time_us();
}

void lv_draw_sw_profile_stage_end(lv_draw_sw_profile_stage_t stage, uint64_t started, uint32_t pixels)
{
    lv_draw_sw_profile_stage_record(stage, (uint32_t)(lv_draw_sw_profile_time_us() - started), pixels);
}

void lv_draw_sw_profile_stage_record(lv_draw_sw_profile_stage_t stage, uint32_t elapsed_us, uint32_t pixels)
{
    if((unsigned)stage >= LV_DRAW_SW_PROFILE_STAGE_COUNT) return;
    __atomic_fetch_add(&s_totals.stages[stage].elapsed_us, elapsed_us, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_totals.stages[stage].pixels, pixels, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_totals.stages[stage].calls, 1, __ATOMIC_RELAXED);
}

const char * lv_draw_sw_profile_stage_name(unsigned stage)
{
    static const char * const names[LV_DRAW_SW_PROFILE_STAGE_COUNT] = {
        "ROWS_PREPARE", "ROWS_DRAW", "GLASS_PREPARE", "GLASS_DOWNSAMPLE", "GLASS_FILTER", "GLASS_COMPOSE",
        "DRAW_EVENTS"
    };
    return stage < LV_DRAW_SW_PROFILE_STAGE_COUNT ? names[stage] : "UNKNOWN";
}

const char * lv_draw_sw_profile_type(unsigned slot)
{
    if(slot >= LV_DRAW_SW_PROFILE_SLOTS) return "OTHER";
    switch(slot / (LV_DRAW_SW_PROFILE_CLASSES * 2)) {
        case LV_DRAW_TASK_TYPE_FILL: return "FILL";
        case LV_DRAW_TASK_TYPE_BORDER: return "BORDER";
        case LV_DRAW_TASK_TYPE_BOX_SHADOW: return "BOX_SHADOW";
        case LV_DRAW_TASK_TYPE_LETTER: return "LETTER";
        case LV_DRAW_TASK_TYPE_LABEL: return "LABEL";
        case LV_DRAW_TASK_TYPE_IMAGE: return "IMAGE";
        case LV_DRAW_TASK_TYPE_LAYER: return "LAYER";
        case LV_DRAW_TASK_TYPE_LINE: return "LINE";
        case LV_DRAW_TASK_TYPE_ARC: return "ARC";
        case LV_DRAW_TASK_TYPE_TRIANGLE: return "TRIANGLE";
        case LV_DRAW_TASK_TYPE_MASK_RECTANGLE: return "MASK_RECTANGLE";
        case LV_DRAW_TASK_TYPE_MASK_BITMAP: return "MASK_BITMAP";
        case LV_DRAW_TASK_TYPE_BLUR: return "BLUR";
        case LV_DRAW_TASK_TYPE_CUSTOM: return "CUSTOM";
#if LV_USE_VECTOR_GRAPHIC
        case LV_DRAW_TASK_TYPE_VECTOR: return "VECTOR";
#endif
#if LV_USE_3DTEXTURE
        case LV_DRAW_TASK_TYPE_3D: return "3D";
#endif
        default: return "OTHER";
    }
}

const char * lv_draw_sw_profile_class(unsigned slot)
{
    static const char * const names[LV_DRAW_SW_PROFILE_CLASSES] = {"none", "obj", "label", "image", "other"};
    return slot < LV_DRAW_SW_PROFILE_SLOTS ? names[(slot / 2) % LV_DRAW_SW_PROFILE_CLASSES] : "other";
}

bool lv_draw_sw_profile_transformed(unsigned slot)
{
    return (slot & 1) != 0;
}
#endif
