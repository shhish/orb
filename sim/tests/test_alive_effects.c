/* The real renderer checks finite, target-honest kit effects. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_kit.h"
#include "sim_display.h"

extern size_t sim_memory_allocation_calls(void);

static uint32_t s_now;
static unsigned s_checks, s_frames;
static uint8_t s_before[SIM_D * SIM_D * 3], s_after[SIM_D * SIM_D * 3];
static uint32_t tick(void) { return s_now; }
static void rendered(lv_event_t *event) { LV_UNUSED(event); s_frames++; }
static void require(bool valid, const char *message)
{
    s_checks++;
    if (!valid) { fprintf(stderr, "alive effects: %s\n", message); exit(EXIT_FAILURE); }
}
static void advance(unsigned ms)
{
    unsigned end = s_now + ms;
    while (s_now < end) { s_now += LV_MIN(8, end - s_now); lv_timer_handler(); }
}
static void quiet(void)
{
    advance(1200);
    unsigned frames = s_frames;
    advance(1200);
    require(s_frames == frames, "settled effect rendered another frame");
    require(!orb_kit_motion_timer_running(), "shared kit timer still running at rest");
}
static lv_obj_t *label(lv_obj_t *root, const char *text)
{
    lv_obj_t *obj = lv_label_create(root);
    orb_text_apply(obj, ORB_TEXT_TITLE2, ORB_TEXT_TABULAR, 0);
    lv_obj_set_style_text_color(obj, lv_color_white(), 0);
    lv_obj_set_pos(obj, 100, 190);
    orb_kit_value_set(obj, text, "888 lights on");
    return obj;
}
static void digit_roll(lv_obj_t *root)
{
    lv_obj_t *value = label(root, "12 lights on");
    orb_kit_value_set_rolling(value, "12 lights on", NULL);
    quiet();
    int32_t width = lv_obj_get_width(value);
    orb_kit_value_set_rolling(value, "13 lights on", NULL);
    require(!strcmp(lv_label_get_text(value), "13 lights on"), "roll exposed a false target");
    const char *semantic = NULL;
    require(orb_kit_value_roll_semantic_text(value, &semantic) && !strcmp(semantic, "13 lights on"),
        "roll hid its true target from semantic traces");
    require(orb_kit_motion_timer_running(), "digit change did not start rolling");
    advance(80);
    sim_display_rgb(s_before);
    require(lv_obj_get_width(value) == width, "roll changed its reservation");
    advance(400);
    sim_display_rgb(s_after);
    require(memcmp(s_before, s_after, sizeof(s_before)), "digit roll had no intermediate presentation");
    /* The leading unchanged digit occupies x100..126. It must not slide. */
    bool unchanged = true;
    for (unsigned y = 180; y < 260; y++)
        for (unsigned x = 100; x < 126; x++) {
            unsigned offset = (y * SIM_D + x) * 3;
            unchanged &= !memcmp(s_before + offset, s_after + offset, 3);
        }
    require(unchanged, "unchanged digit moved during the roll");
    require(lv_obj_get_style_text_opa(value, 0) == LV_OPA_COVER, "roll endpoint lost opacity");
    orb_kit_value_set_rolling(value, "14 lights on", NULL);
    require(!orb_kit_motion_timer_running(), "subsecond update rolled");
    advance(1100);
    orb_kit_value_set_rolling(value, "All off", NULL);
    require(!orb_kit_motion_timer_running(), "nondigit text change rolled");
    advance(1100);
    orb_kit_value_set_rolling(value, "15 lights on", NULL);
    advance(1100);
    orb_ui_set_reduce_motion(true);
    orb_kit_value_set_rolling(value, "16 lights on", NULL);
    require(!orb_kit_motion_timer_running(), "Reduce Motion still rolled");
    orb_ui_set_reduce_motion(false);
    advance(1100);
    orb_kit_value_set_rolling(value, "17 lights on", NULL);
    lv_timer_t *refresh = lv_display_get_refr_timer(lv_display_get_default());
    lv_timer_pause(refresh);
    advance(32);
    size_t allocations = sim_memory_allocation_calls();
    advance(96);
    require(sim_memory_allocation_calls() == allocations, "digit roll allocated from its timer ticks");
    lv_timer_resume(refresh);
    lv_obj_delete(value);
    quiet();
}
static void digit_length_roll(lv_obj_t *root)
{
    lv_obj_t *value = label(root, "9 lights on");
    const char *pairs[][2] = {
        {"9 lights on", "10 lights on"}, {"99 lights on", "100 lights on"},
        {"10 lights on", "9 lights on"},
    };
    for (unsigned i = 0; i < 3; i++) {
        orb_kit_value_set_rolling(value, pairs[i][0], NULL);
        quiet();
        int32_t width = lv_obj_get_width(value);
        orb_kit_value_set_rolling(value, pairs[i][1], NULL);
        require(orb_kit_motion_timer_running(), "added or removed digit column did not roll");
        require(!strcmp(lv_label_get_text(value), pairs[i][1]), "length-changing roll lost semantic target");
        advance(80); sim_display_rgb(s_before);
        advance(400); sim_display_rgb(s_after);
        require(memcmp(s_before, s_after, sizeof(s_before)), "length-changing roll lacked intermediate ink");
        require(lv_obj_get_width(value) == width && lv_obj_get_style_text_opa(value, 0) == LV_OPA_COVER,
            "length-changing roll reflowed or missed exact endpoint");
    }
    advance(1100);
    orb_kit_value_set_rolling(value, "9 lamps off", NULL);
    require(!orb_kit_motion_timer_running(), "changed nondigit run rolled");
    orb_kit_value_set_rolling(value, "9 lights on", NULL);
    advance(1100);
    orb_kit_value_set_rolling(value, "10 lights on", NULL);
    advance(80);
    orb_kit_value_set_rolling(value, "11 lights on", NULL);
    require(!orb_kit_motion_timer_running(), "rapid length-changing update rolled");
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    orb_kit_value_set_rolling(value, "19", "888");
    quiet();
    lv_area_t bounds; lv_obj_get_coords(value, &bounds);
    int32_t digit_width = lv_font_get_glyph_width(lv_obj_get_style_text_font(value, 0), '9', 0) +
        lv_obj_get_style_text_letter_space(value, 0);
    orb_kit_value_set_rolling(value, "119", NULL);
    advance(80); sim_display_rgb(s_before);
    advance(400); sim_display_rgb(s_after);
    bool same = true;
    unsigned ink = 0;
    for (int y = bounds.y1; y <= bounds.y2; y++)
        for (int x = bounds.x2 - 2 * digit_width + 3; x <= bounds.x2; x++) {
            unsigned at = (y * SIM_D + x) * 3;
            same &= !memcmp(s_before + at, s_after + at, 3);
            ink += s_before[at] > 128;
        }
    require(same && ink > 10, "common right-aligned digit columns moved when a column was added");
    lv_obj_delete(value);
    quiet();
}

