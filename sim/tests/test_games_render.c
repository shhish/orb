/* Compare the retained panel pixels with the exact same frozen game redrawn.
 * Full invalidations here are test oracles, never the game's frame path. */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "orb_fonts.h"
#include "orb_game_view.h"
#include "orb_game_pop.h"
#include "orb_kit.h"
#include "orb_ui.h"
#include "sim_display.h"
#include "src/draw/lv_draw_private.h"
#include "src/libs/lodepng/lodepng.h"

#define FRAME_BYTES (SIM_D * SIM_D * 3)

static uint32_t s_now;
static unsigned s_checks;
static uint8_t s_partial[FRAME_BYTES], s_full[FRAME_BYTES];
static int s_action = -1;
static unsigned s_actions;
static orb_game_state_t *s_model;
static lv_indev_data_t s_input;
static uint32_t s_largest_damage;
static unsigned s_damage_events, s_render_events;
static bool s_measure_flush;
static uint32_t s_flushed_pixels;
static unsigned s_budget_stage, s_budget_controls, s_budget_frame;
static bool s_measure_bead_tasks;
static unsigned s_bead_tasks, s_bead_body_tasks, s_bead_highlight_tasks, s_bead_off_clip_tasks;

/* A passive test unit observes actual tasks after metadata allocation. It
 * never takes work or changes a renderer's preference, and is absent from
 * firmware. Counting view-loop branches would not catch redundant LVGL work. */
static int32_t count_bead_task(lv_draw_unit_t *unit, lv_draw_task_t *task)
{
    (void)unit;
    if (!s_measure_bead_tasks) return 0;
    lv_draw_fill_dsc_t *fill = lv_draw_task_get_fill_dsc(task);
    if (!fill) return 0;
    int width = lv_area_get_width(&task->area), height = lv_area_get_height(&task->area);
    bool highlight = width == 3 && height == 3 &&
        lv_color_eq(fill->color, lv_color_hex(0xffb3c3));
    uint32_t colour = lv_color_to_u32(fill->color);
    bool body = (width == 11 || width == 7 || width == 5) && height == width &&
        (colour >> 16 & 255u) >= 226 && (colour >> 8 & 255u) >= 49 &&
        (colour >> 8 & 255u) <= 54 && (colour & 255u) >= 84 && (colour & 255u) <= 93;
    if (!body && !highlight) return 0;
    ++s_bead_tasks;
    s_bead_body_tasks += body;
    s_bead_highlight_tasks += highlight;
    /* One pixel of conservative AA coverage is intentional in bounds(). */
    lv_area_t area = task->area;
    lv_area_increase(&area, 1, 1);
    if (area.x2 < task->clip_area.x1 || area.x1 > task->clip_area.x2 ||
        area.y2 < task->clip_area.y1 || area.y1 > task->clip_area.y2)
        ++s_bead_off_clip_tasks;
    return 0;
}

static int32_t passive_dispatch(lv_draw_unit_t *unit, lv_layer_t *layer)
{
    (void)unit; (void)layer;
    return 0;
}

static uint64_t monotonic_us(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000u + (uint64_t)time.tv_nsec / 1000u;
}

static void check(bool value, const char *why)
{
    ++s_checks;
    if (value) return;
    fprintf(stderr, "game render: %s (%u ms)\n", why, s_now);
    exit(EXIT_FAILURE);
}

static uint32_t tick(void) { return s_now; }

static void action(int value, float x, float y, void *context)
{
    (void)x; (void)y; (void)context;
    s_action = value;
    ++s_actions;
    if (s_model && value != ORB_GAME_CLOSE)
        orb_game_action(s_model, (orb_game_action_t)value, x, y);
}

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    *data = s_input;
}

static void tap(lv_indev_t *pointer, int32_t x, int32_t y)
{
    s_input.point = (lv_point_t){x, y};
    s_input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(pointer);
    s_input.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
}

static void damage(lv_event_t *event)
{
    ++s_damage_events;
    const lv_area_t *area = lv_event_get_param(event);
    uint32_t pixels = (uint32_t)(lv_area_get_width(area) * lv_area_get_height(area));
    if (pixels > s_largest_damage) s_largest_damage = pixels;
}

static void rendered(lv_event_t *event)
{
    (void)event;
    ++s_render_events;
}

static void flushed(lv_event_t *event)
{
    if (!s_measure_flush) return;
    const lv_area_t *area = lv_event_get_param(event);
    s_flushed_pixels += (uint32_t)lv_area_get_size(area);
}

static lv_obj_t *find_text(lv_obj_t *object, const char *text)
{
    if (!lv_obj_is_visible(object)) return NULL;
    if (lv_obj_check_type(object, &lv_label_class) && !strcmp(lv_label_get_text(object), text)) return object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = find_text(lv_obj_get_child(object, (int32_t)i), text);
        if (found) return found;
    }
    return NULL;
}

/* Geometry is inspected only after a semantic caption locates its control;
 * it never selects an input target by a private child index. */
static lv_obj_t *find_size(lv_obj_t *object, int width, int height)
{
    if (!object || !lv_obj_is_visible(object)) return NULL;
    if (lv_obj_get_width(object) == width && lv_obj_get_height(object) == height) return object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = find_size(lv_obj_get_child(object, (int32_t)i), width, height);
        if (found) return found;
    }
    return NULL;
}

static bool has_rim_suffix(lv_obj_t *object, const char *suffix)
{
    if (!lv_obj_is_visible(object)) return false;
    const char *semantic = orb_kit_rim_text_get(object);
    if (semantic) {
        size_t length = strlen(semantic), tail = strlen(suffix);
        if (length >= tail && !strcmp(semantic + length - tail, suffix)) return true;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        if (has_rim_suffix(lv_obj_get_child(object, (int32_t)i), suffix)) return true;
    return false;
}

static bool has_rim(lv_obj_t *object, const char *text)
{
    if (!lv_obj_is_visible(object)) return false;
    const char *semantic = orb_kit_rim_text_get(object);
    if (semantic && !strcasecmp(semantic, text)) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        if (has_rim(lv_obj_get_child(object, (int32_t)i), text)) return true;
    return false;
}

static bool has_text(lv_obj_t *object, const char *text)
{
    if (find_text(object, text)) return true;
    /* In-play conditions and Marble's HUD take the top rim (review #9/#13/#35). */
    if (has_rim(object, text)) return true;
    /* Design 2.0 headings are real curved kit titles, with a semantic reading
     * API instead of label children. These checks retain scene identity. */
    if (!strcmp(text, "Paused")) return has_rim_suffix(object, " · PAUSED");
    if (!strcmp(text, "Garden")) return has_rim_suffix(object, " · GARDEN");
    if (!strcmp(text, "Planted")) return has_rim_suffix(object, " · PLANTED");
    if (!strcmp(text, "Round over")) return has_rim_suffix(object, " · ROUND OVER");
    return false;
}

static unsigned child_count(lv_obj_t *object)
{
    unsigned count = lv_obj_get_child_count(object);
    unsigned total = count;
    for (unsigned i = 0; i < count; ++i) total += child_count(lv_obj_get_child(object, (int32_t)i));
    return total;
}

static void save_rgb(const char *path, const uint8_t *rgb)
{
    unsigned char *png = NULL;
    size_t size = 0;
    if (lodepng_encode24(&png, &size, rgb, SIM_D, SIM_D)) return;
    FILE *file = fopen(path, "wb");
    if (file) { (void)fwrite(png, 1, size, file); fclose(file); }
    lv_free(png);
}

static void frozen_frame(lv_display_t *display)
{
    lv_refr_now(display);
    sim_display_rgb(s_partial);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(display);
    sim_display_rgb(s_full);
    if (memcmp(s_partial, s_full, sizeof(s_full))) {
        unsigned x1 = SIM_D, y1 = SIM_D, x2 = 0, y2 = 0, count = 0;
        for (unsigned y = 0; y < SIM_D; ++y) for (unsigned x = 0; x < SIM_D; ++x) {
            size_t at = (y * SIM_D + x) * 3;
            if (!memcmp(s_partial + at, s_full + at, 3)) continue;
            if (x < x1) x1 = x;
            if (y < y1) y1 = y;
            if (x > x2) x2 = x;
            if (y > y2) y2 = y;
            if (count < 16)
                fprintf(stderr, "  (%u,%u): partial=%u,%u,%u full=%u,%u,%u\n", x, y,
                        s_partial[at], s_partial[at + 1], s_partial[at + 2],
                        s_full[at], s_full[at + 1], s_full[at + 2]);
            ++count;
        }
        fprintf(stderr, "game render mismatch: %u pixels [%u,%u]-[%u,%u]; budget=%u/%u/%u\n", count, x1, y1, x2, y2,
                s_budget_stage, s_budget_controls, s_budget_frame);
        save_rgb("/tmp/orb-game-partial.png", s_partial);
        save_rgb("/tmp/orb-game-full.png", s_full);
    }
    check(!memcmp(s_partial, s_full, sizeof(s_full)), "partial/full pixels differ");
}

static void screen_isolation(lv_display_t *display)
{
    /* A top-layer overlay is insufficient: the shell's clock can invalidate
     * the full active screen beneath it. A separate active game screen leaves
     * those updates pending without converting them into game-frame damage. */
    lv_obj_t *shell_screen = lv_screen_active();
    lv_obj_t *clock = lv_label_create(shell_screen);
    orb_text_apply(clock, ORB_TEXT_BODY, ORB_TEXT_TABULAR, 0);
    lv_label_set_text(clock, "10:09");
    lv_obj_set_pos(clock, 180, 200);
    lv_refr_now(display);

    lv_obj_t *game_screen = lv_obj_create(NULL);
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_MARBLE, 0x4f5242u);
    orb_game_view_t *view = orb_game_view_create(game_screen, ORB_GAME_MARBLE, action, NULL);
    check(view != NULL, "create isolated game view");
    orb_game_view_update(view, &state, 0, false, false);
    lv_screen_load(game_screen);
    lv_timer_handler();
    lv_refr_now(display);
    (void)sim_display_take_changed();
    unsigned renders = s_render_events;
    s_damage_events = 0;
    s_largest_damage = 0;

    lv_label_set_text(clock, "10:10");
    lv_obj_update_layout(shell_screen);
    lv_obj_invalidate(clock);
    lv_obj_invalidate(shell_screen);
    check(s_damage_events == 0, "inactive shell label/root dirties game display");
    lv_timer_handler();
    lv_refr_now(display);
    check(s_render_events == renders, "inactive shell update renders a game frame");
    check(!sim_display_take_changed(), "inactive shell update flushes game pixels");

    for (unsigned i = 0; i < 12; ++i)
        orb_game_step(&state, 0.7f, 0, true, false);
    s_now += 33;
    orb_game_view_update(view, &state, 0, false, false);
    check(s_damage_events > 0, "moving isolated sprite fails to invalidate");
    check(s_largest_damage < SIM_D * SIM_D / 2, "isolated sprite dirties a broad screen area");
    lv_refr_now(display);
    check(s_render_events > renders, "moving isolated sprite fails to render");
    check(sim_display_take_changed(), "moving isolated sprite fails to flush");

    lv_screen_load(shell_screen);
    orb_game_view_destroy(view);
    lv_obj_delete(game_screen);
    lv_refr_now(display);
    check(lv_screen_active() == shell_screen, "exit fails to restore original shell screen");
    check(!strcmp(lv_label_get_text(clock), "10:10"), "inactive shell update is lost on restore");
    lv_obj_delete(clock);
    check(child_count(shell_screen) == 0, "isolated screen cleanup leaks widgets");
}

static void update(orb_game_view_t *view, const orb_game_state_t *state,
                   uint32_t best, bool paused, bool error)
{
    orb_game_view_update(view, state, best, paused, error);
    lv_obj_update_layout(lv_screen_active());
}

static void pointer_action(lv_indev_t *pointer, int x, int y, int expected, const char *why)
{
    /* Real pointer dispatch remains the oracle. Locate kit actions from their
     * visible ink, so old 72/60/48px coordinates cannot accidentally tap a
     * neighbouring control after the 64/52px Design 2.0 migration. */
    lv_obj_t *label = NULL;
    if (expected == ORB_GAME_RESTART) label = find_text(lv_screen_active(), "Play again");
    else if (expected == ORB_GAME_FINISH) label = find_text(lv_screen_active(), "End round");
    else if (expected == ORB_GAME_CLOSE) {
        label = find_text(lv_screen_active(), "Exit");
        if (!label) label = find_text(lv_screen_active(), "Done");
        if (!label) label = find_text(lv_screen_active(), ORB_ICON_X);
    } else if (expected == ORB_GAME_PAUSE) label = find_text(lv_screen_active(), "Resume");
    else if (expected == ORB_GAME_PRIMARY) {
        label = find_text(lv_screen_active(), "Next stage");
        if (!label) label = find_text(lv_screen_active(), "Finish");
    } else if (expected == ORB_GAME_GARDEN) {
        label = find_text(lv_screen_active(), "Garden");
        if (!label) label = find_text(lv_screen_active(), ORB_ICON_STAR);
        if (!label) label = find_text(lv_screen_active(), "Done");
    } else if (expected == ORB_GAME_CONTROLS) label = find_text(lv_screen_active(), "Touch controls");
    if (label) {
        lv_obj_t *target = label;
        while (target && !lv_obj_is_clickable(target)) target = lv_obj_get_parent(target);
        if (expected == ORB_GAME_CONTROLS && target) {
            lv_obj_t *toggle = orb_kit_row_switch(target);
            if (toggle) target = toggle;
        }
        check(target != NULL, "visible action has no tappable ancestor");
        lv_area_t box; lv_obj_get_coords(target, &box);
        x = (box.x1 + box.x2) / 2; y = (box.y1 + box.y2) / 2;
    }
    s_actions = 0;
    tap(pointer, x, y);
    check(s_actions == 1 && s_action == expected, why);
}

static void capture(lv_display_t *display, const char *directory,
                    orb_game_kind_t kind, const char *suffix)
{
    char path[1024];
    frozen_frame(display);
    snprintf(path, sizeof(path), "%s/game-%s-%s.png", directory, orb_game_id(kind), suffix);
    check(sim_display_save_png(path), "save game fixture PNG");
}

