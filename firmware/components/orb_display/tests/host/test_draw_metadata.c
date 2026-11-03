/* Production task and heap-layer ownership, including nested layer cleanup. */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "src/draw/lv_draw_private.h"
#include "src/draw/lv_draw_vector_private.h"
#include "esp_heap_caps.h"

#undef LV_LOG_TRACE
#define LV_LOG_TRACE(...) ((void)0)
#undef LV_ASSERT_MALLOC
#define LV_ASSERT_MALLOC(memory) assert(memory)
#undef LV_ASSERT_FORMAT_MSG
#define LV_ASSERT_FORMAT_MSG(test, ...) assert(test)
#undef LV_CHECK_ARG
#define LV_CHECK_ARG(test, action) do { if(!(test)) { action; } } while(0)
#undef LV_LOG_WARN
#define LV_LOG_WARN(...) ((void)0)

static unsigned s_generic, s_external, s_live, s_layers, s_finalized;
static bool s_fail;

void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps)
{
    assert(alignment == _Alignof(max_align_t));
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++s_external;
    if(s_fail) return NULL;
    void *memory = malloc(size);
    assert(memory && (uintptr_t)memory % alignment == 0);
    memset(memory, 0xa5, size);
    ++s_live;
    return memory;
}

void heap_caps_free(void *memory)
{
    if(memory) { assert(s_live); --s_live; }
    free(memory);
}

void *lv_malloc_zeroed(size_t size)
{
    ++s_generic;
    return calloc(1, size);
}

void lv_free(void *memory) { free(memory); }
void lv_memset(void *memory, uint8_t value, size_t size) { memset(memory, value, size); }
void *lv_memcpy(void *destination, const void *source, size_t size) { return memcpy(destination, source, size); }
size_t lv_strnlen(const char *text, size_t size) { return strnlen(text, size); }

void lv_draw_finalize_task_creation(lv_layer_t *layer, lv_draw_task_t *task)
{
    assert(layer->draw_task_head == task && task->draw_dsc);
    s_finalized++;
}

lv_layer_t *lv_draw_layer_create_drop_shadow(lv_layer_t *parent, const lv_draw_dsc_base_t *base,
                                           const lv_area_t *area)
{
    (void)parent; (void)base; (void)area;
    assert(!"unexpected shadow");
    return NULL;
}
void lv_draw_layer_finish_drop_shadow(lv_layer_t *layer, const lv_draw_dsc_base_t *base)
{ (void)layer; (void)base; assert(!"unexpected shadow"); }

void lv_draw_layer_init(lv_layer_t *layer, lv_layer_t *parent, lv_color_format_t format, const lv_area_t *area)
{
    const uint8_t *bytes = (const uint8_t *)layer;
    for(size_t i = 0; i < sizeof(*layer); ++i) assert(bytes[i] == 0);
    layer->parent = parent;
    layer->color_format = format;
    layer->buf_area = *area;
    ++s_layers;
}

void lv_draw_layer_deinit(lv_layer_t *layer)
{
    assert(s_layers);
    while(layer->draw_task_head) {
        lv_draw_task_t *task = layer->draw_task_head;
        layer->draw_task_head = task->next;
        lv_draw_cleanup_task(task);
    }
    --s_layers;
}

lv_draw_label_dsc_t *lv_draw_task_get_label_dsc(lv_draw_task_t *task)
{
    return task->type == LV_DRAW_TASK_TYPE_LABEL ? task->draw_dsc : NULL;
}

/* PRODUCTION_FUNCTIONS */

int main(void)
{
    lv_layer_t parent = {._clip_area = {0, 0, 465, 465}, .opa = 173};
    lv_area_t area = {10, 20, 100, 80};
    /* A realistic population stays externally allocated until the existing
     * completion path retires every descriptor; no per-task header is used. */
    lv_draw_task_t *previous = NULL;
    for(unsigned i = 0; i < 128; ++i) {
        lv_draw_task_t *task = lv_draw_add_task(&parent, &area, LV_DRAW_TASK_TYPE_FILL);
        assert(task && task->target_layer == &parent && task->opa == 173);
        assert(!memcmp(&task->area, &area, sizeof(area)));
        assert(!task->next && task->state == LV_DRAW_TASK_STATE_WAITING);
        assert((uintptr_t)task->draw_dsc % 8 == 0);
        assert(!previous || previous->next == task);
        const uint8_t *descriptor = task->draw_dsc;
        for(size_t j = 0; j < sizeof(lv_draw_fill_dsc_t); ++j) assert(descriptor[j] == 0);
        previous = task;
    }
#ifdef ESP_PLATFORM
    assert(s_external == 128 && !s_generic && s_live == 128);
#else
    assert(s_generic == 128 && !s_external && !s_live);
#endif
    while(parent.draw_task_head) {
        lv_draw_task_t *task = parent.draw_task_head;
        parent.draw_task_head = task->next;
        lv_draw_cleanup_task(task);
    }
    assert(!s_live);

    lv_layer_t *child = lv_draw_layer_create(&parent, LV_COLOR_FORMAT_ARGB8888, &area);
    assert(child && child->opa == parent.opa && s_layers == 1);
    (void)lv_draw_add_task(child, &area, LV_DRAW_TASK_TYPE_LABEL);
    lv_draw_task_t *layer_task = lv_draw_add_task(&parent, &area, LV_DRAW_TASK_TYPE_LAYER);
    ((lv_draw_image_dsc_t *)layer_task->draw_dsc)->src = child;
    lv_draw_cleanup_task(layer_task);
    parent.draw_task_head = NULL;
    assert(!s_layers && !s_live);
    lv_draw_layer_delete(NULL);

    /* Owned strings use the same external-only allocator as the task and
     * cleanup frees both. Explicit lengths copy no extra bytes; static text
     * keeps its original pointer and allocates just the task. */
    char mutable[] = "abcdef";
    lv_draw_label_dsc_t label = {.text = mutable, .text_local = 1, .text_length = 4,
        .font = (const lv_font_t *)&parent, .opa = LV_OPA_COVER};
    unsigned external = s_external, generic = s_generic;
    lv_draw_label(&parent, &label, &area);
    lv_draw_label_dsc_t *owned = parent.draw_task_head->draw_dsc;
    assert(owned->text != mutable && !strcmp(owned->text, "abcd") && owned->text_local);
    mutable[0] = '\0';
    assert(!strcmp(owned->text, "abcd"));
    lv_draw_cleanup_task(parent.draw_task_head); parent.draw_task_head = NULL;
    assert(!s_live && s_finalized == 1);
#ifdef ESP_PLATFORM
    assert(s_external == external + 2 && s_generic == generic);
#else
    assert(s_generic == generic + 2 && s_external == external);
#endif
    label.text = "static"; label.text_local = 0; label.text_static = 1;
    lv_draw_label(&parent, &label, &area);
    owned = parent.draw_task_head->draw_dsc;
    assert(owned->text == label.text && !owned->text_local);
    lv_draw_cleanup_task(parent.draw_task_head); parent.draw_task_head = NULL;
    assert(!s_live && s_finalized == 2);
#ifdef ESP_PLATFORM
    generic = s_generic;
    s_fail = true;
    label.text_local = 1;
    lv_draw_label(&parent, &label, &area);
    assert(!parent.draw_task_head && s_finalized == 2);
    assert(!lv_draw_metadata_alloc(1));
    assert(!lv_draw_metadata_alloc(8192));
    assert(s_generic == generic && !s_live);
#endif
    puts("draw metadata: task/text population, nested cleanup, owned/static strings and no internal fallback/OOM publication passed");
    return 0;
}