static void wait_budget_and_resume(lv_obj_t *root)
{
    lv_obj_t *waiting = label(root, "Loading");
    lv_obj_set_style_text_opa(waiting, 153, 0);
    quiet();
    uint32_t started = s_now;
    orb_kit_motion_set_active(root, false);
    orb_kit_breathe_since(waiting, true, started);
    require(!orb_kit_motion_timer_running(), "staged waiting label started its clock while covered");
    advance(500);
    orb_kit_motion_set_active(root, true);
    require(orb_kit_motion_timer_running(), "activation did not resume a registered wait");
    require(lv_obj_get_style_text_opa(waiting, 0) == LV_OPA_COVER, "essential waiting ink did not start at full opacity");
    advance(800);
    require(lv_obj_get_style_text_opa(waiting, 0) >= 128, "essential waiting ink fell below the HIG contrast floor");
    orb_kit_motion_interrupt(root);
    require(orb_kit_motion_timer_running(), "a touch stopped a named wait that is still pending");
    orb_kit_breathe_since(waiting, true, started);
    require(orb_kit_motion_timer_running(), "restating the same model wait stopped its breath");
    orb_kit_motion_set_active(root, false);
    orb_kit_motion_set_active(root, true);
    require(orb_kit_motion_timer_running(), "visible paused wait did not resume inside its original budget");
    orb_kit_motion_set_active(root, false);
    require(lv_obj_get_style_text_opa(waiting, 0) == 153, "quiet wait did not restore its saved secondary opacity");
    advance(30000);
    orb_kit_motion_set_active(root, true);
    require(!orb_kit_motion_timer_running(), "cover return renewed the absolute 30-second budget");
    lv_obj_delete(waiting);
    waiting = label(root, "Loading");
    orb_kit_breathe_since(waiting, true, started);
    require(!orb_kit_motion_timer_running(), "rebuilding a waiting label renewed its expired budget");
    orb_kit_breathe_since(waiting, true, s_now - 29900);
    advance(200);
    require(!orb_kit_motion_timer_running(), "rebuilt near-deadline wait gained a fresh 30 seconds");
    orb_kit_breathe(waiting, false);
    orb_kit_breathe(waiting, true);
    advance(160);
    lv_obj_set_pos(waiting, 600, 190);
    lv_obj_update_layout(waiting);
    advance(120);
    require(!orb_kit_motion_timer_running(), "clipped offscreen wait kept ticking");
    lv_obj_set_pos(waiting, 100, 190);
    lv_obj_update_layout(waiting);
    orb_kit_breathe_resume(root);
    require(orb_kit_motion_timer_running(), "visible waiting label did not resume");
    lv_obj_delete(waiting);
    quiet();
}