static void trigger_reward(orb_game_state_t *state)
{
    switch (state->kind) {
    case ORB_GAME_MARBLE:
        state->game.marble.x = state->game.marble.y = 0;
        state->game.marble.vx = state->game.marble.vy = 0;
        orb_game_step(state, 0, 0, true, false);
        break;
    case ORB_GAME_ORBIT:
        orb_game_action(state, ORB_GAME_PRIMARY, 0, 0);
        break;
    case ORB_GAME_SNAKE: {
        state->game.snake.food = state->game.snake.cells[0];
        for (unsigned i = 0; i < 12; ++i) orb_game_step(state, 0, 0, true, false);
        break;
    }
    case ORB_GAME_POP:
        orb_game_action(state, ORB_GAME_PRIMARY, 0, 0);
        break;
    default: break;
    }
}

static void advance_clock(orb_game_view_t *view, const orb_game_state_t *state,
                          uint32_t best, bool paused, bool error, uint32_t elapsed)
{
    /* The material timer must see the same monotonic cadence as the simulator;
     * jumping to its end before its first callback is a different lifecycle. */
    for (uint32_t ms = 0; ms < elapsed; ++ms) {
        ++s_now;
        if (ms % 33 == 0) update(view, state, best, paused, error);
        lv_timer_handler();
    }
}

static unsigned transition_oracles(lv_display_t *display)
{
    /* Synthetic unstarted states retain renderer fallback coverage; picker
     * launches are already playing in the lifecycle fixtures below. */
    static const uint32_t frames[] = {0, 16, 48, 96, 176, 320, 600, 800};
    unsigned compared = 0;
    for (unsigned mode = 0; mode < 3; ++mode) {
        orb_ui_set_reduce_motion(mode == 1);
        orb_game_state_t state;
        orb_game_init(&state, ORB_GAME_ORBIT, 0x4f5242u);
        state.started = false;
        orb_game_view_t *view = orb_game_view_create(lv_screen_active(), state.kind, action, NULL);
        check(view != NULL, "create transition fixture");
        update(view, &state, 120, false, false);
        advance_clock(view, &state, 120, false, false, 800);
        update(view, &state, 120, false, false);
        frozen_frame(display);
        for (unsigned transition = 0; transition < 3; ++transition) {
            orb_game_action(&state, transition == 0 ? ORB_GAME_START : ORB_GAME_PAUSE, 0, 0);
            uint32_t begin = s_now;
            update(view, &state, 120, false, false);
            for (unsigned frame = 0; frame < sizeof(frames) / sizeof(frames[0]); ++frame) {
                advance_clock(view, &state, 120, false, false, begin + frames[frame] - s_now);
                /* Setting changes during both directions of materialisation
                 * must not leave old rims or stale score/life pixels. */
                if (mode == 2 && frame == 3) orb_ui_set_reduce_motion(true);
                if (mode == 2 && frame == 5) orb_ui_set_reduce_motion(false);
                lv_timer_handler();
                update(view, &state, 120, false, false);
                frozen_frame(display);
                ++compared;
            }
        }
        orb_game_view_destroy(view);
    }
    orb_ui_set_reduce_motion(false);
    return compared;
}

static void round_lifecycle(lv_display_t *display, lv_indev_t *pointer)
{
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_POP, 0x4f5242u);
    s_model = &state;
    orb_game_view_t *view = orb_game_view_create(lv_screen_active(), state.kind, action, NULL);
    check(view != NULL, "create repeat-round fixture");
    update(view, &state, 120, false, false);
    advance_clock(view, &state, 120, false, false, 800);
    check(state.started && !state.user_paused && !state.game_over,
          "repeat-round entry is already playing");
    check(!has_text(lv_screen_active(), "Play") && !has_text(lv_screen_active(), "Tap to Play"),
          "repeat-round entry has no second Play action");
    pointer_action(pointer, 101, 101, ORB_GAME_TAP, "cold Close does not intercept play");
    pointer_action(pointer, 365, 101, ORB_GAME_TAP, "cold settings do not intercept play");
    pointer_action(pointer, 233, 399, ORB_GAME_TAP, "cold entry accepts playfield input immediately");
    advance_clock(view, &state, 120, false, false, 800);

    state.score = 180;
    pointer_action(pointer, 233, 54, ORB_GAME_PAUSE, "Pop score taps to pause");
    update(view, &state, 180, false, false);
    advance_clock(view, &state, 180, false, false, 800);
    check(has_text(lv_screen_active(), "End round"), "Pop pause exposes End Round");
    pointer_action(pointer, 233, 233, ORB_GAME_FINISH, "paused End Round dispatches once");
    check(state.game_over, "End Round reaches the result through actual logic");
    update(view, &state, 180, false, false);
    advance_clock(view, &state, 180, false, false, 800);
    check(has_text(lv_screen_active(), "Best 180"), "first improved round reports its updated best");
    frozen_frame(display);

    pointer_action(pointer, 233, 314, ORB_GAME_RESTART, "first result restarts");
    update(view, &state, 180, false, false);
    pointer_action(pointer, 101, 101, ORB_GAME_TAP, "outgoing result Close is not actionable");
    advance_clock(view, &state, 180, false, false, 800);
    state.score = 170;
    state.game_over = true;
    update(view, &state, 180, false, false);
    advance_clock(view, &state, 180, false, false, 800);
    check(has_text(lv_screen_active(), "Round over"), "second round uses the newly raised best baseline");
    check(has_text(lv_screen_active(), "Best 180"), "a weaker round preserves the authoritative best");
    frozen_frame(display);

    pointer_action(pointer, 233, 314, ORB_GAME_RESTART, "second result restarts");
    update(view, &state, 180, false, false);
    advance_clock(view, &state, 180, false, false, 800);
    state.score = 190;
    state.game_over = true;
    update(view, &state, 190, false, false);
    advance_clock(view, &state, 190, false, false, 800);
    check(has_text(lv_screen_active(), "Best 190"), "later improvement updates the authoritative best");
    frozen_frame(display);
    orb_game_view_destroy(view);
    s_model = NULL;
}

static void pointer_state(lv_indev_t *pointer, int32_t x, int32_t y, bool pressed)
{
    s_input.point = (lv_point_t){x, y};
    s_input.state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
}

static void cold_garden(lv_display_t *display, lv_indev_t *pointer)
{
    /* Direct paused garden entry exercises a freshly built reading surface
     * without relying on a preceding start page to materialise Close. */
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_MARBLE, 0x4f5242u);
    state.user_paused = true;
    state.game.marble.garden_open = true;
    state.game.marble.keepsake_count = ORB_MARBLE_KEEPSAKES_MAX;
    state.game.marble.stages_cleared = ORB_MARBLE_STAGES;
    for (unsigned i = 0; i < ORB_MARBLE_KEEPSAKES_MAX; ++i)
        state.game.marble.keepsakes[i] = (orb_marble_keepsake_t){(uint8_t)(i % 3), (uint8_t)(1 + i % 3), 0, 0};
    s_model = &state;
    orb_game_view_t *view = orb_game_view_create(lv_screen_active(), state.kind, action, NULL);
    check(view != NULL, "create garden without a preceding start page");
    update(view, &state, 0, false, false);
    advance_clock(view, &state, 0, false, false, 900);
    frozen_frame(display);
    check(!has_text(lv_screen_active(), ORB_ICON_X), "garden has no redundant floating Close control");
    check(has_text(lv_screen_active(), "Garden"), "cold garden renders its heading");
    /* The Garden sheet's own primary is Done (review #31); nothing from the
     * result or Pause may leak onto it. */
    check(!has_text(lv_screen_active(), "Play again") && !has_text(lv_screen_active(), "Exit") &&
          !has_text(lv_screen_active(), "Touch controls"),
          "cold garden exposes no controls from result or pause");
    check(has_text(lv_screen_active(), "12 keepsakes"), "cold garden retains all keepsakes");
    pointer_action(pointer, 233, 380, ORB_GAME_GARDEN, "cold garden returns through its kit primary");
    orb_game_view_destroy(view);
    s_model = NULL;
}

static unsigned planting_motion(lv_display_t *display)
{
    /* Exercise the real stage-clear rule with a frozen model: only the view's
     * brief bloom/flight (or Reduce Motion cross-fade) has visual work to do.
     * Comparing every retained frame catches stale petals and flight trails. */
    unsigned frames = 0;
    for (unsigned mode = 0; mode < 2; ++mode) {
        orb_ui_set_reduce_motion(mode != 0);
        orb_game_state_t state;
        orb_game_init(&state, ORB_GAME_MARBLE, 0x4f5242u);
        state.started = true;
        state.reduce_motion = mode != 0;
        orb_game_view_t *view = orb_game_view_create(lv_screen_active(), state.kind, action, NULL);
        check(view != NULL, "create planting motion fixture");
        update(view, &state, 0, false, false);
        advance_clock(view, &state, 0, false, false, 800);
        frozen_frame(display);
        unsigned objects = child_count(lv_screen_active());
        trigger_reward(&state);
        check(state.game.marble.stage_cleared, "planting animation follows actual goal arrival");
        update(view, &state, state.score, false, false);
        frozen_frame(display);
        for (unsigned frame = 0; frame < 48; ++frame) {
            advance_clock(view, &state, state.score, false, false, 33);
            check(child_count(lv_screen_active()) == objects, "planting animation creates/deletes widgets");
            frozen_frame(display);
            if ((mode != 0 && frame == 6) || frame == 47) {
                unsigned token_pixels = 0;
                for (unsigned y = 315; y < 352; ++y) for (unsigned x = 210; x < 257; ++x) {
                    size_t at = (y * SIM_D + x) * 3;
                    if (s_full[at] > 30 || s_full[at + 1] > 30 || s_full[at + 2] > 30)
                        ++token_pixels;
                }
                check(token_pixels > 200, "earned keepsake remains visible after arrival or cross-fade");
            }
            ++frames;
        }
        unsigned renders = s_render_events;
        advance_clock(view, &state, state.score, false, false, 500);
        lv_refr_now(display);
        check(s_render_events == renders, "planting reward rests after its bounded response");
        check(state.game.marble.keepsake_count == 1, "animation never awards extra keepsakes");
        orb_game_view_destroy(view);
    }
    orb_ui_set_reduce_motion(false);
    return frames;
}

