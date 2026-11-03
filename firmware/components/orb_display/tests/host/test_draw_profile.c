/* Counter arithmetic/classification and two-worker contention, no device I/O. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "src/draw/lv_draw_private.h"
#include "src/core/lv_obj_private.h"
#include "src/core/lv_obj_class_private.h"
#include "include/lvgl/draw/lv_draw_sw_profile.h"

const lv_obj_class_t lv_obj_class = {.name = "obj"};
const lv_obj_class_t lv_label_class = {.name = "label"};
const lv_obj_class_t lv_image_class = {.name = "image"};
static const lv_obj_class_t s_custom_class = {.name = "test"};

const lv_obj_class_t *lv_obj_get_class(const lv_obj_t *obj) { return obj->class_p; }

static unsigned slot(unsigned type, unsigned cls, bool transformed)
{
    return (type * LV_DRAW_SW_PROFILE_CLASSES + cls) * 2 + transformed;
}

static void *worker(void *task)
{
    for (unsigned i = 0; i < 100000; i++) {
        lv_draw_sw_profile_record(task, 7);
        lv_draw_sw_profile_stage_record(LV_DRAW_SW_PROFILE_ROWS_DRAW, 3, 8);
    }
    return NULL;
}

int main(void)
{
    lv_draw_sw_profile_snapshot_t first, next;
    lv_draw_sw_profile_snapshot(&first);
    for (unsigned i = 0; i < LV_DRAW_SW_PROFILE_SLOTS; i++) {
        assert(first.rows[i].calls == 0 && first.rows[i].elapsed_us == 0);
    }
    for (unsigned i = 0; i < LV_DRAW_SW_PROFILE_STAGE_COUNT; i++) {
        assert(first.stages[i].calls == 0 && first.stages[i].elapsed_us == 0 && first.stages[i].pixels == 0);
        assert(strcmp(lv_draw_sw_profile_stage_name(i), "UNKNOWN") != 0);
    }
    assert(strcmp(lv_draw_sw_profile_stage_name(LV_DRAW_SW_PROFILE_STAGE_COUNT), "UNKNOWN") == 0);
    lv_draw_sw_profile_stage_record(LV_DRAW_SW_PROFILE_STAGE_COUNT, 123, 456);
    lv_draw_sw_profile_stage_record((lv_draw_sw_profile_stage_t)-1, 123, 456);
    lv_draw_sw_profile_stage_record(LV_DRAW_SW_PROFILE_GLASS_FILTER, UINT32_MAX - 10, UINT32_MAX - 4);
    lv_draw_sw_profile_snapshot(&first);
    lv_draw_sw_profile_stage_record(LV_DRAW_SW_PROFILE_GLASS_FILTER, 20, 12);
    lv_draw_sw_profile_snapshot(&next);
    assert(next.stages[LV_DRAW_SW_PROFILE_GLASS_FILTER].calls == 2);
    assert(next.stages[LV_DRAW_SW_PROFILE_GLASS_FILTER].elapsed_us == 9);
    assert(next.stages[LV_DRAW_SW_PROFILE_GLASS_FILTER].pixels == 7);
    assert((uint32_t)(next.stages[LV_DRAW_SW_PROFILE_GLASS_FILTER].elapsed_us -
                      first.stages[LV_DRAW_SW_PROFILE_GLASS_FILTER].elapsed_us) == 20);
    assert((uint32_t)(next.stages[LV_DRAW_SW_PROFILE_GLASS_FILTER].pixels -
                      first.stages[LV_DRAW_SW_PROFILE_GLASS_FILTER].pixels) == 12);

    lv_obj_t obj = {.class_p = &lv_obj_class};
    lv_draw_image_dsc_t image = {.base.obj = &obj, .scale_x = LV_SCALE_NONE, .scale_y = LV_SCALE_NONE};
    lv_draw_task_t task = {.type = LV_DRAW_TASK_TYPE_IMAGE, .draw_dsc = &image};
    lv_draw_sw_profile_record(&task, 12);
    image.scale_x--;
    lv_draw_sw_profile_record(&task, 34);
    image.scale_x = LV_SCALE_NONE;
    image.rotation = 900;
    lv_draw_sw_profile_record(&task, 56);
    image.rotation = 0;
    image.scale_y--;
    task.type = LV_DRAW_TASK_TYPE_LAYER;
    lv_draw_sw_profile_record(&task, 78);
    image.scale_y = LV_SCALE_NONE;

    obj.class_p = &lv_label_class;
    task.type = LV_DRAW_TASK_TYPE_LABEL;
    lv_draw_sw_profile_record(&task, 90);
    obj.class_p = &lv_image_class;
    task.type = LV_DRAW_TASK_TYPE_IMAGE;
    lv_draw_sw_profile_record(&task, 123);
    obj.class_p = &s_custom_class;
    task.type = LV_DRAW_TASK_TYPE_BORDER;
    lv_draw_sw_profile_record(&task, 45);
    image.base.obj = NULL;
    task.type = LV_DRAW_TASK_TYPE_BLUR;
    lv_draw_sw_profile_record(&task, 67);
    lv_draw_sw_profile_snapshot(&next);
    assert(next.rows[slot(LV_DRAW_TASK_TYPE_IMAGE, 1, false)].elapsed_us == 12);
    assert(next.rows[slot(LV_DRAW_TASK_TYPE_IMAGE, 1, true)].calls == 2);
    assert(next.rows[slot(LV_DRAW_TASK_TYPE_IMAGE, 1, true)].elapsed_us == 90);
    assert(next.rows[slot(LV_DRAW_TASK_TYPE_LAYER, 1, true)].elapsed_us == 78);
    assert(next.rows[slot(LV_DRAW_TASK_TYPE_LABEL, 2, false)].elapsed_us == 90);
    assert(next.rows[slot(LV_DRAW_TASK_TYPE_IMAGE, 3, false)].elapsed_us == 123);
    assert(next.rows[slot(LV_DRAW_TASK_TYPE_BORDER, 4, false)].elapsed_us == 45);
    assert(next.rows[slot(LV_DRAW_TASK_TYPE_BLUR, 0, false)].elapsed_us == 67);

    /* Modulo snapshots remain valid when boot totals wrap during a phase. */
    task.type = LV_DRAW_TASK_TYPE_FILL;
    lv_draw_sw_profile_record(&task, UINT32_MAX - 10);
    lv_draw_sw_profile_snapshot(&first);
    lv_draw_sw_profile_record(&task, 20);
    lv_draw_sw_profile_snapshot(&next);
    unsigned fill = slot(LV_DRAW_TASK_TYPE_FILL, 0, false);
    assert(next.rows[fill].elapsed_us == 9);
    assert((uint32_t)(next.rows[fill].elapsed_us - first.rows[fill].elapsed_us) == 20);
    assert(next.rows[fill].calls - first.rows[fill].calls == 1);

    task.type = LV_DRAW_TASK_TYPE_BOX_SHADOW;
    pthread_t threads[2];
    assert(pthread_create(&threads[0], NULL, worker, &task) == 0);
    assert(pthread_create(&threads[1], NULL, worker, &task) == 0);
    assert(pthread_join(threads[0], NULL) == 0);
    assert(pthread_join(threads[1], NULL) == 0);
    lv_draw_sw_profile_snapshot(&next);
    assert(next.rows[slot(task.type, 0, false)].calls == 200000);
    assert(next.rows[slot(task.type, 0, false)].elapsed_us == 1400000);
    assert(next.stages[LV_DRAW_SW_PROFILE_ROWS_DRAW].calls == 200000);
    assert(next.stages[LV_DRAW_SW_PROFILE_ROWS_DRAW].elapsed_us == 600000);
    assert(next.stages[LV_DRAW_SW_PROFILE_ROWS_DRAW].pixels == 1600000);

    assert(strcmp(lv_draw_sw_profile_type(slot(LV_DRAW_TASK_TYPE_IMAGE, 1, true)), "IMAGE") == 0);
    assert(strcmp(lv_draw_sw_profile_class(slot(LV_DRAW_TASK_TYPE_IMAGE, 1, true)), "obj") == 0);
    assert(lv_draw_sw_profile_transformed(slot(LV_DRAW_TASK_TYPE_IMAGE, 1, true)));
    assert(!lv_draw_sw_profile_transformed(slot(LV_DRAW_TASK_TYPE_IMAGE, 1, false)));
    assert(strcmp(lv_draw_sw_profile_type(LV_DRAW_SW_PROFILE_SLOTS), "OTHER") == 0);
    uint64_t start = lv_draw_sw_profile_time_us();
    assert(lv_draw_sw_profile_time_us() >= start);
    start = lv_draw_sw_profile_stage_begin();
    lv_draw_sw_profile_stage_end(LV_DRAW_SW_PROFILE_ROWS_PREPARE, start, 16);
    lv_draw_sw_profile_snapshot(&next);
    assert(next.stages[LV_DRAW_SW_PROFILE_ROWS_PREPARE].calls == 1);
    assert(next.stages[LV_DRAW_SW_PROFILE_ROWS_PREPARE].pixels == 16);
    puts("draw profile: task/stage classification, modulo deltas, 200000 concurrent updates passed");
    return 0;
}