static void wait_contrast_floor(lv_obj_t *root)
{
    const lv_opa_t inks[] = {ORB_KIT_SECONDARY_OPA, ORB_KIT_DISABLED_OPA};
    for (unsigned i = 0; i < 2; i++) {
        lv_obj_t *word = label(root, "Saving…");
        lv_obj_set_style_text_opa(word, inks[i], 0);
        orb_kit_breathe_since(word, true, s_now);
        require(lv_obj_get_style_text_opa(word, 0) == LV_OPA_COVER, "essential wait did not use literal full-strength ink");
        unsigned minimum = LV_OPA_COVER, lower = 128;
        for (unsigned sample = 0; sample < 150; sample++) {
            advance(8);
            unsigned opacity = lv_obj_get_style_text_opa(word, 0);
            require(opacity >= lower, "essential wait fell below literal 50% opacity");
            minimum = LV_MIN(minimum, opacity);
        }
        require(minimum <= lower + 6, "sampled wait did not approach its literal 50% trough");
        orb_kit_breathe(word, false);
        require(lv_obj_get_style_text_opa(word, 0) == inks[i], "Breathe completion lost the native ink");
        lv_obj_delete(word); quiet();
    }
}

static void staged_wait(lv_obj_t *root)
{
    lv_obj_t *page = lv_obj_create(root);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, 466, 466);
    lv_obj_set_scrollable(page, false);
    lv_obj_set_hidden(page, true);
    lv_obj_t *word = label(page, "Loading");
    uint32_t started = s_now;
    orb_kit_breathe_since(word, true, started);
    require(!orb_kit_motion_timer_running(), "hidden wait admitted motion before its page");
    advance(1200);
    lv_obj_set_hidden(page, false);
    orb_kit_arrival_complete(page, true);
    advance(400);
    require(orb_kit_motion_timer_running(), "visible arrival fence did not resume its staged wait");
    advance(28432);
    require(!orb_kit_motion_timer_running(), "staged wait restarted its absolute budget at visibility");
    lv_obj_delete(page); quiet();
}