static unsigned conservatory(lv_display_t *display, lv_indev_t *pointer, const char *directory)
{
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_MARBLE, 0x4f5242u);
    s_model = &state;
    orb_game_view_t *view = orb_game_view_create(lv_screen_active(), state.kind, action, NULL);
    check(view != NULL, "create Conservatory fixture");
    update(view, &state, 0, false, false);
    advance_clock(view, &state, 0, false, false, 800);
    check(state.started && !state.user_paused && !state.game_over,
          "Conservatory enters a playing round");
    check(!has_text(lv_screen_active(), "Marble") && !has_text(lv_screen_active(), "Play") &&
          !has_text(lv_screen_active(), "Garden") && !has_text(lv_screen_active(), "Touch controls"),
          "Conservatory entry skips the title and second Play scene");
    capture(display, directory, state.kind, "conservatory-entry-playing");
    check(!has_text(lv_screen_active(), "Turn rim to rewind"),
          "playing arena never hides a collision wall under rewind teaching");
    check(!has_text(lv_screen_active(), ORB_ICON_PAUSE), "Marble arena has no pause chrome");
    check(!has_text(lv_screen_active(), ORB_ICON_X), "Marble arena has no Close chrome");
    check(!has_text(lv_screen_active(), ORB_ICON_CHEVRON_LEFT ORB_ICON_CHEVRON_LEFT),
          "Marble arena has no rewind chrome");
    pointer_action(pointer, 365, 101, ORB_GAME_PAUSE, "single arena tap opens Pause");
    update(view, &state, 0, false, false);
    advance_clock(view, &state, 0, false, false, 800);
    capture(display, directory, state.kind, "conservatory-pause");
    lv_obj_t *instruction = find_text(lv_screen_active(), "Turn rim to rewind");
    check(instruction != NULL, "Pause explains rim rewind without concealing play");
    check(lv_obj_get_height(instruction) <=
          lv_font_get_line_height(lv_obj_get_style_text_font(instruction, 0)) + 2,
          "paused rewind instruction fits one readable line");
    check(!has_text(lv_screen_active(), "Garden"), "Marble Pause shares the common three-action template");
    pointer_action(pointer, 233, 155, ORB_GAME_FINISH, "Marble explicitly ends its round before the Garden");
    update(view, &state, 0, false, false);
    advance_clock(view, &state, 0, false, false, 800);
    pointer_action(pointer, 365, 101, ORB_GAME_GARDEN, "result Garden toolbar opens the keepsakes");
    check(state.game.marble.garden_open && state.game_over, "Garden preserves the ended round");
    update(view, &state, 0, false, false);
    advance_clock(view, &state, 0, false, false, 800);
    capture(display, directory, state.kind, "conservatory-garden-empty");
    pointer_action(pointer, 233, 399, ORB_GAME_GARDEN, "garden returns to the same result");
    check(!state.game.marble.garden_open && state.game_over, "Garden return retains the result");
    update(view, &state, 0, false, false);
    advance_clock(view, &state, 0, false, false, 800);
    pointer_action(pointer, 233, 316, ORB_GAME_RESTART, "Marble starts a new round after the Garden");
    orb_game_action(&state, ORB_GAME_PAUSE, 0, 0);
    update(view, &state, 0, false, false);
    advance_clock(view, &state, 0, false, false, 800);
    lv_obj_t *done_label = find_text(lv_screen_active(), "Exit");
    check(done_label && has_text(lv_screen_active(), "End round"),
          "Marble separates ending its round from leaving the game");
    check(!has_text(lv_screen_active(), ORB_ICON_X), "Marble Pause has no floating close control");
    pointer_action(pointer, 233, 415, ORB_GAME_CLOSE, "quiet Done exits Marble once");
    lv_obj_t *controls_row = lv_obj_get_parent(find_text(lv_screen_active(), "Touch controls"));
    lv_obj_t *track = find_size(controls_row, 80, 46);
    lv_obj_t *knob = find_size(track, 38, 38);
    check(track && knob, "Touch controls exposes a visible toggle track and knob");
    check(!orb_kit_switch_get_checked(track), "Touch controls starts with the actual unchecked state");
    int knob_before = lv_obj_get_x(knob);
    pointer_action(pointer, 233, 251, ORB_GAME_CONTROLS, "pause exposes touch alternative");
    check(state.controls, "touch setting reaches Conservatory model");
    update(view, &state, 0, false, false);
    advance_clock(view, &state, 0, false, false, 300);
    check(lv_obj_get_x(knob) > knob_before, "real touch setting tap moves the toggle knob on");
    check(orb_kit_switch_get_checked(track), "Touch controls reflects the model after actual row activation");
    pointer_action(pointer, 233, 343, ORB_GAME_PAUSE, "Conservatory resumes with touch controls");
    update(view, &state, 0, false, false);
    advance_clock(view, &state, 0, false, false, 800);
    check(!has_text(lv_screen_active(), ORB_ICON_ARROW_LEFT) &&
          !has_text(lv_screen_active(), ORB_ICON_ARROW_RIGHT) &&
          !has_text(lv_screen_active(), ORB_ICON_ARROW_UP) &&
          !has_text(lv_screen_active(), ORB_ICON_ARROW_DOWN),
          "Conservatory touch mode keeps the arena free of buttons");
    pointer_action(pointer, 113, 233, ORB_GAME_LEFT, "Conservatory left field tap");
    pointer_action(pointer, 353, 233, ORB_GAME_RIGHT, "Conservatory right field tap");
    pointer_action(pointer, 233, 159, ORB_GAME_UP, "Conservatory up field tap");
    pointer_action(pointer, 233, 307, ORB_GAME_DOWN, "Conservatory down field tap");
    for (unsigned i = 0; i < 12; ++i) orb_game_step(&state, 0, 0, true, false);
    s_actions = 0;
    pointer_state(pointer, 113, 233, true);
    advance_clock(view, &state, 0, false, false, 600);
    check(state.game.marble.rewind_active, "holding a steering area rewinds anywhere on screen");
    pointer_state(pointer, 113, 233, false);
    check(s_actions == 2 && s_action == ORB_GAME_REWIND_STOP && !state.user_paused,
          "steering-area hold releases without a nudge or pause");
    state.touch_fallback_hint = true;
    update(view, &state, 0, false, false);
    check(!has_text(lv_screen_active(), "Touch controls enabled"),
          "touch fallback never hides live walls behind an arena banner");
    orb_game_action(&state, ORB_GAME_PAUSE, 0, 0);
    update(view, &state, 0, false, false);
    check(state.controls && has_text(lv_screen_active(), "Touch controls"),
          "Pause exposes the actual touch mode in its labeled control row");
    orb_game_action(&state, ORB_GAME_PAUSE, 0, 0);
    update(view, &state, 0, false, false);
    state.touch_fallback_hint = false;
    update(view, &state, 0, false, false);
    check(!has_text(lv_screen_active(), "Touch controls enabled"), "retired fallback hint leaves the arena clear");
    /* Use tilt for the path oracle; touch mode deliberately ignores the IMU. */
    orb_game_action(&state, ORB_GAME_CONTROLS, 0, 0);
    update(view, &state, 0, false, false);
    unsigned objects = child_count(lv_screen_active());
    for (unsigned frame = 0; frame < 150; ++frame) {
        if (frame == 90) {
            pointer_state(pointer, 233, 399, true);
            check(!state.game.marble.rewind_active && !state.user_paused,
                  "initial contact waits to distinguish tap from hold");
            advance_clock(view, &state, 0, false, false, 600);
            check(state.game.marble.rewind_active, "whole-field long press begins continuous rewind");
            check(s_action == ORB_GAME_REWIND_START, "rewind hold dispatches START");
        }
        if (frame == 120) {
            pointer_state(pointer, 233, 399, false);
            check(!state.game.marble.rewind_active && !state.user_paused, "hold release stops rewind without pausing");
            check(s_action == ORB_GAME_REWIND_STOP, "rewind release dispatches STOP without tap");
        }
        float gate_before = state.game.marble.gates[0].angle;
        orb_game_step(&state, -0.25f, 0, true, false);
        orb_game_step(&state, -0.25f, 0, true, false);
        if (frame >= 90 && frame < 120)
            check(state.game.marble.gates[0].angle != gate_before, "gates advance while marble rewinds");
        s_now += 33;
        lv_timer_handler();
        s_largest_damage = 0;
        update(view, &state, 0, false, false);
        if (frame == 90)
            check(!has_text(lv_screen_active(), "Rewinding"), "rewind feedback stays in the marble trail");
        if (frame == 120)
            check(!has_text(lv_screen_active(), "Rewinding"), "release removes rewind mode label");
        check(s_largest_damage < SIM_D * SIM_D / 2, "Conservatory moving frame damage remains bounded");
        check(child_count(lv_screen_active()) == objects, "Conservatory frame creates no widgets");
        frozen_frame(display);
    }
    s_actions = 0;
    pointer_state(pointer, 446, 233, true);
    pointer_state(pointer, 446, 247, true);
    check(state.game.marble.rewind_active && s_action == ORB_GAME_REWIND_START,
          "tangential rim movement starts rewind without waiting for a hold");
    pointer_state(pointer, 446, 247, false);
    check(!state.game.marble.rewind_active && !state.user_paused && s_actions == 2,
          "rim release stops rewind without pausing or a stray tap");
    pointer_state(pointer, 20, 233, true);
    advance_clock(view, &state, 0, false, false, 600);
    check(state.game.marble.rewind_active, "hold on the rim starts rewind");
    pointer_state(pointer, 490, 233, true);
    check(!state.game.marble.rewind_active, "lost hold cancels rewind");
    pointer_state(pointer, 490, 233, false);
    pointer_state(pointer, 20, 233, false);
    check(!state.user_paused, "lost long contact does not pause");

    /* A goal fixture enters the same rules as physical steering; the stage
     * reward and Next stage action are never manufactured by the renderer. */
    state.game.marble.x = state.game.marble.y = 0;
    state.game.marble.vx = state.game.marble.vy = 0;
    orb_game_step(&state, 0, 0, true, false);
    check(state.game.marble.stage_cleared && state.game.marble.keepsake_count == 1,
          "reaching goal plants one keepsake");
    update(view, &state, state.score, false, false);
    advance_clock(view, &state, state.score, false, false, 1600);
    check(has_text(lv_screen_active(), "Planted"), "stage completion exposes planted receipt");
    check(!has_text(lv_screen_active(), "Done") && !has_text(lv_screen_active(), "Touch controls"),
          "stage receipt exposes no controls from result or pause");
    capture(display, directory, state.kind, "conservatory-planted");
    pointer_action(pointer, 233, 399, ORB_GAME_PRIMARY, "Next stage advances deliberately");
    check(state.game.marble.stage == 1 && !state.game.marble.stage_cleared,
          "Next stage starts second puzzle");
    update(view, &state, state.score, false, false);
    check(!has_text(lv_screen_active(), "Next stage"), "outgoing stage action hides on entering play");
    check(!has_text(lv_screen_active(), ORB_ICON_CHEVRON_LEFT ORB_ICON_CHEVRON_LEFT),
          "incoming rewind never overlaps outgoing stage action");
    advance_clock(view, &state, state.score, false, false, 800);
    check(!has_text(lv_screen_active(), ORB_ICON_CHEVRON_LEFT ORB_ICON_CHEVRON_LEFT),
          "second-stage arena remains free of rewind controls");
    check(!has_text(lv_screen_active(), "Turn rim to rewind"), "first-stage teaching never returns in later stages");
    capture(display, directory, state.kind, "conservatory-stage-two");

    orb_game_action(&state, ORB_GAME_PAUSE, 0, 0);
    orb_game_action(&state, ORB_GAME_FINISH, 0, 0);
    orb_game_action(&state, ORB_GAME_GARDEN, 0, 0);
    check(state.started && state.game_over && state.game.marble.garden_open,
          "earned keepsake Garden opens after ending the round");
    update(view, &state, state.score, false, false);
    /* The outgoing rewind retracts before the reading action materialises;
     * wait for both bounded springs before checking a genuinely idle garden. */
    advance_clock(view, &state, state.score, false, false, 1300);
    capture(display, directory, state.kind, "conservatory-garden");
    unsigned renders = s_render_events;
    advance_clock(view, &state, state.score, false, false, 500);
    lv_refr_now(display);
    check(s_render_events == renders, "idle keepsake garden produces no frames");
    check(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK, "Conservatory AX3 fonts");
    check(orb_fonts_set_bold_text(true) == ORB_FONTS_OK, "Conservatory Bold Text");
    orb_ui_set_reduce_transparency(true);
    update(view, &state, state.score, false, false);
    advance_clock(view, &state, state.score, false, false, 800);
    capture(display, directory, state.kind, "conservatory-garden-ax3-bold-rt");
    check(has_text(lv_screen_active(), "1 keepsake"), "garden shows the quiet earned-count secondary line");
    update(view, &state, state.score, false, true);
    advance_clock(view, &state, state.score, false, true, 800);
    capture(display, directory, state.kind, "conservatory-garden-save-error-ax3-bold-rt");
    lv_obj_t *warning = find_text(lv_screen_active(), "Not saved");
    lv_obj_t *garden_primary = find_text(lv_screen_active(), "Done");
    check(warning && garden_primary, "garden failure retains warning and return action");
    lv_area_t warning_box, primary_box;
    lv_obj_get_coords(warning, &warning_box);
    lv_obj_get_coords(lv_obj_get_parent(garden_primary), &primary_box);
    check(primary_box.y1 - warning_box.y2 >= 16, "garden failure clears AX3 return action");
    lv_area_t count_box;
    check(!has_text(lv_screen_active(), "1 keepsake"), "garden failure replaces its one secondary line");
    check(state.game.marble.keepsake_count == 1, "garden failure preserves the earned keepsake");
    pointer_action(pointer, 233, 399, ORB_GAME_GARDEN, "AX3 garden return stays reachable");
    update(view, &state, state.score, false, false);
    advance_clock(view, &state, state.score, false, false, 800);
    capture(display, directory, state.kind, "conservatory-result-ax3-bold-rt");
    check(state.started && state.game_over && !state.game.marble.garden_open,
          "AX3 Garden returns to the ended round");
    pointer_action(pointer, 233, 316, ORB_GAME_RESTART, "AX3 starts another round after Garden");
    update(view, &state, state.score, false, false);
    advance_clock(view, &state, state.score, false, false, 800);
    check(!has_text(lv_screen_active(), ORB_ICON_PAUSE) &&
          !has_text(lv_screen_active(), ORB_ICON_CHEVRON_LEFT ORB_ICON_CHEVRON_LEFT),
          "AX3 arena remains free of pause and rewind chrome");
    orb_game_action(&state, ORB_GAME_PAUSE, 0, 0);
    update(view, &state, state.score, false, true);
    advance_clock(view, &state, state.score, false, true, 800);
    capture(display, directory, state.kind, "conservatory-pause-save-error-ax3-bold-rt");
    warning = find_text(lv_screen_active(), "Not saved");
    lv_obj_t *resume = find_text(lv_screen_active(), "Resume");
    check(warning && resume, "pause failure retains warning and Resume");
    lv_obj_get_coords(warning, &warning_box);
    lv_obj_get_coords(lv_obj_get_parent(resume), &primary_box);
    check(primary_box.y1 - warning_box.y2 >= 16, "pause failure clears AX3 Resume action");
    lv_obj_t *pause_done = find_text(lv_screen_active(), "Exit");
    check(pause_done != NULL, "pause failure preserves quiet Done");
    lv_obj_get_coords(pause_done, &count_box);
    check(count_box.y1 - warning_box.y2 >= 16, "pause failure clears AX3 Done");
    pointer_action(pointer, 233, 155, ORB_GAME_FINISH, "AX3 End round stays reachable after a save failure");
    update(view, &state, state.score, false, true);
    advance_clock(view, &state, state.score, false, true, 800);
    pointer_action(pointer, 365, 101, ORB_GAME_GARDEN, "AX3 result Garden stays reachable");
    update(view, &state, state.score, false, true);
    advance_clock(view, &state, state.score, false, true, 800);
    lv_obj_t *paused_garden_back = find_text(lv_screen_active(), "Done");
    check(paused_garden_back != NULL, "paused Garden uses the shared Done acknowledgement");
    check(lv_obj_get_height(paused_garden_back) <=
          lv_font_get_line_height(lv_obj_get_style_text_font(paused_garden_back, 0)) + 2,
          "AX3 Bold paused Garden Back fits one line inside its capsule");
    capture(display, directory, state.kind, "conservatory-result-garden-ax3-bold-rt");
    pointer_action(pointer, 233, 399, ORB_GAME_GARDEN, "AX3 Garden Done returns to the result");
    update(view, &state, state.score, false, true);
    advance_clock(view, &state, state.score, false, true, 800);
    pointer_action(pointer, 233, 316, ORB_GAME_RESTART, "Conservatory replays after save failure");
    update(view, &state, state.score, false, true);
    check(!has_text(lv_screen_active(), "Resume"), "outgoing Resume hides on entering play");
    check(!has_text(lv_screen_active(), ORB_ICON_CHEVRON_LEFT ORB_ICON_CHEVRON_LEFT),
          "rewind does not overlap outgoing Resume");
    advance_clock(view, &state, state.score, false, true, 800);
    check(!has_text(lv_screen_active(), ORB_ICON_CHEVRON_LEFT ORB_ICON_CHEVRON_LEFT),
          "resumed arena remains free of rewind chrome");
    orb_game_view_destroy(view);
    s_model = NULL;
    check(child_count(lv_screen_active()) == 0, "Conservatory cleanup releases widgets");
    check(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK, "restore fonts after Conservatory");
    check(orb_fonts_set_bold_text(false) == ORB_FONTS_OK, "restore weight after Conservatory");
    orb_ui_set_reduce_transparency(false);
    return 150;
}

static void play_chrome(void)
{
    /* Cold entry used to expose Done/settings because specialized games never
     * visited the generic scene code. Check labels through visible ancestors,
     * so hiding only a material's hit target cannot satisfy the contract. */
    static const char *const reading[] = {"Done", "Touch controls", "End round", "Resume",
        "Tap to Play", "Play", "Play again", "Garden", ORB_ICON_X, ORB_ICON_STAR};
    for (unsigned i = 0; i < sizeof(reading) / sizeof(reading[0]); ++i)
        check(!has_text(lv_screen_active(), reading[i]), "play leaks a reading-scene control");
}

static void check_readable_line(lv_obj_t *label, const char *why);

