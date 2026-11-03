/* Gallery rebuilds delete a bubbling child, while its Face root survives. */
#include "orb_ui.h"
#include "sim_display.h"
#include <assert.h>
#include <stdio.h>

static unsigned s_refreshes;
static void refresh(lv_event_t *event)
{
    (void)event;
    s_refreshes++;
}
static void rebuild(lv_event_t *event)
{
    lv_obj_t *root = lv_event_get_current_target_obj(event);
    lv_obj_delete(lv_event_get_target_obj(event));
    lv_obj_t *replacement = lv_obj_create(root);
    lv_obj_set_size(replacement, 80, 80);
}
static void grow_and_delete(lv_event_t *event)
{
    lv_obj_t *owner = lv_event_get_current_target_obj(event);
    for (unsigned i = 0; i < 16; i++)
        lv_obj_add_event_cb(owner, refresh, LV_EVENT_REFRESH, NULL);
    lv_obj_delete(owner);
}
int main(void)
{
    lv_init();
    lv_display_t *display = sim_display_create();
    assert(display); orb_ui_init(NULL);
    lv_obj_t *root = lv_obj_create(lv_screen_active());
    for (unsigned i = 0; i < 4; i++)
        lv_obj_add_event_cb(root, refresh, LV_EVENT_REFRESH, NULL);
    lv_obj_add_event_cb(root, rebuild, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *child = lv_obj_create(root);
    lv_obj_set_event_bubble(child, true);
    lv_obj_send_event(child, LV_EVENT_LONG_PRESSED, NULL);
    assert(lv_obj_get_child_count(root) == 1);
    lv_obj_send_event(root, LV_EVENT_REFRESH, NULL);
    assert(s_refreshes == 4);
    lv_refr_now(display);
    lv_obj_delete(root);
    root = lv_obj_create(lv_screen_active());
    lv_obj_add_event_cb(root, grow_and_delete, LV_EVENT_REFRESH, NULL);
    lv_obj_send_event(root, LV_EVENT_REFRESH, NULL);
    lv_refr_now(display);
    lv_display_delete(display); lv_deinit();
    puts("Event owner lifetime: bubbling rebuild and reallocated self-delete pass");
    return 0;
}