static void disc_and_pop(lv_obj_t *root)
{
    lv_obj_t *disc = orb_kit_disc_create(root, ORB_KIT_DISC_STANDARD,
        ORB_KIT_ACCENT_FILL, ORB_GRAY4, ORB_ICON_LIGHTBULB);
    lv_obj_set_pos(disc, 193, 193);
    quiet();
    orb_kit_disc_light_up(disc, ORB_GRAY4, ORB_KIT_HOME);
    require(orb_kit_motion_timer_running(), "fill did not start");
    advance(40);
    sim_display_rgb(s_before);
    advance(300);
    require(lv_color_eq(lv_obj_get_style_bg_color(disc, 0), ORB_KIT_HOME), "fill missed exact target");
    sim_display_rgb(s_after);
    require(memcmp(s_before, s_after, sizeof(s_before)), "flat fill had no intermediate state");
    quiet();
    orb_kit_disc_light_up(disc, ORB_KIT_HOME, ORB_GRAY4);
    lv_obj_send_event(disc, LV_EVENT_PRESSED, NULL);
    advance(100);
    require(!orb_kit_motion_timer_running(), "touch failed to cancel a light-up");
    lv_obj_send_event(disc, LV_EVENT_RELEASED, NULL);
    quiet();
    lv_obj_add_state(disc, LV_STATE_PRESSED);
    orb_kit_disc_light_up_to(disc, ORB_KIT_HOME, LV_OPA_COVER);
    require(orb_kit_motion_timer_running(), "committed toggle skipped Light up before LVGL retired PRESSED");
    lv_obj_remove_state(disc, LV_STATE_PRESSED);
    advance(300);
    require(lv_color_eq(lv_obj_get_style_bg_color(disc, 0), ORB_KIT_HOME), "committed toggle missed its final fill");
    quiet();
    int32_t x = lv_obj_get_x(disc), y = lv_obj_get_y(disc), width = lv_obj_get_width(disc);
    orb_kit_pop_in(disc);
    require(lv_obj_get_style_transform_scale_x(disc, 0) < 256, "pop did not start at 80 percent");
    lv_point_t edge = {x + 2, y + width / 2};
    require(lv_indev_search_obj(root, &edge) == disc, "pop shrank its real pointer hit area");
    lv_point_t outside = {x - 2, y + width / 2};
    require(lv_indev_search_obj(root, &outside) != disc, "pop enlarged its real pointer hit area");
    advance(80);
    require(lv_obj_get_style_transform_scale_x(disc, 0) > 204, "pop did not progress");
    require(lv_obj_get_x(disc) == x && lv_obj_get_y(disc) == y && lv_obj_get_width(disc) == width,
        "pop moved layout or hit geometry");
    advance(240);
    require(orb_kit_motion_timer_running(), "pop clipped its spring before the 350 ms boundary");
    require(lv_indev_search_obj(root, &edge) == disc && lv_indev_search_obj(root, &outside) != disc,
        "pop changed its pointer hit area near the spring endpoint");
    lv_refr_now(lv_display_get_default());
    sim_display_rgb(s_before);
    lv_obj_invalidate(root);
    lv_refr_now(lv_display_get_default());
    sim_display_rgb(s_after);
    require(!memcmp(s_before, s_after, sizeof(s_before)), "pop draw extent left stale edge pixels");
    advance(80);
    require(lv_obj_get_style_transform_scale_x(disc, 0) == 256, "pop missed exact endpoint");
    orb_kit_pop_in(disc);
    advance(40);
    lv_obj_send_event(disc, LV_EVENT_PRESSED, NULL);
    require(lv_obj_get_style_transform_scale_x(disc, 0) == 256, "touch did not finish pop");
    lv_obj_send_event(disc, LV_EVENT_RELEASED, NULL);
    advance(200);
    orb_kit_disc_light_up(disc, ORB_GRAY4, ORB_KIT_HOME);
    lv_obj_delete(disc);
    quiet();
}
static void transformed_disc_damage(lv_obj_t *root)
{
    lv_obj_t *disc = orb_kit_disc_create(root, ORB_KIT_DISC_STANDARD,
        ORB_KIT_ACCENT_FILL, ORB_GRAY4, ORB_ICON_LOCK_FILL);
    lv_obj_set_pos(disc, 193, 100);
    quiet();
    lv_obj_set_style_transform_scale(disc, ORB_KIT_PRESS_SCALE, 0);
    advance(32);
    orb_kit_disc_set_glyph(disc, ORB_ICON_LOCK_OPEN);
    orb_kit_disc_set_ink(disc, lv_color_black());
    lv_obj_set_style_bg_color(disc, ORB_KIT_HOME, 0);
    lv_refr_now(lv_display_get_default());
    sim_display_rgb(s_before);
    lv_obj_invalidate(root);
    lv_refr_now(lv_display_get_default());
    sim_display_rgb(s_after);
    require(!memcmp(s_before, s_after, sizeof(s_before)),
        "pressed state disc differs between partial and full frozen redraw");
    lv_obj_delete(disc);
    quiet();
}

