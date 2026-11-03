/* Actual Face rings distinguish display wake from a restored cover. */
#include "orb_faces.h"
#include "orb_home.h"
#include "orb_kit.h"
#include "orb_kit_controls_motion.h"
#include "orb_kit_motion.h"
#include "orb_model.h"
#include "orb_ui.h"
#include "sim_display.h"
#include <assert.h>
#include <stdio.h>
static uint32_t s_now = 10000;
static uint32_t tick(void) { return s_now; }
static void advance(unsigned ms)
{
    while (ms) { unsigned step = LV_MIN(16u, ms); ms -= step; s_now += step; lv_timer_handler(); }
    lv_refr_now(lv_display_get_default());
}
static lv_obj_t *find_ring(lv_obj_t *root, int target)
{
    if (!lv_obj_is_visible(root)) return NULL;
    int32_t semantic;
    if (orb_kit_ring_semantic_value(root, &semantic) && semantic == target * 10) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_ring(lv_obj_get_child(root, i), target); if (found) return found;
    }
    return NULL;
}
int main(void)
{
    lv_init(); lv_tick_set_cb(tick); assert(sim_display_create()); orb_ui_init(NULL);
    assert(orb_model_init() && orb_home_init());
    orb_faces_t *face = orb_faces_create(lv_screen_active(), NULL); assert(face);
    orb_faces_prefs_t prefs; orb_faces_preferences(face, &prefs);
    prefs.face = ORB_FACE_ORB; prefs.slot[0] = ORB_FACE_CLAUDE;
    prefs.slot[1] = ORB_FACE_CODEX; prefs.slot[2] = ORB_FACE_BATTERY;
    orb_faces_set_preferences(face, &prefs);
    orb_faces_snapshot_t data = {.now = 1793182140, .clock = "10:09", .weekday = "WED", .day = "28",
        .usage_percent = {20, 30}, .battery_percent = 80, .online = true, .weather_override = true};
    orb_faces_update(face, &data); advance(600);
    lv_obj_t *ring = find_ring(orb_faces_root(face), 20); assert(ring);
    orb_faces_set_active(face, false); data.usage_percent[0] = 70; orb_faces_update(face, &data); advance(32);
    assert(orb_kit_ring_get_presented_value(ring) == 70 && !orb_kit_motion_timer_running());
    orb_faces_set_active(face, true); assert(orb_kit_ring_get_presented_value(ring) == 20); advance(80);
    assert(orb_kit_ring_get_presented_value(ring) > 20 && orb_kit_ring_get_presented_value(ring) < 70);
    advance(600); assert(!orb_kit_motion_timer_running());
    orb_faces_set_active(face, false); orb_faces_set_active(face, true); assert(!orb_kit_motion_timer_running());
    orb_faces_set_covered(face, true); data.usage_percent[0] = 75; orb_faces_update(face, &data); advance(32);
    assert(orb_kit_ring_get_presented_value(ring) == 75 && !orb_kit_motion_timer_running());
    orb_faces_set_covered(face, false); assert(!orb_kit_motion_timer_running());
    data.usage_percent[0] = -1; orb_faces_update(face, &data); advance(32);
    assert(orb_kit_ring_get_presented_value(ring) == 0 && !orb_kit_motion_timer_running());
    orb_faces_set_active(face, false); orb_faces_set_active(face, true); assert(!orb_kit_motion_timer_running());
    orb_face_complication_t unknown; assert(orb_faces_complication(face, ORB_FACE_CLAUDE, &unknown) && unknown.percent == -1);
    orb_faces_set_active(face, false); data.usage_percent[0] = 45; orb_faces_update(face, &data);
    orb_faces_set_active(face, true);
    assert(orb_kit_ring_get_presented_value(ring) == 45 && !orb_kit_motion_timer_running());
    /* A newly reported value has no invented previous zero to follow. */
    prefs.face = ORB_FACE_ACTIVITY; prefs.activity_slot[0] = ORB_FACE_NEXT;
    prefs.activity_slot[1] = ORB_FACE_CODEX; prefs.activity_slot[2] = ORB_FACE_BATTERY;
    orb_faces_set_preferences(face, &prefs);
    data.next_seconds = 120; data.next_total_seconds = 240; orb_faces_update(face, &data); advance(600);
    lv_obj_t *timer = find_ring(orb_faces_root(face), 50); assert(timer);
    data.next_seconds = 118; orb_faces_update(face, &data);
    assert(orb_kit_ring_get_presented_value(timer) == 49 && !orb_kit_motion_timer_running());
    data.next_seconds = 180; orb_faces_update(face, &data);
    assert(orb_kit_ring_get_presented_value(timer) == 49 && orb_kit_motion_timer_running());
    advance(400); assert(orb_kit_ring_get_presented_value(timer) == 75 && !orb_kit_motion_timer_running());
    orb_faces_destroy(face); advance(32); puts("Alive Face: changed/equal wake, covered restore and unknown pass"); return 0;
}