static void scene_contract(lv_display_t *display, lv_indev_t *pointer)
{
    /* The unstarted row is a synthetic renderer fallback, not app entry. */
    for (unsigned mode = 0; mode < 2; ++mode) {
        check(orb_fonts_set_content_size(mode ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK,
              "scene contract content size");
        check(orb_fonts_set_bold_text(mode != 0) == ORB_FONTS_OK, "scene contract weight");
        orb_ui_set_reduce_transparency(mode != 0);
        orb_ui_set_reduce_motion(true);
        for (orb_game_kind_t kind = ORB_GAME_MARBLE; kind < ORB_GAME_COUNT; ++kind) {
            for (unsigned scene = 0; scene < 3; ++scene) {
                orb_game_state_t state;
                orb_game_init(&state, kind, 0x4f5242u);
                state.started = scene != 0;
                state.user_paused = scene == 2;
                state.controls = false;
                state.score = 24;
                s_model = &state;
                orb_game_view_t *view = orb_game_view_create(lv_screen_active(), kind, action, NULL);
                check(view != NULL, "create cold scene contract");
                update(view, &state, 31, false, false);
                advance_clock(view, &state, 31, false, false, 800);
                /* Cold start/play/pause must paint the same pixels as a full
                 * redraw, including freshly materialised Close and labels. */
                frozen_frame(display);
                check(has_text(lv_screen_active(), "Exit") == (scene == 2),
                      "Exit is reserved for the paused reading surface");
                if (scene == 2) {
                    check(!has_text(lv_screen_active(), ORB_ICON_X), "Pause has no floating close control");
                    pointer_action(pointer, 233, 415, ORB_GAME_CLOSE, "Pause Done is touch accessible");
                    lv_obj_t *resume_ink = find_text(lv_screen_active(), "Resume");
                    lv_color_t expected_ink = kind == ORB_GAME_POP ? ORB_KIT_HOME :
                        kind == ORB_GAME_SNAKE ? ORB_KIT_CLAUDE :
                        kind == ORB_GAME_ORBIT ? ORB_KIT_DOT_CONTRAST :
                        kind == ORB_GAME_BEADS ? ORB_KIT_SOUND : ORB_KIT_PLAY;
                    if (!lv_color_eq(lv_obj_get_style_text_color(resume_ink, 0), expected_ink))
                        fprintf(stderr, "Resume ink game=%s expected=%06x actual=%06x\n", orb_game_id(kind),
                            (unsigned)lv_color_to_u32(expected_ink),
                            (unsigned)lv_color_to_u32(lv_obj_get_style_text_color(resume_ink, 0)));
                    check(lv_color_eq(lv_obj_get_style_text_color(resume_ink, 0), expected_ink),
                        "Resume uses the game accent as ink");
                    lv_area_t resume_bounds;
                    lv_obj_get_coords(lv_obj_get_parent(resume_ink), &resume_bounds);
                    lv_area_t previous = resume_bounds, next;
                    lv_obj_t *end_round = find_text(lv_screen_active(), "End round");
                    check((end_round != NULL) == (kind != ORB_GAME_BEADS),
                        "every scored Pause exposes End round and the open-ended toy omits it");
                    if (end_round) {
                        lv_obj_get_coords(lv_obj_get_parent(end_round), &next);
                        check(next.y1 - previous.y2 - 1 >= 20,
                            "End round follows Resume with the standard control gap");
                        previous = next;
                    }
                    lv_obj_t *touch = find_text(lv_screen_active(), "Touch controls");
                    bool tilt_game = kind == ORB_GAME_MARBLE || kind == ORB_GAME_BEADS || kind == ORB_GAME_SNAKE;
                    check((touch != NULL) == tilt_game, "only tilt games expose the input-mode option");
                    if (touch) {
                        lv_obj_get_coords(lv_obj_get_parent(touch), &next);
                        check(next.y1 - previous.y2 - 1 >= 20,
                            "game options follow End round (or Resume for the toy)");
                        previous = next;
                    }
                    lv_obj_get_coords(lv_obj_get_parent(find_text(lv_screen_active(), "Exit")), &next);
                    check(next.y1 - previous.y2 - 1 >= 20,
                        "Exit is last in the shared Pause stack with the standard gap");
                    /* Sample clear material beside the label. The old fixed
                     * y299 sample now lands on Resume's correctly painted ink. */
                    size_t fill = (((resume_bounds.y1 + resume_bounds.y2) / 2) * SIM_D +
                        resume_bounds.x1 + ORB_KIT_PRIMARY_H / 3) * 3;
                    check(s_full[fill] < 180 && s_full[fill + 1] < 180 && s_full[fill + 2] < 180,
                          "Resume has a low-tint material instead of a solid accent fill");
                    if (kind != ORB_GAME_BEADS) {
                        /* Persistence feedback replaces the hint; it must not
                         * insert a second line or shift the action stack. */
                        update(view, &state, 31, false, true);
                        advance_clock(view, &state, 31, false, true, 800);
                        lv_obj_t *warning = find_text(lv_screen_active(), "Not saved");
                        check(warning != NULL, "every scored Pause uses the canonical save-error phrase");
                        lv_area_t error_resume;
                        lv_obj_get_coords(lv_obj_get_parent(find_text(lv_screen_active(), "Resume")), &error_resume);
                        check(error_resume.y1 == resume_bounds.y1 && error_resume.y2 == resume_bounds.y2,
                            "save failure shifts the shared Pause action stack");
                        update(view, &state, 31, false, false);
                        advance_clock(view, &state, 31, false, false, 800);
                    }
                    state.motion_unavailable = true;
                    update(view, &state, 31, false, false);
                    check(has_text(lv_screen_active(), "Motion unavailable"),
                        "a sensor-condition change refreshes the existing Pause status");
                    state.motion_unavailable = false;
                    update(view, &state, 31, false, false);
                    check(!has_text(lv_screen_active(), "Motion unavailable"),
                        "sensor recovery refreshes the existing Pause status");
                    if (kind == ORB_GAME_BEADS) {
                        check(has_text(lv_screen_active(), "Time not set"),
                            "Beads Pause reports missing time in its status slot");
                        orb_game_beads_set_time(&state, 10, 9);
                        update(view, &state, 31, false, false);
                        check(!has_text(lv_screen_active(), "Time not set") &&
                            has_text(lv_screen_active(), "Rest for time"),
                            "time synchronization refreshes Beads Pause without leaving it");
                    }
                }
                if (scene == 1) {
                    play_chrome();
                    check(!has_text(lv_screen_active(), orb_game_name(kind)), "game title overlaps live arena");
                    if (kind == ORB_GAME_MARBLE)
                        check(has_text(lv_screen_active(), "Stage 1 of 3 · 24"),
                            "Marble reports both the current stage and live score");
                    state.controls = true;
                    update(view, &state, 31, false, false);
                    check(has_text(lv_screen_active(), "Touch controls on"),
                        "every game reports the same explicit touch-mode condition");
                    if (kind == ORB_GAME_POP) {
                        check(!find_text(lv_screen_active(), "24") && !find_text(lv_screen_active(), "45 s"),
                            "Pop reserves the transient condition band instead of covering its score HUD");
                        advance_clock(view, &state, 31, false, false, 3100);
                        check(!has_text(lv_screen_active(), "Touch controls on"),
                            "Pop condition expires after its bounded reading interval");
                        check(find_text(lv_screen_active(), "24") && find_text(lv_screen_active(), "45 s"),
                            "Pop restores both score and countdown after the condition expires");
                        frozen_frame(display);
                    }
                    state.motion_unavailable = true;
                    update(view, &state, 31, false, false);
                    check(has_text(lv_screen_active(), "Motion unavailable"),
                        "every game reports the same explicit motion-unavailable condition");
                    state.controls = false;
                    state.motion_unavailable = false;
                    update(view, &state, 31, false, false);
                    check(!has_text(lv_screen_active(), ORB_ICON_ARROW_LEFT) &&
                          !has_text(lv_screen_active(), ORB_ICON_ARROW_RIGHT), "tilt mode exposes touch arrows");
                    if (kind == ORB_GAME_MARBLE || kind == ORB_GAME_BEADS)
                        pointer_action(pointer, 233, 380, ORB_GAME_PAUSE, "hidden Done does not intercept toy play");
                    if (kind == ORB_GAME_SNAKE) {
                        check_readable_line(find_text(lv_screen_active(), "Tilt to steer"),
                            "Snake teaches absolute steering on its first-second bottom chord");
                        frozen_frame(display);
                        size_t head_pixel = (233 * SIM_D + 233) * 3;
                        size_t body_pixel = (245 * SIM_D + 233) * 3;
                        size_t food_pixel = (137 * SIM_D + 233) * 3; /* First pellet clears the score disc. */
                        size_t wall_pixel = (233 * SIM_D + 447) * 3;
                        check(s_full[wall_pixel] >= 30 && s_full[wall_pixel + 1] >= 30 &&
                            s_full[wall_pixel + 2] >= 30,
                            "Snake exposes its arena boundary before a rim warning");
                        check(s_full[head_pixel] > 200 && s_full[head_pixel + 1] > 200 &&
                            s_full[head_pixel + 2] > 200, "Snake teaching hint retains the rendered white head");
                        check(s_full[body_pixel] < 100 && s_full[body_pixel + 1] > 150,
                            "Snake teaching hint retains the rendered green body");
                        check(s_full[food_pixel] > 200 && s_full[food_pixel + 1] > 100 &&
                            s_full[food_pixel + 2] < 100, "Snake teaching hint retains the rendered orange food");
                        state.controls = true;
                        update(view, &state, 31, false, false);
                        advance_clock(view, &state, 31, false, false, 800);
                        /* Review #16: switching input shows the shared condition
                         * line; its Start screen already teaches "Hold to steer". */
                        check_readable_line(find_text(lv_screen_active(), "Touch controls on"),
                            "Snake touch hint fits Default and AX3/Bold");
                        frozen_frame(display);
                        check(s_full[head_pixel] > 200 && s_full[food_pixel] > 200,
                            "Snake touch hint retains head and food after retained redraw");
                        lv_obj_t *left = find_text(lv_screen_active(), ORB_ICON_ARROW_LEFT);
                        lv_obj_t *right = find_text(lv_screen_active(), ORB_ICON_ARROW_RIGHT);
                        check(!left && !right, "touch Snake keeps the continuous field free of relative arrows");
                        s_input.point = (lv_point_t){360, 280};
                        s_input.state = LV_INDEV_STATE_PRESSED;
                        lv_indev_read(pointer);
                        check(state.game.snake.touch_held && s_action == ORB_GAME_STEER_START,
                            "touch Snake hold dispatches absolute heading");
                        s_now += 600;
                        lv_indev_read(pointer);
                        s_input.state = LV_INDEV_STATE_RELEASED;
                        lv_indev_read(pointer);
                        check(!state.game.snake.touch_held && s_action == ORB_GAME_STEER_STOP,
                            "touch Snake release clears steering without relatching a tap");
                        s_input.point = (lv_point_t){320, 280};
                        s_input.state = LV_INDEV_STATE_PRESSED;
                        lv_indev_read(pointer);
                        s_input.point.x = 340;
                        lv_indev_read(pointer);
                        s_input.state = LV_INDEV_STATE_RELEASED;
                        lv_indev_read(pointer);
                        check(!state.user_paused && !state.game.snake.touch_held &&
                            s_action == ORB_GAME_STEER_STOP,
                            "quick Snake steering drag cannot also tap to pause");
                        state.ticks = ORB_GAME_SNAKE_GRACE;
                        update(view, &state, 31, false, false);
                        check(!has_text(lv_screen_active(), "Tilt to steer") &&
                            !has_text(lv_screen_active(), "Hold to steer"),
                            "Snake instruction leaves the arena after its first second");
                        frozen_frame(display);
                    }
                }
                /* The first frame after leaving a reading surface must not
                 * retain labels while a hidden material completes its spring. */
                state.started = true;
                state.user_paused = false;
                update(view, &state, 31, false, false);
                play_chrome();
                orb_game_view_destroy(view);
                s_model = NULL;
            }
        }
        orb_game_state_t state;
        orb_game_init(&state, ORB_GAME_POP, 0x4f5242u);
        state.started = true;
        state.game_over = true;
        state.score = 128;
        orb_game_view_t *view = orb_game_view_create(lv_screen_active(), state.kind, action, NULL);
        check(view != NULL, "create result capsule contract");
        update(view, &state, 114, false, false);
        check(!has_text(lv_screen_active(), ORB_ICON_X), "cold result retains a Close glyph");
        advance_clock(view, &state, 114, false, false, 800);
        frozen_frame(display);
        /* The curved result title now legitimately crosses this old toolbar
         * rectangle. frozen_frame above still compares retained pixels with
         * a complete redraw, catching an outgoing Close's stale material. */
        lv_obj_t *label = find_text(lv_screen_active(), "Play again");
        lv_obj_t *done = find_text(lv_screen_active(), "Done");
        check(label && done, "result exposes Play again and quiet Done");
        lv_obj_t *button = lv_obj_get_parent(label);
        lv_area_t capsule, ink;
        lv_obj_get_coords(button, &capsule);
        lv_obj_get_coords(label, &ink);
        check(lv_area_get_height(&capsule) == ORB_KIT_PRIMARY_H,
              "result uses the kit64 primary role");
        check(capsule.x1 + capsule.x2 == 465 && capsule.y1 == 284,
              "result capsule is centered at the frame baseline");
        check(ink.x1 >= capsule.x1 && ink.x2 <= capsule.x2 &&
              ink.y1 >= capsule.y1 && ink.y2 <= capsule.y2, "result text clips outside capsule");
        check(abs((ink.x1 + ink.x2) - (capsule.x1 + capsule.x2)) <= 2 &&
              abs((ink.y1 + ink.y2) - (capsule.y1 + capsule.y2)) <= 2,
              "result text is not centred within capsule");
        check(lv_obj_get_height(label) <= lv_font_get_line_height(lv_obj_get_style_text_font(label, 0)) + 2,
              "result label wraps inside capsule");
        check(lv_color_eq(lv_obj_get_style_text_color(label, 0), ORB_KIT_HOME),
              "Pop result capsule uses the kit accent ink");
        bool extra_pill = false;
        for (unsigned y = 278; y < 350; ++y) for (unsigned x = 100; x < 366; ++x) {
            if (x >= capsule.x1 && x <= capsule.x2 && y >= capsule.y1 && y <= capsule.y2) continue;
            size_t at = (y * SIM_D + x) * 3;
            extra_pill |= s_full[at] || s_full[at + 1] || s_full[at + 2];
        }
        check(!extra_pill, "result paints a second outer pill beyond its capsule");
        size_t fill = (((capsule.y1 + capsule.y2) / 2) * SIM_D + capsule.x1 + ORB_KIT_PRIMARY_H / 3) * 3;
        check(s_full[fill] < 180 && s_full[fill + 1] < 180,
              "result capsule uses an opaque bright accent instead of low tint");
        state.game_over = false;
        state.user_paused = true;
        update(view, &state, 114, false, false);
        advance_clock(view, &state, 114, false, false, 800);
        check(!has_text(lv_screen_active(), ORB_ICON_X), "Pause removes floating Close");
        lv_obj_t *resume = lv_obj_get_parent(find_text(lv_screen_active(), "Resume"));
        state.game_over = true;
        update(view, &state, 114, false, false);
        check(!has_text(lv_screen_active(), ORB_ICON_X), "Pause to result leaves an outgoing Close glyph");
        check(!lv_obj_is_visible(resume), "Pause to result leaves an outgoing Resume capsule over Done");
        orb_game_view_destroy(view);

        orb_game_init(&state, ORB_GAME_ORBIT, 0x4f5242u);
        state.started = true;
        state.score = 180;
        view = orb_game_view_create(lv_screen_active(), state.kind, action, NULL);
        check(view != NULL, "create three-digit Orbit score contract");
        update(view, &state, 180, false, false);
        advance_clock(view, &state, 180, false, false, 800);
        frozen_frame(display);
        lv_obj_t *score = find_text(lv_screen_active(), "180");
        check(score != NULL, "three-digit Orbit score remains visible");
        /* The scoreboard is now the shared80px neutral glass disc. The arena
         * sun keeps its original physics; score readability is bounded by the
         * control instead of enlarging the sun to manufacture white clearance. */
        lv_obj_t *score_disc = lv_obj_get_parent(lv_obj_get_parent(score));
        check(lv_obj_get_width(score_disc) == ORB_KIT_DISC &&
              lv_obj_get_height(score_disc) == ORB_KIT_DISC,
              "Orbit scoreboard is not the kit80 disc");
        const lv_font_t *font = lv_obj_get_style_text_font(score, 0);
        lv_point_t extent;
        lv_text_get_size(&extent, "180", font, lv_obj_get_style_text_letter_space(score, 0),
                         0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        check(extent.x <= ORB_KIT_DISC && extent.y <= ORB_KIT_DISC,
              "Orbit score ink exceeds its shared disc");
        check(lv_color_eq(lv_obj_get_style_text_color(score, 0), ORB_LABEL),
              "Orbit scoreboard lost white accessible ink");
        orb_game_view_destroy(view);

        orb_game_init(&state, ORB_GAME_POP, 0x4f5242u);
        state.started = true;
        memset(state.game.pop.bubbles, 0, sizeof(state.game.pop.bubbles));
        state.game.pop.bubbles[0] = (orb_game_bubble_t){-73, 67, 0, 0, 24, true};
        view = orb_game_view_create(lv_screen_active(), state.kind, action, NULL);
        check(view != NULL, "create live-size Pop bubble contract");
        update(view, &state, 0, false, false);
        frozen_frame(display);
        size_t reflection = (291 * SIM_D + 152) * 3;
        /* Review #36 / hig §2.2: no printed-on static specular dot; the spot
         * shows only the flat translucent body (yellow has no blue). */
        check(s_full[reflection + 2] < 60, "live-size Pop bubble still carries a static reflection dot");
        orb_game_view_destroy(view);
    }
    check(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK, "restore scene fonts");
    check(orb_fonts_set_bold_text(false) == ORB_FONTS_OK, "restore scene weight");
    orb_ui_set_reduce_transparency(false);
    orb_ui_set_reduce_motion(false);
}

static void check_readable_line(lv_obj_t *label, const char *why)
{
    check(label != NULL, why);
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    orb_kit_text_fit_info_t fit;
    bool hero = orb_kit_hero_fit_info(label, &fit);
    if (hero) check(fit.complete && fit.lines <= 2, "kit hero loses complete instructional copy");
    lv_point_t ink;
    lv_text_get_size(&ink, lv_label_get_text(label), font,
        lv_obj_get_style_text_letter_space(label, 0), 0,
        hero ? lv_obj_get_width(label) : LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_area_t area;
    lv_obj_get_coords(label, &area);
    if (!hero && lv_obj_get_height(label) > lv_font_get_line_height(font) +
        lv_obj_get_style_pad_top(label, 0) + lv_obj_get_style_pad_bottom(label, 0) + 2)
        fprintf(stderr, "readable line '%s' height=%d line=%d width=%d\n", lv_label_get_text(label),
            lv_obj_get_height(label), lv_font_get_line_height(font), lv_obj_get_width(label));
    check(hero || lv_obj_get_height(label) <= lv_font_get_line_height(font) +
          lv_obj_get_style_pad_top(label, 0) + lv_obj_get_style_pad_bottom(label, 0) + 2,
          "tuning status/result label wraps");
    check(ink.x <= lv_area_get_width(&area) &&
          ink.x <= orb_chord_width(area.y1, area.y2, 16),
          "tuning status/result ink exceeds its round safe chord");
}

static void tuning_contract(lv_display_t *display, lv_indev_t *pointer, const char *directory)
{
    for (unsigned mode = 0; mode < 2; ++mode) {
        check(orb_fonts_set_content_size(mode ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK,
              "tuning content size");
        check(orb_fonts_set_bold_text(mode != 0) == ORB_FONTS_OK, "tuning weight");
        orb_ui_set_reduce_transparency(mode != 0);
        orb_ui_set_reduce_motion(mode != 0);
        const char *suffix = mode ? "ax3-bold-rt-rm" : "default";
        char capture_name[80];
        static const orb_game_kind_t tilting[] = {ORB_GAME_MARBLE, ORB_GAME_BEADS, ORB_GAME_SNAKE};
        for (unsigned i = 0; i < sizeof(tilting) / sizeof(tilting[0]); ++i) {
            orb_game_state_t state;
            orb_game_init(&state, tilting[i], 0x4f5242u);
            state.tilt_calibrating = true;
            s_model = &state;
            orb_game_view_t *view = orb_game_view_create(lv_screen_active(), state.kind, action, NULL);
            check(view != NULL, "create tuning motion fixture");
            update(view, &state, 0, false, false);
            check_readable_line(find_text(lv_screen_active(), "Hold still"), "neutral capture has a reading line");
            snprintf(capture_name, sizeof(capture_name), "calibrating-%s", suffix);
            capture(display, directory, state.kind, capture_name);
            state.tilt_calibrating = false;
            update(view, &state, 0, false, false);
            check(!has_text(lv_screen_active(), "Hold still"), "neutral line clears when calibration completes");
            frozen_frame(display);
            if (state.kind != ORB_GAME_MARBLE) {
                state.motion_unavailable = true;
                state.controls = state.kind == ORB_GAME_SNAKE;
                update(view, &state, 0, false, false);
                /* One phrase in every tilt game: Beads on its top rim, Snake on
                 * the bottom line its wall leaves free (review #16). */
                if (state.kind == ORB_GAME_SNAKE)
                    check_readable_line(find_text(lv_screen_active(), "Motion unavailable"),
                        "failed motion has an honest reading line");
                else check(has_text(lv_screen_active(), "Motion unavailable"), "failed motion has an honest reading line");
                snprintf(capture_name, sizeof(capture_name), "motion-unavailable-%s", suffix);
                capture(display, directory, state.kind, capture_name);
                state.motion_unavailable = false;
                update(view, &state, 0, false, false);
                check(!has_text(lv_screen_active(), "Motion unavailable") &&
                      !has_text(lv_screen_active(), "Touch controls enabled"),
                      "motion recovery clears its reading line");
                frozen_frame(display);
            }
            if (state.kind == ORB_GAME_BEADS) {
                state.game.beads.time_valid = false;
                update(view, &state, 0, false, false);
                check(has_text(lv_screen_active(), "Time not set"), "Beads unsynchronised clock is honest");
                snprintf(capture_name, sizeof(capture_name), "time-unset-%s", suffix);
                capture(display, directory, state.kind, capture_name);
                orb_game_beads_set_time(&state, 10, 9);
                update(view, &state, 0, false, false);
                check(!has_text(lv_screen_active(), "Time not set"), "Beads real clock clears time-unset reading line");
                frozen_frame(display);
                state.user_paused = true;
                update(view, &state, 0, false, false);
                advance_clock(view, &state, 0, false, false, 800);
                check_readable_line(find_text(lv_screen_active(), "Rest for time"),
                    "Beads pause explains the resting clock");
                snprintf(capture_name, sizeof(capture_name), "pause-rezero-%s", suffix);
                capture(display, directory, state.kind, capture_name);
                pointer_action(pointer, 233, 329, ORB_GAME_PAUSE, "Beads Resume remains reachable with accessible type");
                check(!state.user_paused, "Beads Resume keeps its current round");
            }
            orb_game_view_destroy(view);
            s_model = NULL;
        }
        for (orb_game_kind_t kind = ORB_GAME_ORBIT; kind < ORB_GAME_COUNT; ++kind) {
            orb_game_state_t state;
            orb_game_init(&state, kind, 0x4f5242u);
            state.score = UINT32_MAX;
            state.game_over = true;
            orb_game_view_t *view = orb_game_view_create(lv_screen_active(), kind, action, NULL);
            check(view != NULL, "create maximum score result fixture");
            update(view, &state, UINT32_MAX, false, false);
            advance_clock(view, &state, UINT32_MAX, false, false, 800);
            lv_obj_t *score = find_text(lv_screen_active(), "4b");
            check_readable_line(score, "maximum score is compact and visible");
            lv_font_glyph_dsc_t suffix_glyph;
            check(lv_font_get_glyph_dsc(lv_obj_get_style_text_font(score, 0), &suffix_glyph, 'b', 0),
                  "compact result includes its unit glyph");
            snprintf(capture_name, sizeof(capture_name), "maximum-score-result-%s", suffix);
            capture(display, directory, kind, capture_name);
            orb_game_view_destroy(view);
        }
        orb_game_state_t state;
        orb_game_init(&state, ORB_GAME_ORBIT, 0x4f5242u);
        state.game.orbit.angle = 0;
        for (unsigned i = 0; i < ORB_GAME_OBSTACLES; ++i)
            state.game.orbit.obstacles[i] = (orb_game_obstacle_t){0, 0, false};
        orb_game_step(&state, 0, 0, false, mode != 0);
        check(state.lives == 2 && state.game.orbit.hit_grace, "clustered hit offers a recoverable state");
        orb_game_view_t *view = orb_game_view_create(lv_screen_active(), state.kind, action, NULL);
        check(view != NULL, "create Orbit recovery fixture");
        update(view, &state, 0, false, false);
        lv_obj_t *cold_score = find_text(lv_screen_active(), "0");
        check(cold_score != NULL, "cold Orbit displays its initial score");
        lv_area_t cold_score_box;
        lv_obj_get_coords(cold_score, &cold_score_box);
        lv_obj_t *cold_disc = lv_obj_get_parent(lv_obj_get_parent(cold_score));
        lv_area_t cold_disc_box;
        lv_obj_get_coords(cold_disc, &cold_disc_box);
        check(abs(cold_score_box.x1 + cold_score_box.x2 - 466) <= 2 &&
              cold_score_box.y1 >= cold_disc_box.y1 && cold_score_box.y2 <= cold_disc_box.y2,
              "cold Orbit score is immediately fitted inside its kit disc");
        snprintf(capture_name, sizeof(capture_name), "recovery-%s", suffix);
        capture(display, directory, state.kind, capture_name);
        orb_game_view_destroy(view);
    }
    check(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK, "restore tuning fonts");
    check(orb_fonts_set_bold_text(false) == ORB_FONTS_OK, "restore tuning weight");
    orb_ui_set_reduce_transparency(false);
    orb_ui_set_reduce_motion(false);
}

static int compare_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void native_beads(lv_event_t *event)
{
    const orb_game_state_t *state = lv_event_get_user_data(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    for (unsigned i = 0; i < state->game.beads.count; ++i) {
        const orb_game_bead_t *bead = &state->game.beads.beads[i];
        int x = (int)lroundf(ORB_GAME_CX + bead->x), y = (int)lroundf(ORB_GAME_CY + bead->y);
        int radius = ORB_GAME_BEAD_DRAW_RADIUS;
        unsigned tone = ((i * 1664525u + 1013904223u) >> 16) % 30u;
        lv_area_t area = {x - radius, y - radius, x + radius, y + radius};
        lv_draw_rect_dsc_t descriptor;
        lv_draw_rect_dsc_init(&descriptor);
        descriptor.radius = LV_RADIUS_CIRCLE;
        descriptor.bg_color = lv_color_hex(((226u + tone) << 16) | ((49u + tone / 5u) << 8) | (84u + tone / 3u));
        descriptor.bg_opa = state->game.beads.target_opacity[i];
        descriptor.border_width = 0;
        lv_draw_rect(layer, &descriptor, &area);
    }
}

static void beads_native_oracle(lv_display_t *display)
{
    static const uint8_t opacities[] = {0, 1, 2, 3, 7, 32, 92, 127, 252, 253, 254, 255};
    for (unsigned sample = 0; sample < sizeof(opacities); ++sample) {
        orb_game_state_t state;
        orb_game_init(&state, ORB_GAME_BEADS, 17);
        state.game.beads.time_valid = true;
        state.game.beads.settled = true;
        state.game.beads.count = 64;
        for (unsigned i = 0; i < state.game.beads.count; ++i) {
            float x = (float)(i % 4) * 80 - 120;
            float y = (float)(i / 4) * 80 - 120;
            if (i < 8) y = (float)(i % 4) * 84 + 83 - ORB_GAME_CY;
            if (i >= 12) { x = (float)(i - 12) * 2; y = 0; }
            if (i >= 16) {
                /* Packed overlap in both rows neighbouring a physical band
                 * cut exercises body order and clipped opaque runs. */
                x = (float)(i % 8) * 4 - 14;
                y = (float)((i - 16) / 8) * 4 + 81 - ORB_GAME_CY;
            }
            if (i >= 48) { x = (float)(i % 8) * 56 - 196; y = 102; }
            state.game.beads.beads[i].x = x;
            state.game.beads.beads[i].y = y;
            state.game.beads.target_opacity[i] = i % 2 ? 255 : opacities[sample];
        }
        orb_game_view_t *view = orb_game_view_create(lv_screen_active(), state.kind, action, NULL);
        check(view != NULL, "create native Beads oracle");
        update(view, &state, 0, false, false);
        lv_refr_now(display);
        sim_display_rgb(s_partial);
        const lv_color_format_t formats[] = {LV_COLOR_FORMAT_RGB565, LV_COLOR_FORMAT_ARGB8888,
            LV_COLOR_FORMAT_XRGB8888};
        lv_draw_buf_t *snapshots[sizeof(formats) / sizeof(formats[0])];
        for (unsigned format = 0; format < sizeof(formats) / sizeof(formats[0]); ++format) {
            snapshots[format] = lv_snapshot_take(lv_screen_active(), formats[format]);
            check(snapshots[format] != NULL, "take Beads native/snapshot format oracle");
        }
        orb_game_view_destroy(view);

        lv_obj_t *reference = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(reference);
        lv_obj_set_size(reference, 466, 466);
        lv_obj_set_style_bg_color(reference, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(reference, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(reference, native_beads, LV_EVENT_DRAW_MAIN, &state);
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(display);
        sim_display_rgb(s_full);
        check(!memcmp(s_partial, s_full, sizeof(s_full)), "batched Beads differ from native antialiased circles");
        for (unsigned format = 0; format < sizeof(formats) / sizeof(formats[0]); ++format) {
            lv_draw_buf_t *native = lv_snapshot_take(lv_screen_active(), formats[format]);
            check(native && native->header.stride == snapshots[format]->header.stride &&
                  native->header.h == snapshots[format]->header.h,
                  "take native Beads snapshot comparison");
            check(!memcmp(native->data, snapshots[format]->data,
                          (size_t)native->header.stride * native->header.h),
                  "Beads batch differs from native RGB565/ARGB8888/XRGB8888 snapshot");
            lv_draw_buf_destroy(native);
            lv_draw_buf_destroy(snapshots[format]);
        }
        lv_obj_delete(reference);
    }
}

/* Pass 5 named Pop motion patch: at integer centres the quarter-pixel stamps
 * equal LVGL's own body/rim/gloss; at quarter phases they move by sub-pixel
 * amounts, stay inside the damage they request and still compare equal to a
 * complete redraw. */
static void pop_phase_oracle(lv_display_t *display)
{
    static uint8_t integer_frame[SIM_D * SIM_D * 3];
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_POP, 0x4f5242u);
    state.started = true;
    orb_game_view_t *view = orb_game_view_create(lv_screen_active(), state.kind, action, NULL);
    check(view != NULL, "create Pop phase oracle");
    for (unsigned sample = 0; sample < 12; ++sample) {
        for (unsigned i = 0; i < ORB_GAME_BUBBLES; ++i) {
            float angle = (float)(i * 7 + sample) * .41f;
            state.game.pop.bubbles[i] = (orb_game_bubble_t){.x = roundf(cosf(angle) * (60 + 11 * i)),
                .y = roundf(sinf(angle) * (52 + 13 * i)), .radius = (uint8_t)(18 + (i + sample) % 4 * 3),
                .active = (i + sample) % 5 != 0};
        }
        orb_game_pop_test_integer(false);
        update(view, &state, 0, false, false);
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(display);
        sim_display_rgb(s_partial);
        orb_game_pop_test_integer(true);
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(display);
        sim_display_rgb(integer_frame);
        check(!memcmp(s_partial, integer_frame, sizeof(integer_frame)),
              "integer-centre Pop phase stamps differ from LVGL bubbles");
        orb_game_pop_test_integer(false);
        for (unsigned phase = 1; phase < 4; ++phase) {
            for (unsigned i = 0; i < ORB_GAME_BUBBLES; ++i) {
                state.game.pop.bubbles[i].x += .25f;
                state.game.pop.bubbles[i].y -= phase == 2 ? .25f : 0.f;
            }
            update(view, &state, 0, false, false);
            lv_refr_now(display);
            sim_display_rgb(s_partial);
            lv_obj_invalidate(lv_screen_active());
            lv_refr_now(display);
            sim_display_rgb(s_full);
            check(!memcmp(s_partial, s_full, sizeof(s_full)),
                  "quarter-pixel Pop damage misses part of a moved bubble");
        }
        check(memcmp(s_full, integer_frame, sizeof(s_full)), "quarter-pixel Pop bubbles did not move");
    }
    orb_game_view_destroy(view);
}

static void orbit_native_oracle(lv_display_t *display)
{
    const lv_color_format_t formats[] = {LV_COLOR_FORMAT_RGB565,
        LV_COLOR_FORMAT_ARGB8888, LV_COLOR_FORMAT_XRGB8888};
    for (unsigned mode = 0; mode < 2; ++mode) {
        check(orb_fonts_set_content_size(mode ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK, "load Orbit pose fonts");
        check(orb_fonts_set_bold_text(mode != 0) == ORB_FONTS_OK, "load Orbit pose weight");
        orb_ui_set_reduce_transparency(mode != 0);
        orb_game_state_t state;
        orb_game_init(&state, ORB_GAME_ORBIT, 1789);
        state.started = true;
        state.score = 9;
        orb_game_view_t *view = orb_game_view_create(lv_screen_active(), state.kind, action, NULL);
        check(view != NULL, "create immutable Orbit pose oracle");
        for (unsigned sample = 0; sample < 24; ++sample) {
            state.game.orbit.ring = sample % 3;
            state.game.orbit.angle = (float)sample * .27f;
            state.game.orbit.trail_age = (uint8_t)(sample % 16);
            state.game.orbit.previous_radius = 80;
            state.game.orbit.hit_grace = sample % 2;
            for (unsigned i = 0; i < ORB_GAME_OBSTACLES; ++i) {
                state.game.orbit.obstacles[i].ring = (sample + i) % 3;
                state.game.orbit.obstacles[i].angle = (float)(sample + i * 4) * .193f;
            }
            for (unsigned i = 0; i < ORB_GAME_ORBIT_STARS; ++i) {
                state.game.orbit.stars[i].ring = (sample + i) % 3;
                state.game.orbit.stars[i].angle = (float)(sample + i * 7) * .17f;
                state.game.orbit.stars[i].active = true;
            }
            state.effect = (orb_game_effect_t){.x = 35, .y = -40, .duration = 100,
                .age = (uint8_t)(sample * 4), .count = ORB_GAME_PARTICLES,
                .ring_radius = 12, .colour = 0xff9230};
            update(view, &state, 0, false, false);
            lv_refr_now(display);
            sim_display_rgb(s_partial);
            lv_draw_buf_t *snapshots[sizeof(formats) / sizeof(formats[0])];
            for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
                snapshots[i] = lv_snapshot_take(lv_screen_active(), formats[i]);
                check(snapshots[i] != NULL, "snapshot immutable Orbit pose");
            }
            orb_game_view_test_serial_orbit(view, true);
            lv_refr_now(display);
            sim_display_rgb(s_full);
            check(!memcmp(s_partial, s_full, sizeof(s_full)),
                "split-Y Orbit pose differs from serial panel rendering");
            for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
                lv_draw_buf_t *reference = lv_snapshot_take(lv_screen_active(), formats[i]);
                check(reference && reference->header.stride == snapshots[i]->header.stride &&
                    reference->header.h == snapshots[i]->header.h, "snapshot serial Orbit oracle");
                check(!memcmp(reference->data, snapshots[i]->data,
                    (size_t)reference->header.stride * reference->header.h),
                    "immutable Orbit pose differs from serial RGB565/ARGB/XRGB");
                lv_draw_buf_destroy(reference);
                lv_draw_buf_destroy(snapshots[i]);
            }
            orb_game_view_test_serial_orbit(view, false);
        }
        orb_game_view_destroy(view);
    }
    check(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK, "restore Orbit pose fonts");
    check(orb_fonts_set_bold_text(false) == ORB_FONTS_OK, "restore Orbit pose weight");
    orb_ui_set_reduce_transparency(false);
}

static void orbit_glass_crossings(lv_display_t *display)
{
    const float diagonals[] = {.42f, .6f, .785398f, .95f, 1.15f, 2.35619f, 3.92699f, 5.49779f};
    for (unsigned mode = 0; mode < 2; ++mode) {
        check(orb_fonts_set_content_size(mode ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK,
              "load Orbit crossing fonts");
        check(orb_fonts_set_bold_text(mode != 0) == ORB_FONTS_OK, "load Orbit crossing weight");
        orb_ui_set_reduce_transparency(mode != 0);
        orb_game_state_t state;
        orb_game_init(&state, ORB_GAME_ORBIT, 17);
        state.game.orbit.ring = 0;
        orb_game_view_t *view = orb_game_view_create(lv_screen_active(), state.kind, action, NULL);
        check(view != NULL, "create diagonal glass crossing fixture");
        update(view, &state, 0, false, false);
        advance_clock(view, &state, 0, false, false, 800);
        for (unsigned pass = 0; pass < 2; ++pass) {
            for (unsigned sample = 0; sample < sizeof(diagonals) / sizeof(diagonals[0]); ++sample) {
                /* Change score once to cover every offset of its real rolling
                 * labels while source ink crosses the inner diagonal halo. */
                state.score += 10;
                for (unsigned frame = 0; frame < 40; ++frame) {
                    state.game.orbit.angle = diagonals[sample] + (float)frame * .003f;
                    for (unsigned i = 0; i < ORB_GAME_OBSTACLES; ++i) {
                        state.game.orbit.obstacles[i].ring = 0;
                        state.game.orbit.obstacles[i].angle = diagonals[(sample + i) % 8] + (float)frame * .004f;
                    }
                    for (unsigned i = 0; i < ORB_GAME_ORBIT_STARS; ++i) {
                        state.game.orbit.stars[i].ring = 0;
                        state.game.orbit.stars[i].angle = diagonals[(sample + i * 2) % 8];
                        state.game.orbit.stars[i].active = true;
                    }
                    s_now += 17;
                    update(view, &state, 0, false, false);
                    frozen_frame(display);
                }
            }
        }
        orb_game_view_destroy(view);
    }
    check(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK, "restore Orbit crossing fonts");
    check(orb_fonts_set_bold_text(false) == ORB_FONTS_OK, "restore Orbit crossing weight");
    orb_ui_set_reduce_transparency(false);
}

static void rings_native_oracle(lv_display_t *display)
{
    const orb_game_kind_t kinds[] = {ORB_GAME_ORBIT};
    for (unsigned mode = 0; mode < 2; ++mode) {
        check(orb_fonts_set_content_size(mode ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK,
              "load native ring oracle fonts");
        check(orb_fonts_set_bold_text(mode != 0) == ORB_FONTS_OK, "load native ring oracle weight");
        orb_ui_set_reduce_transparency(mode != 0);
        for (unsigned game = 0; game < sizeof(kinds) / sizeof(kinds[0]); ++game) {
            orb_game_state_t state;
            orb_game_init(&state, kinds[game], 17);
            orb_game_view_t *view = orb_game_view_create(lv_screen_active(), state.kind, action, NULL);
            check(view != NULL, "create native ring oracle");
            update(view, &state, 0, false, false);
            advance_clock(view, &state, 0, false, false, 800);
            lv_refr_now(display);
            sim_display_rgb(s_partial);
            orb_game_view_stats_t stats;
            orb_game_view_stats(view, &stats);
            check(stats.ring_cache_bytes > 0 && stats.ring_cache_bytes <= 7000,
                  "native ring cache exceeds its bounded memory allowance");
            printf("game %s native ring cache: %u bytes\n", orb_game_id(state.kind), stats.ring_cache_bytes);
            orb_game_view_test_native_rings(view);
            lv_refr_now(display);
            sim_display_rgb(s_full);
            check(!memcmp(s_partial, s_full, sizeof(s_full)), "cached rings differ from native BORDER pixels");
            orb_game_view_destroy(view);
        }
    }
    check(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK, "restore ring oracle fonts");
    check(orb_fonts_set_bold_text(false) == ORB_FONTS_OK, "restore ring oracle weight");
    orb_ui_set_reduce_transparency(false);
}

static void beads_packed_damage(lv_display_t *display)
{
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_BEADS, 17);
    state.game.beads.time_valid = true;
    state.game.beads.count = ORB_GAME_BEAD_COUNT;
    for (unsigned i = 0; i < state.game.beads.count; ++i) {
        state.game.beads.beads[i].x = (float)(i % 20) * 7 - 66;
        state.game.beads.beads[i].y = (float)(i / 20) * 7 + 10;
    }
    orb_game_view_t *view = orb_game_view_create(lv_screen_active(), state.kind, action, NULL);
    check(view != NULL, "create packed Beads damage fixture");
    update(view, &state, 0, false, false);
    lv_refr_now(display);
    for (unsigned i = 0; i < state.game.beads.count; ++i) state.game.beads.beads[i].x += 1;
    s_flushed_pixels = 0;
    s_measure_flush = true;
    update(view, &state, 0, false, false);
    lv_refr_now(display);
    s_measure_flush = false;
    check(s_flushed_pixels > 8000 && s_flushed_pixels < 40000,
          "packed Beads damage overflows into a full-screen refresh");
    frozen_frame(display);
    orb_game_view_destroy(view);
}

static unsigned interpolated_frames(lv_display_t *display, const char *directory)
{
    unsigned frames = 0;
    static const float phases[] = {0, .35f, .9f};
    for (unsigned mode = 0; mode < 2; ++mode) {
        check(orb_fonts_set_content_size(mode ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK,
              "load interpolated pose fonts");
        check(orb_fonts_set_bold_text(mode != 0) == ORB_FONTS_OK, "load interpolated pose weight");
        orb_ui_set_reduce_transparency(mode != 0);
        for (orb_game_kind_t kind = ORB_GAME_MARBLE; kind < ORB_GAME_COUNT; ++kind) {
            orb_game_state_t model, rendered;
            orb_game_pose_t pose;
            orb_game_init(&model, kind, 17);
            if (kind == ORB_GAME_BEADS) orb_game_beads_set_time(&model, 10, 9);
            orb_game_view_t *view = orb_game_view_create(lv_screen_active(), kind, action, NULL);
            check(view != NULL, "create interpolated damage fixture");
            update(view, &model, 0, false, false);
            lv_refr_now(display);
            for (unsigned step = 0; step < 24; ++step) {
                if (kind == ORB_GAME_MARBLE && step == 12) orb_game_action(&model, ORB_GAME_REWIND_START, 0, 0);
                if (kind == ORB_GAME_MARBLE && step == 22) orb_game_action(&model, ORB_GAME_REWIND_STOP, 0, 0);
                orb_game_pose_capture(&model, &pose);
                orb_game_step(&model, kind == ORB_GAME_SNAKE ? 0 : kind == ORB_GAME_MARBLE ? -.4f : .2f,
                              0, true, false);
                for (unsigned phase = 0; phase < sizeof(phases) / sizeof(phases[0]); ++phase) {
                    rendered = model;
                    orb_game_pose_interpolate(&rendered, &pose, phases[phase]);
                    s_now += 5;
                    update(view, &rendered, 0, false, false);
                    s_budget_stage = 100u + (unsigned)kind;
                    s_budget_controls = step;
                    s_budget_frame = phase;
                    frozen_frame(display);
                    if (kind == ORB_GAME_MARBLE && step == 15 && phase == 1)
                        capture(display, directory, kind, mode ? "rewind-half-ax3-bold-rt" : "rewind-half-default");
                    ++frames;
                }
            }
            orb_game_view_destroy(view);
        }
    }
    check(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK, "restore interpolated pose fonts");
    check(orb_fonts_set_bold_text(false) == ORB_FONTS_OK, "restore interpolated pose weight");
    orb_ui_set_reduce_transparency(false);
    s_budget_stage = s_budget_controls = s_budget_frame = 0;
    return frames;
}

static void beads_task_culling(lv_display_t *display)
{
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_BEADS, 0x4f5242u);
    orb_game_beads_set_time(&state, 10, 9);
    orb_game_view_t *view = orb_game_view_create(lv_screen_active(), state.kind, action, NULL);
    check(view != NULL, "create Beads allocation-count fixture");
    update(view, &state, 0, false, false);
    advance_clock(view, &state, 0, false, false, 800);
    frozen_frame(display);
    s_bead_tasks = s_bead_body_tasks = s_bead_highlight_tasks = s_bead_off_clip_tasks = 0;
    s_measure_bead_tasks = true;
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(display);
    s_measure_bead_tasks = false;
    check(s_bead_tasks == 0 && s_bead_body_tasks == 0 && s_bead_highlight_tasks == 0,
          "Beads native batch allocates per-particle task metadata");
    check(s_bead_off_clip_tasks == 0, "Beads allocates particle metadata beyond band AA coverage");
    printf("game beads task allocations: full_frame=%u (%u particles), off_clip=%u\n",
           s_bead_tasks, state.game.beads.count, s_bead_off_clip_tasks);
    frozen_frame(display);

    /* Initial beads occupy the inset bowl; a small top-rim damage region has
     * no particle pixels and must allocate no bead draw metadata at all. */
    const lv_area_t empty_rim = {220, 14, 246, 24};
    s_bead_tasks = s_bead_off_clip_tasks = 0;
    s_measure_bead_tasks = true;
    lv_obj_invalidate_area(lv_screen_active(), &empty_rim);
    lv_refr_now(display);
    s_measure_bead_tasks = false;
    check(s_bead_tasks == 0, "empty rim damage allocates off-screen Beads tasks");
    frozen_frame(display);
    orb_game_view_destroy(view);
}

static void beads_noncanonical_fallback(lv_display_t *display)
{
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_BEADS, 17);
    state.started = false;
    orb_game_view_t *view = orb_game_view_create(lv_screen_active(), state.kind, action, NULL);
    check(view != NULL, "create noncanonical Beads preview fixture");
    update(view, &state, 0, false, false);
    advance_clock(view, &state, 0, false, false, 800);
    s_bead_tasks = s_bead_body_tasks = s_bead_highlight_tasks = s_bead_off_clip_tasks = 0;
    s_measure_bead_tasks = true;
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(display);
    s_measure_bead_tasks = false;
    /* HIG §2.2: thumbnails and live beads both use plain, separate bodies;
     * the retired fixed reflection colour must never allocate draw tasks. */
    check(s_bead_body_tasks >= ORB_GAME_BEAD_COUNT && s_bead_highlight_tasks == 0,
          "noncanonical thumbnail Beads preview failed to use the plain body fallback");
    frozen_frame(display);
    state.started = true;
    update(view, &state, 0, false, false);
    advance_clock(view, &state, 0, false, false, 800);
    s_bead_tasks = s_bead_body_tasks = s_bead_highlight_tasks = 0;
    s_measure_bead_tasks = true;
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(display);
    s_measure_bead_tasks = false;
    check(s_bead_tasks == 0, "canonical Beads play failed to recover mask batching after preview");
    frozen_frame(display);
    orb_game_view_destroy(view);
}

static unsigned beads_budget(lv_display_t *display, const char *directory)
{
    enum { FLOW_FRAMES = 90, IDLE_FRAMES = 60 };
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_BEADS, 0x4f5242u);
    orb_game_beads_set_time(&state, 10, 9);
    orb_game_view_t *view = orb_game_view_create(lv_screen_active(), state.kind, action, NULL);
    check(view != NULL, "create Beads frame budget fixture");
    update(view, &state, 0, false, false);
    advance_clock(view, &state, 0, false, false, 800);
    frozen_frame(display);
    unsigned objects = child_count(lv_screen_active());
    uint64_t times[FLOW_FRAMES], pixels = 0;
    uint32_t maximum = 0;
    for (unsigned frame = 0; frame < FLOW_FRAMES; ++frame) {
        s_flushed_pixels = 0;
        s_measure_flush = true;
        uint64_t begin = monotonic_us();
        /* Two fixed steps per presentation include the full particle physics
         * cost, then measure only the retained panel's real band transfers. */
        float ax = frame < FLOW_FRAMES / 2 ? 0.30f : -0.30f;
        orb_game_step(&state, ax, 0.08f, true, false);
        orb_game_step(&state, ax, 0.08f, true, false);
        s_now += 33;
        lv_timer_handler();
        update(view, &state, 0, false, false);
        lv_refr_now(display);
        times[frame] = monotonic_us() - begin;
        s_measure_flush = false;
        pixels += s_flushed_pixels;
        if (s_flushed_pixels > maximum) maximum = s_flushed_pixels;
        check(child_count(lv_screen_active()) == objects, "Beads flow allocates game widgets");
        frozen_frame(display);
    }
    qsort(times, FLOW_FRAMES, sizeof(times[0]), compare_u64);
    /* This Mac also runs live services. Report wall-clock cost without making
     * unrelated host load a physics failure; the ESP32 frame gate is measured
     * on the board. Deterministic transfer/idle/widget checks stay below. */
    printf("game beads flow: frames=%u flushed_px mean=%.1f max=%u "
           "host_us physics+render p95=%llu max=%llu\n", FLOW_FRAMES,
           (double)pixels / FLOW_FRAMES, maximum,
           (unsigned long long)times[FLOW_FRAMES * 95 / 100],
           (unsigned long long)times[FLOW_FRAMES - 1]);
    capture(display, directory, state.kind, "budget-flow-default");

    /* A held fluid pile is distinct from a sensor-latched clock: contact
     * equilibrium itself must not shimmer or keep repainting half pixels. */
    for (unsigned step = 0; step < 1800; ++step)
        orb_game_step(&state, sinf(35 * 0.0174532925f), 0, true, false);
    for (unsigned frame = 0; frame < 30; ++frame) {
        orb_game_step(&state, sinf(35 * 0.0174532925f), 0, true, false);
        s_now += 17;
        lv_timer_handler();
        update(view, &state, 0, false, false);
        lv_refr_now(display);
        frozen_frame(display);
    }
    uint64_t pile_pixels = 0;
    for (unsigned frame = 0; frame < 300; ++frame) {
        orb_game_step(&state, sinf(35 * 0.0174532925f), 0, true, false);
        s_now += 17;
        lv_timer_handler();
        s_flushed_pixels = 0;
        s_measure_flush = true;
        update(view, &state, 0, false, false);
        lv_refr_now(display);
        s_measure_flush = false;
        pile_pixels += s_flushed_pixels;
        frozen_frame(display);
    }
    check(!state.game.beads.settled, "fluid-pile redraw test accidentally latched the clock");
    check(pile_pixels == 0, "Beads stable contact pile shimmers without changed gravity");
    printf("game beads held pile: frames=300 flushed_px total=%llu\n",
           (unsigned long long)pile_pixels);

    /* Rendering fixture: rest was observed by the independently traced sensor
     * classifier; physics alone must never infer it from zero steering. */
    orb_game_tilt_t rest = {.ready = true, .gyro_observed = true, .still_us = ORB_GAME_BEAD_SETTLE_STEPS * ORB_GAME_STEP_US};
    orb_game_set_motion(&state, &rest);
    for (unsigned step = 0; step < 240; ++step) orb_game_step(&state, 0, 0, true, false);
    check(state.game.beads.settled && state.game.beads.travel_ticks == 45,
          "Beads frame budget reaches a stable clock");
    update(view, &state, 0, false, false);
    capture(display, directory, state.kind, "budget-clock-default");
    pixels = 0;
    for (unsigned frame = 0; frame < IDLE_FRAMES; ++frame) {
        orb_game_step(&state, 0, 0, true, false);
        orb_game_step(&state, 0, 0, true, false);
        s_now += 33;
        lv_timer_handler();
        s_flushed_pixels = 0;
        s_measure_flush = true;
        update(view, &state, 0, false, false);
        lv_refr_now(display);
        s_measure_flush = false;
        pixels += s_flushed_pixels;
        check(child_count(lv_screen_active()) == objects, "Beads resting clock allocates game widgets");
        frozen_frame(display);
    }
    check(pixels == 0, "Beads stable clock redraws without a time or motion change");
    printf("game beads idle: frames=%u flushed_px total=%llu\n",
           IDLE_FRAMES, (unsigned long long)pixels);
    orb_game_view_destroy(view);
    return FLOW_FRAMES + IDLE_FRAMES;
}

static unsigned conservatory_budget(lv_display_t *display)
{
    /* Capture real band transfers separately from the full-redraw oracle.
     * Host wall time demonstrates the simulator's 30 Hz budget only; the wire
     * and ESP32 CPU still require the owner's independent panel measurement. */
    enum { FRAMES = 180 };
    unsigned compared = 0;
    for (unsigned controls = 0; controls < 2; ++controls) {
        for (unsigned stage = 0; stage < ORB_MARBLE_STAGES; ++stage) {
            s_budget_stage = stage + 1;
            s_budget_controls = controls;
            orb_game_state_t state;
            orb_game_init(&state, ORB_GAME_MARBLE, 0x4f5242u);
            state.started = true;
            for (unsigned i = 0; i < stage; ++i) {
                trigger_reward(&state);
                orb_game_action(&state, ORB_GAME_PRIMARY, 0, 0);
            }
            state.controls = controls != 0;
            /* Both rotation directions cross the angle seam; touch-mode arc
             * splits also appear/disappear as openings pass real controls. */
            for (unsigned g = 0; g < ORB_MARBLE_GATES_MAX; ++g)
                state.game.marble.gates[g].angle =
                    state.game.marble.gates[g].speed > 0 ? 6.27f : 0.01f;
            orb_game_view_t *view = orb_game_view_create(lv_screen_active(), state.kind, action, NULL);
            check(view != NULL, "create enlarged arena budget fixture");
            update(view, &state, 0, false, false);
            advance_clock(view, &state, 0, false, false, 4500);
            frozen_frame(display);
            s_damage_events = 0;
            update(view, &state, 0, false, false);
            check(s_damage_events == 0, "unchanged enlarged gate unions submit no damage");
            unsigned objects = child_count(lv_screen_active());
            uint64_t times[FRAMES], total_pixels = 0, total_us = 0;
            uint32_t max_pixels = 0, max_damage = 0;
            for (unsigned frame = 0; frame < FRAMES; ++frame) {
                s_budget_frame = frame;
                orb_game_step(&state, 0.25f, 0.1f, true, false);
                orb_game_step(&state, 0.25f, 0.1f, true, false);
                s_now += 33;
                lv_timer_handler();
                s_largest_damage = s_flushed_pixels = 0;
                s_measure_flush = true;
                uint64_t begin = monotonic_us();
                update(view, &state, 0, false, false);
                lv_refr_now(display);
                times[frame] = monotonic_us() - begin;
                s_measure_flush = false;
                total_us += times[frame];
                total_pixels += s_flushed_pixels;
                if (s_flushed_pixels > max_pixels) max_pixels = s_flushed_pixels;
                if (s_largest_damage > max_damage) max_damage = s_largest_damage;
                check(s_largest_damage < SIM_D * SIM_D / 2,
                      "moving enlarged gates never request the arena square");
                check(child_count(lv_screen_active()) == objects,
                      "expanded arena frames create no widgets");
                frozen_frame(display);
                ++compared;
            }
            qsort(times, FRAMES, sizeof(times[0]), compare_u64);
            check(max_pixels < SIM_D * SIM_D * 3 / 5,
                  "enlarged arena steady frames flush too much of the retained panel");
            check(total_pixels / FRAMES < SIM_D * SIM_D / 4,
                  "enlarged arena average damage exceeds a quarter of the panel square");
            printf("conservatory stage=%u controls=%s frames=%u flushed_px mean=%.1f max=%u "
                   "largest_damage=%u host_us mean=%.1f p95=%llu max=%llu\n",
                   stage + 1, controls ? "touch" : "tilt", FRAMES,
                   (double)total_pixels / FRAMES, max_pixels, max_damage,
                   (double)total_us / FRAMES,
                   (unsigned long long)times[FRAMES * 95 / 100],
                   (unsigned long long)times[FRAMES - 1]);
            check(times[FRAMES * 95 / 100] < 33333,
                  "enlarged arena simulator p95 exceeds the 30 fps frame budget");
            orb_game_view_destroy(view);
        }
    }
    s_budget_stage = s_budget_controls = s_budget_frame = 0;
    return compared;
}

int main(int argc, char **argv)
{
    const char *directory = argc > 1 ? argv[1] : ".";
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *display = sim_display_create();
    lv_draw_unit_t *observer = lv_draw_create_unit(sizeof(*observer));
    check(observer != NULL, "create passive draw-task observer");
    observer->name = "Beads task observer";
    observer->evaluate_cb = count_bead_task;
    observer->dispatch_cb = passive_dispatch;
    orb_ui_init(NULL);
    check(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK, "load default fonts");
    lv_indev_t *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer, display);
    lv_indev_set_read_cb(pointer, pointer_read);
    lv_display_add_event_cb(display, damage, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(display, rendered, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(display, flushed, LV_EVENT_FLUSH_START, NULL);

    scene_contract(display, pointer);
    tuning_contract(display, pointer, directory);
    beads_native_oracle(display);
    rings_native_oracle(display);
    orbit_native_oracle(display);
    pop_phase_oracle(display);
    orbit_glass_crossings(display);
    beads_packed_damage(display);
    beads_task_culling(display);
    beads_noncanonical_fallback(display);
    unsigned transitions = transition_oracles(display);
    round_lifecycle(display, pointer);
    cold_garden(display, pointer);
    unsigned frames = conservatory(display, pointer, directory);
    frames += interpolated_frames(display, directory);
    frames += planting_motion(display);
    frames += beads_budget(display, directory);
    frames += conservatory_budget(display);
    for (orb_game_kind_t kind = ORB_GAME_ORBIT; kind < ORB_GAME_COUNT; ++kind) {
        orb_game_state_t state;
        orb_game_init(&state, kind, 0x4f5242u);
        s_model = &state;
        orb_game_view_t *view = orb_game_view_create(lv_screen_active(), kind, action, NULL);
        check(view != NULL, "create view");
        update(view, &state, 120, false, false);
        advance_clock(view, &state, 120, false, false, 800);
        update(view, &state, 120, false, false);
        lv_timer_handler();
        capture(display, directory, kind, "entry-playing-default");
        check(state.started && !state.user_paused && !state.game_over,
              "cold game entry is already playing");
        play_chrome();
        uint32_t before = state.ticks;
        /* Verify the active clock without steering Snake into a radial lane;
         * the moving-frame oracle below deliberately follows a safe orbit. */
        orb_game_step(&state, 0, 0, true, false);
        check(state.ticks == before + 1, "cold game entry advances without a second Play");
        update(view, &state, 120, false, false);
        lv_refr_now(display);
        unsigned objects = child_count(lv_screen_active());
        uint64_t frame_times[90], flushed_total = 0;
        uint32_t flushed_max = 0;
        for (unsigned frame = 0; frame < 90; ++frame) {
            if (frame == 12) trigger_reward(&state);
            /* The render budget follows a safe lane. Directional Snake
             * steering is exercised above; a held deliberate tilt here would
             * correctly end its round and invalidate the result surface. */
            float ax = 0.25f, ay = -0.1f;
            if (kind == ORB_GAME_SNAKE) {
                /* Drive a broad circle with absolute heading, continuously
                 * exercising retained body damage without hitting the rim. */
                float target = -1.5707963f + (float)frame * .025f;
                ax = -cosf(target) * .3f; ay = sinf(target) * .3f;
            }
            orb_game_step(&state, ax, ay, true, false);
            orb_game_step(&state, ax, ay, true, false);
            s_now += 33;
            lv_timer_handler();
            s_largest_damage = 0;
            s_flushed_pixels = 0;
            s_measure_flush = true;
            uint64_t begin = monotonic_us();
            update(view, &state, 120, false, false);
            lv_refr_now(display);
            frame_times[frame] = monotonic_us() - begin;
            s_measure_flush = false;
            flushed_total += s_flushed_pixels;
            if (s_flushed_pixels > flushed_max) flushed_max = s_flushed_pixels;
            if (s_largest_damage >= SIM_D * SIM_D / 2)
                fprintf(stderr, "moving fixture %s frame=%u ticks=%u score=%u lives=%u over=%u damage=%u\n",
                        orb_game_id(kind), frame, state.ticks, state.score, state.lives,
                        state.game_over, s_largest_damage);
            orb_game_view_stats_t frame_stats;
            orb_game_view_stats(view, &frame_stats);
            if (frame_stats.last_dirty_pixels >= SIM_D * SIM_D)
                fprintf(stderr, "moving damage %s frame=%u ticks=%u score=%u lives=%u over=%u "
                    "requested=%u max=%u flushed=%u largest=%u\n", orb_game_id(kind), frame,
                    state.ticks, state.score, state.lives, state.game_over,
                    frame_stats.last_dirty_pixels, frame_stats.max_dirty_pixels,
                    s_flushed_pixels, s_largest_damage);
            check(s_largest_damage < SIM_D * SIM_D / 2, "frame requests broad screen damage");
            check(child_count(lv_screen_active()) == objects, "frame creates/deletes widgets");
            frozen_frame(display);
            ++frames;
        }
        orb_game_view_stats_t stats;
        orb_game_view_stats(view, &stats);
        check(stats.updates >= 91, "view records every update");
        if (stats.max_dirty_pixels >= SIM_D * SIM_D)
            fprintf(stderr, "game damage %s updates=%llu last=%u max=%u sum=%llu flushed_max=%u\n",
                orb_game_id(kind), (unsigned long long)stats.updates, stats.last_dirty_pixels,
                stats.max_dirty_pixels, (unsigned long long)stats.dirty_pixels, flushed_max);
        check(stats.max_dirty_pixels < SIM_D * SIM_D, "view reports full-screen sprite damage");
        check(stats.changed_updates > 0, "moving fixture records changes");
        qsort(frame_times, 90, sizeof(frame_times[0]), compare_u64);
        check(frame_times[85] < 33333, "game simulator p95 exceeds 30 fps budget");
        printf("game %s: changed=%llu dirty_pixels/update=%.1f max=%u "
               "flushed_px mean=%.1f max=%u host_us p95=%llu max=%llu\n",
               orb_game_id(kind), (unsigned long long)stats.changed_updates,
               (double)stats.dirty_pixels / (double)stats.updates, stats.max_dirty_pixels,
               (double)flushed_total / 90, flushed_max,
               (unsigned long long)frame_times[85], (unsigned long long)frame_times[89]);

        /* Results stay until a deliberate restart; use a live fixture for each
         * game so every visible alternative is dispatched by actual pointer. */
        state.game_over = false;
        state.user_paused = false;
        update(view, &state, 120, false, false);
        if (kind == ORB_GAME_SNAKE) {
            state.controls = true;
            update(view, &state, 120, false, false);
            s_input.point = (lv_point_t){320, 280};
            s_input.state = LV_INDEV_STATE_PRESSED;
            lv_indev_read(pointer);
            s_now += 600;
            lv_indev_read(pointer);
            s_input.state = LV_INDEV_STATE_RELEASED;
            lv_indev_read(pointer);
            check(s_action == ORB_GAME_STEER_STOP && !state.game.snake.touch_held,
                "Snake hold release always stops absolute steering without pausing");
        } else {
            pointer_action(pointer, 233, kind == ORB_GAME_ORBIT ? 290 : 399, ORB_GAME_TAP,
                           "playfield tap dispatches the game action");
        }
        if (kind != ORB_GAME_SNAKE)
            pointer_action(pointer, 320, 200, ORB_GAME_TAP, "playfield tap reaches game input");
        if (kind == ORB_GAME_ORBIT || kind == ORB_GAME_POP) {
            s_actions = 0;
            s_input.point = (lv_point_t){320, 200};
            s_input.state = LV_INDEV_STATE_PRESSED;
            lv_indev_read(pointer);
            /* Several individually small movements must accumulate to a
             * cancelled tap, even when the canvas never starts scrolling. */
            for (unsigned move = 0; move < 4; ++move) {
                s_input.point.x += 4;
                lv_indev_read(pointer);
            }
            s_input.state = LV_INDEV_STATE_RELEASED;
            lv_indev_read(pointer);
            check(s_actions == 0, "short field drag never switches Orbit or pops a bubble");
            s_input.state = LV_INDEV_STATE_PRESSED;
            lv_indev_read(pointer);
            s_now += 600;
            lv_indev_read(pointer);
            s_input.state = LV_INDEV_STATE_RELEASED;
            lv_indev_read(pointer);
            check(s_actions == 0, "long field hold is not an Orbit or Pop tap");
        }
        if (kind == ORB_GAME_SNAKE) {
            tap(pointer, 233, 80);
            check(s_action == ORB_GAME_PAUSE && !state.game.snake.touch_held,
                "Snake short field tap releases steering then opens pause");
        } else {
            pointer_action(pointer, 233, kind == ORB_GAME_ORBIT ? 233 : 54,
                           ORB_GAME_PAUSE, "score pause alternative dispatches once");
        }
        check(state.user_paused, "user pause changes model state");
        update(view, &state, 120, false, false);
        advance_clock(view, &state, 120, false, false, 800);
        update(view, &state, 120, false, false);
        lv_timer_handler();
        check(has_text(lv_screen_active(), "Resume"), "pause offers Resume");
        before = state.ticks;
        orb_game_step(&state, 0.5f, 0, true, false);
        check(state.ticks == before, "user pause freezes fixed-step logic");
        check(!has_text(lv_screen_active(), ORB_ICON_X), "paused scene has no floating Close");
        pointer_action(pointer, 233, 329, ORB_GAME_PAUSE, "Resume dispatches exactly once");
        check(!state.user_paused, "Resume preserves the existing round");
        update(view, &state, 120, false, false);
        check(!has_text(lv_screen_active(), "Resume"), "outgoing Resume hides on entering play");
        check(!has_text(lv_screen_active(), "Tap to Play"), "wake never advertises a new start over a live round");
        advance_clock(view, &state, 120, false, false, 800);
        update(view, &state, 120, false, false);
        lv_refr_now(display);

        /* A frozen model at a frozen clock has no visual debt. Reduce Motion
         * also suppresses all decorative attract/reward/odometer movement. */
        orb_ui_set_reduce_motion(true);
        state.reduce_motion = true;
        update(view, &state, 120, false, false);
        frozen_frame(display);
        s_largest_damage = 0;
        update(view, &state, 120, false, false);
        check(s_largest_damage == 0, "unchanged model invalidates display");

        check(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK, "load AX3 fonts");
        check(orb_fonts_set_bold_text(true) == ORB_FONTS_OK, "load AX3 Bold Text for error layout");
        orb_ui_set_reduce_transparency(true);
        state.user_paused = true;
        update(view, &state, 120, false, true);
        advance_clock(view, &state, 120, false, true, 800);
        capture(display, directory, kind, "paused-ax3-save-error");
        check(has_text(lv_screen_active(), "Not saved"), "pause exposes score failure");
        check(has_text(lv_screen_active(), "Paused"), "save error never erases Paused");
        check(has_text(lv_screen_active(), "Resume"), "save error retains Resume");
        lv_area_t end_box, pause_warning;
        lv_obj_get_coords(find_text(lv_screen_active(), "End round"), &end_box);
        lv_obj_get_coords(find_text(lv_screen_active(), "Not saved"), &pause_warning);
        check(end_box.y1 - pause_warning.y2 >= 16, "pause action and error retain readable separation");
        state.user_paused = false;
        state.lives = 1;
        state.combo = 5;
        update(view, &state, 120, false, true);
        capture(display, directory, kind, "save-error-ax3");
        check(!has_text(lv_screen_active(), "Not saved"),
              "save failure stays on Pause/Result and leaves the arena clear");
        state.game_over = true;
        state.lives = 0;
        state.score = 180;
        update(view, &state, 120, false, true);
        advance_clock(view, &state, 120, false, true, 800);
        update(view, &state, 120, false, true);
        capture(display, directory, kind, "over-save-error-ax3");
        check(has_text(lv_screen_active(), "Not saved"), "result exposes score failure");
        check(has_text(lv_screen_active(), "Play again"), "result has a specific restart action");
        lv_area_t score_box, error_box, action_box;
        lv_obj_get_coords(find_text(lv_screen_active(), "180"), &score_box);
        lv_obj_get_coords(find_text(lv_screen_active(), "Not saved"), &error_box);
        lv_obj_get_coords(lv_obj_get_parent(find_text(lv_screen_active(), "Play again")), &action_box);
        check(error_box.y1 - score_box.y2 >= 0, "final score and error do not overlap");
        check(action_box.y1 - error_box.y2 >= 0, "error clears Play again");
        check(has_text(lv_screen_active(), "Done"), "result retains a visible Done action");
        pointer_action(pointer, 233, 314, ORB_GAME_RESTART, "Play again dispatches exactly once");
        check(!state.game_over && state.started && state.score == 0, "Play Again starts a clean round");
        check(orb_fonts_set_bold_text(true) == ORB_FONTS_OK, "load Bold Text");
        state.score = 999999;
        state.combo = 99;
        update(view, &state, 999999, false, false);
        capture(display, directory, kind, "large-score-ax3");
        /* Scored HUDs compact large counters. Review #12: Snake now shares
         * Pop's top score disc (pellets never spawn under it, and it is not a
         * tap target over the steering field); the exact score is on its
         * reading surface. */
        lv_obj_t *score_label = find_text(lv_screen_active(), "999k");
        if (kind == ORB_GAME_SNAKE) {
            check(score_label != NULL, "Snake HUD shows its compact live score");
            check(!lv_obj_is_clickable(lv_obj_get_parent(lv_obj_get_parent(score_label))),
                  "Snake score disc never intercepts a steering hold");
            state.game_over = true;
            update(view, &state, 999999, false, false);
            advance_clock(view, &state, 999999, false, false, 800);
            score_label = find_text(lv_screen_active(), "999999");
            capture(display, directory, kind, "large-score-result-ax3");
        }
        check(score_label != NULL, "large score remains visible on its reading surface");
        check(lv_obj_get_height(score_label) <=
              2 * lv_font_get_line_height(lv_obj_get_style_text_font(score_label, 0)) + 2,
              "large score fits at AX3/Bold");
        state.user_paused = true;
        update(view, &state, 999999, false, false);
        advance_clock(view, &state, 999999, false, false, 800);
        pointer_action(pointer, 233, 415, ORB_GAME_CLOSE, "quiet Done dispatches exactly once before exit");
        orb_game_view_destroy(view);
        s_model = NULL;
        check(child_count(lv_screen_active()) == 0, "view deletion releases widgets");
        check(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK, "restore default fonts");
        orb_ui_set_reduce_transparency(false);
        orb_ui_set_reduce_motion(false);
        check(orb_fonts_set_bold_text(false) == ORB_FONTS_OK, "restore normal text weight");
    }
    screen_isolation(display);
    printf("games_render: %u checks, %u moving and %u transition frozen partial/full frames plus lifecycle fixtures\n",
           s_checks, frames, transitions);
    return EXIT_SUCCESS;
}