static void remote_pop_arrival(lv_obj_t *root)
{
    lv_obj_t *disc = orb_kit_disc_create(root, ORB_KIT_DISC_STANDARD,
        ORB_KIT_ACCENT_FILL, ORB_GRAY4, ORB_ICON_LIGHTBULB);
    lv_obj_set_pos(disc, 320, 90); quiet();
    lv_area_t before, after; lv_obj_get_coords(disc, &before);
    int32_t pivot_x = lv_obj_get_style_transform_pivot_x(disc, 0);
    int32_t pivot_y = lv_obj_get_style_transform_pivot_y(disc, 0);
    lv_point_t edge = {before.x1 + lv_obj_get_width(disc) / 2, before.y1 + 2};
    lv_point_t outside = {edge.x, before.y1 - 2};
    orb_kit_pop_in_arrival_from(disc, 233, 233);
    int32_t staged = lv_obj_get_style_transform_scale_x(disc, 0);
    require(staged < 256 && lv_obj_get_style_transform_pivot_x(disc, 0) == 233 - before.x1 &&
        lv_obj_get_style_transform_pivot_y(disc, 0) == 233 - before.y1,
        "remote arrival did not stage about its display-space origin");
    advance(160);
    require(lv_obj_get_style_transform_scale_x(disc, 0) == staged,
        "remote Pop moved before the fresh transition fence");
    require(lv_indev_search_obj(root, &edge) == disc && lv_indev_search_obj(root, &outside) != disc,
        "remote staged Pop changed the original pointer target");
    orb_kit_arrival_complete(root, true); advance(80);
    require(lv_obj_get_style_transform_scale_x(disc, 0) > staged &&
        lv_obj_get_style_transform_scale_x(disc, 0) != 256, "fresh remote Pop had no intermediate ink");
    lv_obj_get_coords(disc, &after);
    require(!memcmp(&before, &after, sizeof(before)) && lv_indev_search_obj(root, &edge) == disc &&
        lv_indev_search_obj(root, &outside) != disc, "remote Pop moved layout or pointer geometry");
    advance(320);
    require(!orb_kit_motion_timer_running() && lv_obj_get_style_transform_scale_x(disc, 0) == 256 &&
        lv_obj_get_style_transform_pivot_x(disc, 0) == pivot_x &&
        lv_obj_get_style_transform_pivot_y(disc, 0) == pivot_y, "remote Pop missed its restored endpoint");
    quiet(); orb_kit_pop_in_arrival_from(disc, 233, 233); orb_kit_arrival_complete(root, false);
    require(!orb_kit_motion_timer_running() && lv_obj_get_style_transform_scale_x(disc, 0) == 256 &&
        lv_obj_get_style_transform_pivot_x(disc, 0) == pivot_x &&
        lv_obj_get_style_transform_pivot_y(disc, 0) == pivot_y, "Back replayed or stranded remote Pop");
    quiet(); lv_obj_delete(disc); quiet();
}

static void composed_arrival(lv_obj_t *root)
{
    lv_obj_t *glyph = label(root, ORB_ICON_CHECK);
    quiet();
    orb_kit_pop_in_arrival(glyph);
    orb_kit_check_label_arrival(glyph);
    require(orb_kit_motion_count() == 2, "Pop and Draw arrival fences replaced one another");
    require(lv_obj_get_style_transform_scale_x(glyph, 0) < 256,
        "composed arrival did not stage its Pop before the snapshot");
    orb_kit_arrival_complete(root, true);
    advance(80);
    require(lv_obj_get_style_transform_scale_x(glyph, 0) > 204 &&
        lv_obj_get_style_text_opa(glyph, 0) == LV_OPA_TRANSP,
        "Pop and Draw did not make simultaneous intermediate progress");
    advance(320);
    require(lv_obj_get_style_transform_scale_x(glyph, 0) == 256 &&
        lv_obj_get_style_opa(glyph, 0) == LV_OPA_COVER &&
        lv_obj_get_style_text_opa(glyph, 0) == LV_OPA_COVER,
        "composed arrival stranded scaled or transparent outcome ink");
    quiet();
    orb_kit_pop_in_arrival(glyph);
    orb_kit_check_label_arrival(glyph);
    orb_kit_arrival_complete(root, false);
    require(!orb_kit_motion_timer_running() &&
        lv_obj_get_style_transform_scale_x(glyph, 0) == 256 &&
        lv_obj_get_style_opa(glyph, 0) == LV_OPA_COVER &&
        lv_obj_get_style_text_opa(glyph, 0) == LV_OPA_COVER,
        "Back/restore did not settle every composed arrival property");
    orb_kit_pop_in_arrival(glyph);
    orb_kit_check_label_arrival(glyph);
    orb_kit_arrival_complete(root, true);
    advance(48);
    orb_kit_motion_interrupt(root);
    require(!orb_kit_motion_timer_running() &&
        lv_obj_get_style_transform_scale_x(glyph, 0) == 256 &&
        lv_obj_get_style_text_opa(glyph, 0) == LV_OPA_COVER,
        "touch failed to finish every composed arrival property");
    lv_obj_delete(glyph);
    quiet();
}

