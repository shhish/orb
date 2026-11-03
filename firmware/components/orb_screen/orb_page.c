#include "orb_screen_private.h"
#include "orb_page.h"

static void title_deleted(lv_event_t *event)
{
    orb_screen_data_t *data = orb_screen_data(lv_event_get_user_data(event));
    if (data && data->title == lv_event_get_current_target_obj(event)) data->title = NULL;
}

void orb_screen_page_visibility(orb_screen_data_t *data)
{
    if (data->title)
        lv_obj_set_hidden(data->title, !data->preview && data->visibility != ORB_VISIBILITY_ACTIVE);
}

void orb_screen_page_cleanup(orb_screen_data_t *data)
{
    /* Chrome can remain above the legacy shell's independently moving body.
     * Delete it while callbacks still have valid instance state. A descendant
     * title instead follows the framework root's ordinary teardown. */
    if (data->title && lv_obj_get_parent(data->title) != data->root) lv_obj_delete(data->title);
    data->viewport = NULL;
}

void orb_screen_page_bind(orb_screen_instance_t *instance, lv_obj_t *viewport, lv_obj_t *title)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    if (!data || data->destroying || title == data->root) return;
    if (data->title && data->title != title) lv_obj_delete(data->title);
    data->viewport = viewport;
    if (data->title != title && title)
        lv_obj_add_event_cb(title, title_deleted, LV_EVENT_DELETE, instance);
    data->title = title;
    orb_screen_page_visibility(data);
}

lv_obj_t *orb_screen_page_viewport(const orb_screen_instance_t *instance)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    return data && !data->destroying ? data->viewport : NULL;
}

lv_obj_t *orb_screen_page_title(const orb_screen_instance_t *instance)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    return data && !data->destroying ? data->title : NULL;
}

orb_page_context_t orb_page_save(const lv_obj_t *viewport)
{
    return (orb_page_context_t){.scroll = viewport ? lv_obj_get_scroll_y(viewport) : 0};
}

void orb_page_restore(lv_obj_t *viewport, const orb_page_context_t *context)
{
    if (!viewport || !context) return;
    lv_obj_update_layout(viewport);
    int32_t maximum = LV_MAX(0, lv_obj_get_scroll_y(viewport) + lv_obj_get_scroll_bottom(viewport));
    lv_obj_scroll_to_y(viewport, orb_screen_clamp_scroll(context->scroll, maximum), LV_ANIM_OFF);
}

bool orb_sheet_begin(orb_sheet_lifecycle_t *sheet, bool pause_page,
    bool runtime_active, bool runtime_was_paused)
{
    *sheet = (orb_sheet_lifecycle_t){pause_page, runtime_active, runtime_was_paused};
    return runtime_active;
}

bool orb_sheet_end(orb_sheet_lifecycle_t *sheet, bool covered_by_app)
{
    bool resume = sheet->pauses_runtime && !sheet->runtime_was_paused && !covered_by_app;
    sheet->pause_page = false;
    sheet->pauses_runtime = false;
    return resume;
}