static lv_obj_t *find_text(lv_obj_t *obj, const char *text)
{
    if (lv_obj_check_type(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), text)) return obj;
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *found = find_text(lv_obj_get_child(obj, i), text);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *visible_text(lv_obj_t *obj, const char *text)
{
    if (!lv_obj_is_visible(obj)) return NULL;
    if (lv_obj_check_type(obj, &lv_label_class) && lv_obj_get_style_text_opa(obj, 0) &&
        !strcmp(lv_label_get_text(obj), text)) return obj;
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *found = visible_text(lv_obj_get_child(obj, i), text); if (found) return found;
    }
    return NULL;
}

static void native_row_waits(lv_obj_t *root)
{
    const orb_kit_row_text_role_t roles[] = {ORB_KIT_ROW_TEXT_TITLE, ORB_KIT_ROW_TEXT_SUBTITLE};
    for (unsigned i = 0; i < 2; i++) {
        const char *word = i ? "Saving…" : "Loading…", *done = i ? "Saved" : "Ready";
        lv_obj_t *list = lv_obj_create(root); lv_obj_remove_style_all(list);
        lv_obj_set_size(list, SIM_D, SIM_D); lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF); lv_obj_set_style_pad_bottom(list, 400, 0);
        lv_obj_t *row = orb_kit_row_create(list, &(orb_kit_row_config_t){
            .title = i ? "Claude" : word, .subtitle = i ? word : NULL,
            .glyph = ORB_ICON_CLOCK, .accent = ORB_KIT_GREY});
        lv_obj_set_y(row, 190); require(orb_kit_list_apply(list), "prepare native waiting row"); quiet();
        lv_obj_t *native = find_text(row, word); require(native != NULL, "native wait word missing");
        lv_area_t before, after; lv_obj_get_coords(row, &before);
        unsigned children = lv_obj_get_child_count(list);
        orb_kit_row_set_waiting(row, ORB_KIT_ROW_TEXT_VALUE, false);
        require(lv_obj_get_child_count(list) == children, "stopping absent wait allocated an overlay");
        uint32_t started = s_now - 20000;
        orb_kit_row_set_waiting_since(row, roles[i], true, started); advance(400);
        lv_obj_t *live = visible_text(list, word);
        require(live && live != native, "native wait did not promote visible live ink");
        require(orb_kit_motion_timer_running() && lv_obj_get_style_text_opa(live, 0) >= 128 &&
            lv_obj_get_style_text_opa(live, 0) < LV_OPA_COVER, "promoted wait violated literal 50%-100% ink");
        lv_obj_get_coords(row, &after);
        require(!memcmp(&before, &after, sizeof(before)), "wait promotion moved the row or hit target");
        orb_kit_row_set_waiting(row, roles[i], true);
        orb_kit_row_set_waiting_since(row, roles[i], true, started); advance(9700);
        require(!orb_kit_motion_timer_running(), "native promotion renewed the original wait deadline");
        orb_kit_row_set_waiting(row, roles[i], false); orb_kit_row_set_text(row, roles[i], done); advance(400);
        require(visible_text(list, done) == live && !visible_text(list, word), "completed wait left stale or blank native ink");
        require(lv_obj_get_style_text_opa(live, 0) == (i ? ORB_KIT_SECONDARY_OPA : LV_OPA_COVER),
            "promoted wait completion lost its original ink opacity"); quiet();
        orb_kit_row_set_text(row, roles[i], word); orb_kit_row_set_waiting(row, roles[i], true); advance(400);
        require(orb_kit_motion_timer_running(), "new native-row wait did not start a fresh budget");
        orb_kit_row_set_waiting(row, roles[i], false); orb_kit_row_set_text(row, roles[i], done); quiet();
        lv_obj_delete(list); quiet();
    }
}

static void row_effect_refresh(lv_obj_t *root)
{
    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, SIM_D, SIM_D);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_bottom(list, 400, 0);
    lv_obj_t *row = orb_kit_row_create(list, &(orb_kit_row_config_t){
        .title = "Living room", .glyph = ORB_ICON_LIGHTBULB, .accent = ORB_GRAY4,
    });
    lv_obj_set_y(row, 190);
    orb_kit_row_set_live_text_rolling(row, ORB_KIT_ROW_TEXT_SUBTITLE, "12 lights on", "888 lights on");
    orb_kit_row_set_live_accent(row, ORB_GRAY4, lv_color_white());
    require(orb_kit_list_apply(list), "live row preparation failed");
    quiet();
    lv_obj_t *value = find_text(list, "12 lights on");
    require(value != NULL, "live row number missing");
    orb_kit_row_set_live_text_rolling(row, ORB_KIT_ROW_TEXT_SUBTITLE, "13 lights on", NULL);
    orb_kit_row_set_text_colour(row, ORB_KIT_ROW_TEXT_SUBTITLE, ORB_KIT_SECONDARY);
    require(lv_obj_get_style_text_opa(value, 0) == LV_OPA_TRANSP,
        "row colour update exposed target ink during a digit roll");
    advance(80);
    require(orb_kit_value_roll_semantic_text(value, NULL), "row digit roll stopped on a colour refresh");
    quiet();
    require(lv_obj_get_style_text_opa(value, 0) == ORB_KIT_SECONDARY_OPA,
        "row roll failed to restore its secondary opacity");
    orb_kit_row_set_live_text(row, ORB_KIT_ROW_TEXT_SUBTITLE, "Updating", NULL);
    orb_kit_row_set_waiting_since(row, ORB_KIT_ROW_TEXT_SUBTITLE, true, s_now);
    advance(400);
    lv_opa_t opacity = lv_obj_get_style_text_opa(value, 0);
    orb_kit_row_set_text_colour(row, ORB_KIT_ROW_TEXT_SUBTITLE, ORB_KIT_SECONDARY);
    require(lv_obj_get_style_text_opa(value, 0) == opacity,
        "row colour refresh reset the waiting breath sample");
    orb_kit_row_set_waiting(row, ORB_KIT_ROW_TEXT_SUBTITLE, false);
    require(lv_obj_get_style_text_opa(value, 0) == ORB_KIT_SECONDARY_OPA,
        "row waiting completion failed to restore its secondary opacity");
    lv_obj_t *disc = NULL;
    for (unsigned i = 0; i < lv_obj_get_child_count(list); i++) {
        lv_obj_t *child = lv_obj_get_child(list, i);
        if (lv_obj_is_floating(child) && lv_obj_get_style_bg_opa(child, 0) == LV_OPA_COVER &&
            find_text(child, ORB_ICON_LIGHTBULB)) disc = child;
    }
    require(disc != NULL, "native live disc missing");
    for (unsigned shoulder = 0; shoulder < 2; shoulder++) {
        lv_obj_set_y(row, shoulder ? 88 : 190);
        orb_kit_list_refresh(list);
        orb_kit_row_set_live_accent(row, ORB_GRAY4, lv_color_white());
        quiet();
        lv_opa_t expected = shoulder ? LV_OPA_50 : LV_OPA_COVER;
        require(orb_round_list_row_opacity(row) == expected, "round pose opacity disagrees with the known shoulder");
        require(lv_obj_get_style_opa(disc, 0) == expected, "native disc lost the row's shoulder opacity");
        orb_kit_row_set_live_accent_animated(row, ORB_KIT_HOME, lv_color_black());
        advance(40);
        require(orb_kit_motion_timer_running(), "row fill did not begin");
        require(lv_obj_get_style_opa(disc, 0) == expected, "light-up replaced the shoulder opacity");
        orb_kit_row_set_live_accent_animated(row, ORB_KIT_HOME, lv_color_black());
        require(orb_kit_motion_timer_running(), "unchanged row refresh canceled its light-up");
        advance(40);
        lv_refr_now(lv_display_get_default());
        sim_display_rgb(s_before);
        lv_obj_invalidate(root);
        lv_refr_now(lv_display_get_default());
        sim_display_rgb(s_after);
        require(!memcmp(s_before, s_after, sizeof(s_before)),
            "live disc partial redraw differs from full redraw at a centre or shoulder pose");
        lv_obj_send_event(list, LV_EVENT_SCROLL_BEGIN, NULL);
        require(!orb_kit_motion_timer_running(), "scroll failed to complete a live row fill");
        orb_kit_row_set_enabled(row, false);
        require(lv_obj_get_style_opa(disc, 0) == (unsigned)expected * ORB_KIT_DISABLED_OPA / 255,
            "disabled live disc lost its composed shoulder opacity");
        orb_kit_row_set_enabled(row, true);
    }
    lv_obj_delete(list);
    quiet();
}

static void wait_and_check(lv_obj_t *root)
{
    lv_obj_t *waiting = label(root, "Loading");
    quiet();
    orb_kit_breathe(waiting, true);
    unsigned changes = 0;
    lv_opa_t previous = LV_OPA_COVER;
    for (unsigned i = 0; i < 125; i++) {
        advance(8);
        lv_opa_t opacity = lv_obj_get_style_text_opa(waiting, 0);
        changes += previous != opacity;
        previous = opacity;
    }
    require(changes > 0 && changes <= 10, "waiting breath exceeds 10 Hz or never breathes");
    require(previous >= LV_OPA_50 && previous < LV_OPA_COVER, "breath left its opacity range");
    orb_kit_breathe(waiting, false);
    require(lv_obj_get_style_text_opa(waiting, 0) == LV_OPA_COVER, "wait completion lost full ink");
    quiet();
    orb_kit_breathe(waiting, true);
    advance(800);
    lv_obj_set_hidden(waiting, true);
    advance(32);
    require(!orb_kit_motion_timer_running(), "hidden waiting label kept ticking");
    require(lv_obj_get_style_text_opa(waiting, 0) == LV_OPA_COVER, "hidden wait stranded dim ink");
    lv_obj_set_hidden(waiting, false);
    orb_kit_breathe(waiting, true);
    advance(32000);
    require(!orb_kit_motion_timer_running(), "breath ran longer than 30 seconds");
    unsigned frames = s_frames;
    advance(2000);
    require(s_frames == frames, "expired wait still invalidated");
    orb_kit_breathe(waiting, false);
    orb_kit_breathe(waiting, true);
    orb_kit_motion_set_active(root, false);
    require(lv_obj_get_style_text_opa(waiting, 0) == LV_OPA_COVER && !orb_kit_motion_timer_running(),
        "covered or dimmed wait did not finish immediately");
    orb_kit_motion_set_active(root, true);
    orb_ui_set_reduce_motion(true);
    orb_kit_breathe(waiting, false);
    orb_kit_breathe(waiting, true);
    require(!orb_kit_motion_timer_running(), "Reduce Motion started a breath");
    orb_ui_set_reduce_motion(false);
    orb_kit_breathe(waiting, false);
    orb_kit_breathe(waiting, true);
    lv_obj_delete(waiting);
    quiet();
    lv_obj_t *check = orb_kit_check_create(root, ORB_KIT_PLAY, false);
    lv_obj_set_pos(check, 210, 210);
    advance(80);
    sim_display_rgb(s_before);
    advance(280);
    sim_display_rgb(s_after);
    require(memcmp(s_before, s_after, sizeof(s_before)), "check did not draw two strokes over time");
    quiet();
    lv_obj_delete(check);
    check = orb_kit_check_create(root, ORB_KIT_PLAY, false);
    lv_obj_delete(check);
    quiet();
}
int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    require(sim_display_create() != NULL, "display unavailable");
    lv_display_add_event_cb(lv_display_get_default(), rendered, LV_EVENT_RENDER_READY, NULL);
    orb_ui_init(NULL);
    require(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK, "fonts unavailable");
    lv_obj_t *root = lv_screen_active();
    lv_obj_set_scrollable(root, false);
    digit_roll(root);
    digit_length_roll(root);
    disc_and_pop(root);
    transformed_disc_damage(root);
    remote_pop_arrival(root);
    composed_arrival(root);
    row_effect_refresh(root);
    native_row_waits(root);
    wait_and_check(root);
    wait_budget_and_resume(root);
    wait_contrast_floor(root);
    staged_wait(root);
    printf("alive effects: %u checks passed\n", s_checks);
    return EXIT_SUCCESS;
}
