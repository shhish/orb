/* Deterministic game previews use the firmware logic and retained sprite view.
 * The SDL window is a fake board: mouse = touch, arrow keys = physical device tilt. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "orb_game_logic.h"
#include "orb_game_interpolate.h"
#include "orb_game_view.h"
#include "orb_fonts.h"
#include "orb_ui.h"
#include "sim_board.h"
#include "sim_display.h"
#include "sim_window.h"

#define MAX_EVENTS 64

typedef struct {
    uint32_t ms;
    float x, y;
    const char *path;
} sim_event_t;

static uint32_t s_now;
static bool s_headless, s_closed, s_save_error, s_design_frame;
static struct timespec s_epoch;
static float s_right, s_down;
static orb_game_state_t s_game, s_previous, s_rendering;
static uint64_t s_accumulated;
static orb_game_tilt_t s_tilt;
static orb_game_view_t *s_view;
static uint32_t s_best = 120, s_end_at = UINT32_MAX, s_pause_at = UINT32_MAX;
static uint32_t s_reward_at = UINT32_MAX, s_handheld_until;
static sim_event_t s_shots[MAX_EVENTS], s_tilts[MAX_EVENTS], s_taps[MAX_EVENTS];
static sim_event_t s_presses[MAX_EVENTS], s_releases[MAX_EVENTS];
static size_t s_shot_count, s_tilt_count, s_tap_count, s_press_count, s_release_count;
static uint32_t s_bubble_taps[MAX_EVENTS];
static size_t s_bubble_tap_count;
static lv_indev_data_t s_pointer_data;
static lv_indev_t *s_pointer;

static uint32_t tick_ms(void)
{
    if (s_headless) return s_now;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((t.tv_sec - s_epoch.tv_sec) * 1000 +
                      (t.tv_nsec - s_epoch.tv_nsec) / 1000000);
}

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    *data = s_pointer_data;
}

static void game_action(int action, float x, float y, void *ctx)
{
    (void)ctx;
    if (action == ORB_GAME_CLOSE) s_closed = true;
    else {
        bool started = s_game.started, paused = s_game.user_paused;
        orb_game_action(&s_game, (orb_game_action_t)action, x, y);
        if (s_game.kind == ORB_GAME_MARBLE && action == ORB_GAME_FINISH &&
            paused && s_game.game_over) s_closed = true;
        if ((!started && s_game.started) || action == ORB_GAME_RESTART)
            orb_game_tilt_reset(&s_tilt);
    }
}

static void reward_fixture(void)
{
    /* Put collection targets on the next legal move, then exercise the real
     * rules. This records bounded feedback without a second renderer/model. */
    if (s_game.kind == ORB_GAME_MARBLE) {
        s_game.game.marble.x = s_game.game.marble.y = 0;
        s_game.game.marble.vx = s_game.game.marble.vy = 0;
        orb_game_step(&s_game, 0, 0, true, orb_ui_reduce_motion());
    } else if (s_game.kind == ORB_GAME_SNAKE) {
        s_game.game.snake.food = s_game.game.snake.cells[0];
        for (unsigned i = 0; i < 12; ++i)
            orb_game_step(&s_game, 0, 0, true, orb_ui_reduce_motion());
    } else if (s_game.kind == ORB_GAME_ORBIT) {
        s_game.game.orbit.obstacles[0] = (orb_game_obstacle_t){
            .angle = 1.5707963f - 0.01f, .ring = s_game.game.orbit.ring};
        orb_game_step(&s_game, 0, 0, true, orb_ui_reduce_motion());
    } else orb_game_action(&s_game, ORB_GAME_PRIMARY, 0, 0);
}

/* A design comparison must show the same moment as the exported frame,
 * rather than a random early round. These are real model states rendered by
 * the firmware view; normal simulator play and its physics stay unchanged. */
static void design_frame_fixture(void)
{
    const float rad = 6.28318530718f / 360;
    switch (s_game.kind) {
    case ORB_GAME_MARBLE:
        s_game.game.marble.x = -192; s_game.game.marble.y = 70;
        s_game.game.marble.gates[0].angle = 30 * rad;
        s_game.game.marble.gates[0].gap_angle = 35 * rad;
        s_game.game.marble.gates[1].angle = 150 * rad;
        s_game.game.marble.gates[1].gap_angle = 25 * rad;
        s_game.game.marble.gates[2].angle = 300 * rad;
        s_game.game.marble.gates[2].gap_angle = 20 * rad;
        s_game.game.marble.trail_count = 3;
        s_game.game.marble.trail[0] = (orb_game_point_t){-201.9f, 35.6f};
        s_game.game.marble.trail[1] = (orb_game_point_t){-205, 0};
        s_game.game.marble.trail[2] = (orb_game_point_t){-201.9f, -35.6f};
        break;
    case ORB_GAME_ORBIT:
        s_game.game.orbit.ring = 1;
        s_game.game.orbit.angle = -45 * rad;
        s_game.game.orbit.trail_age = 0;
        for (unsigned i = 0; i < ORB_GAME_OBSTACLES; ++i)
            s_game.game.orbit.obstacles[i] = (orb_game_obstacle_t){0, ORB_GAME_ORBIT_RINGS, false};
        s_game.game.orbit.obstacles[0] = (orb_game_obstacle_t){atan2f(135.8f, -96), 1, false};
        s_game.game.orbit.comet_radius[0] = hypotf(96, 135.8f);
        s_game.game.orbit.comet_speed[0] = -1.05f;
        for (unsigned i = 0; i < ORB_GAME_ORBIT_STARS; ++i)
            s_game.game.orbit.stars[i] = (orb_game_star_t){-90 * rad, 2, i == 0};
        break;
    case ORB_GAME_SNAKE:
        s_game.game.snake.length = 9;
        s_game.game.snake.food_age = 12;
        for (unsigned i = 0; i < s_game.game.snake.length; ++i) {
            float angle = -2.1f + (float)i * .12f;
            s_game.game.snake.cells[i] = (orb_game_point_t){80 * cosf(angle), 80 * sinf(angle)};
        }
        s_game.game.snake.heading = -2.1f - 1.5707963f;
        s_game.game.snake.food = (orb_game_point_t){-100, 30};
        break;
    case ORB_GAME_POP: {
        static const orb_game_bubble_t s_design_bubbles[] = {
            {-73, 67, 0, 0, 44, true}, {67, 17, 0, 0, 32, true},
            {13, 127, 0, 0, 22, true}, {97, 137, 0, 0, 16, true},
            {-33, -63, 0, 0, 26, true}};
        memset(s_game.game.pop.bubbles, 0, sizeof(s_game.game.pop.bubbles));
        memcpy(s_game.game.pop.bubbles, s_design_bubbles, sizeof(s_design_bubbles));
        s_game.game.pop.time_left = ORB_GAME_POP_ROUND_STEPS * 119 / 200;
        s_game.game.pop.reward = 3;
        s_game.effect = (orb_game_effect_t){85, -63, 0xffd600, 0, 24, 5, 0};
        break;
    }
    default: break;
    }
}

static void game_tick(lv_timer_t *timer)
{
    (void)timer;
    static uint32_t last;
    uint32_t now = lv_tick_get();
    uint32_t elapsed = now - last;
    last = now;
    /* Same bounded fixed-step debt as firmware: don't catch up stale minutes. */
    s_accumulated += (uint64_t)(elapsed > 100 ? 100 : elapsed) * 1000;
    float gravity[3];
    sim_board_gravity(s_right, s_down, gravity);
    orb_ui_set_gravity(gravity[0], gravity[1], gravity[2], 0.02f);
    float ax = gravity[0], ay = gravity[1];
    bool tilt_game = s_game.kind == ORB_GAME_MARBLE || s_game.kind == ORB_GAME_BEADS ||
                     s_game.kind == ORB_GAME_SNAKE;
    s_game.tilt_calibrating = false;
    if (tilt_game && s_game.started && !s_game.user_paused && !s_game.game_over &&
        (s_game.kind != ORB_GAME_MARBLE ||
            (!s_game.game.marble.garden_open && !s_game.game.marble.stage_cleared)) && !s_game.controls) {
        static float previous_right, previous_down;
        float scale = elapsed ? 1000.0f / (float)elapsed : 0;
        float gyro[3] = {(s_down - previous_down) * scale,
                         (s_right - previous_right) * scale, 0};
        previous_right = s_right; previous_down = s_down;
        /* Explicit reproducible handheld fixture; a perfectly fixed fake board
         * otherwise correctly qualifies as a desk/stand at any tilt angle. */
        if (now < s_handheld_until) gyro[2] = 0.8f * sinf((float)now * 0.071f);
        bool valid = s_game.kind == ORB_GAME_BEADS ?
            orb_game_gravity_update_motion(&s_tilt, gravity[0], gravity[1], gravity[2],
                gyro, elapsed * 1000, &ax, &ay) :
            orb_game_tilt_update_motion(&s_tilt, gravity[0], gravity[1], gravity[2],
                gyro, elapsed * 1000, &ax, &ay);
        if (!valid) s_accumulated = 0;
        s_game.tilt_calibrating = !s_tilt.ready;
        orb_game_set_motion(&s_game, &s_tilt);
    }
    while (s_accumulated >= ORB_GAME_STEP_US) {
        s_previous = s_game;
        if (!s_design_frame || s_game.kind == ORB_GAME_BEADS)
            orb_game_step(&s_game, ax, ay, true, orb_ui_reduce_motion());
        s_accumulated -= ORB_GAME_STEP_US;
    }
    /* These explicit fixtures exercise results without changing game rules or
     * waiting for a deterministic attract scene to lose all its lives. */
    if (now >= s_pause_at) {
        orb_game_action(&s_game, ORB_GAME_PAUSE, 0, 0);
        s_pause_at = UINT32_MAX;
    }
    if (now >= s_reward_at) {
        reward_fixture();
        s_reward_at = UINT32_MAX;
    }
    if (now >= s_end_at) {
        s_game.started = true;
        s_game.game_over = true;
        s_game.lives = 0;
        s_end_at = UINT32_MAX;
    }
    if (s_game.started && s_game.score > s_best) s_best = s_game.score;
}

static void game_render(lv_timer_t *timer)
{
    (void)timer;
    s_rendering = s_game;
    orb_game_interpolate(&s_previous, &s_rendering,
                         (float)s_accumulated / (float)ORB_GAME_STEP_US);
    orb_game_view_update(s_view, &s_rendering, s_best, false, s_save_error);
}

static int event_order(const void *left, const void *right)
{
    const sim_event_t *a = left, *b = right;
    return a->ms < b->ms ? -1 : a->ms > b->ms;
}

static bool add_event(sim_event_t *events, size_t *count, const char *arg, bool shot)
{
    if (*count >= MAX_EVENTS) return false;
    char *end;
    unsigned long ms = strtoul(arg, &end, 10);
    if (end == arg || *end != '=' || ms > 3600000) return false;
    sim_event_t event = { .ms = (uint32_t)ms };
    if (shot) {
        if (!end[1]) return false;
        event.path = end + 1;
    } else {
        char trailing;
        if (sscanf(end + 1, "%f,%f%c", &event.x, &event.y, &trailing) != 2 ||
            !isfinite(event.x) || !isfinite(event.y)) return false;
    }
    events[(*count)++] = event;
    return true;
}

static void pointer(float x, float y, bool pressed)
{
    s_pointer_data.point = (lv_point_t){(int32_t)x, (int32_t)y};
    s_pointer_data.state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer);
}

static void tap(float x, float y)
{
    pointer(x, y, true);
    pointer(x, y, false);
}

static void usage(void)
{
    fputs("usage: orb_games_sim --game marble|beads|orbit|ring-snake|pop [options]\n"
          "  --headless --shot MS=FILE  deterministic PNG capture\n"
          "  --tilt MS=RIGHT,DOWN      fake board tilt in degrees\n"
          "  --handheld-until MS       deterministic gyro jitter, then desk rest\n"
          "  --tap MS=X,Y              pointer press and release\n"
          "  --press MS=X,Y --release MS=X,Y  held pointer input\n"
          "  --bubble-tap MS            real pointer tap at a live Pop bubble\n"
          "  --text-size default|larger|ax3 --bold\n"
          "  --scene start|play|paused|end|finished|cleared|garden|garden-empty --controls\n"
          "  --design-frame             frozen Design 1.0 model snapshot (headless)\n"
          "  --stage 1..3 --keepsakes 0..12  Marble fixtures\n"
          "  --best N --score N --end MS --pause MS --reward MS   synthetic fixtures\n"
          "  --reduce-motion --reduce-transparency --game-over --save-error\n", stderr);
}

static bool unsigned_option(const char *value, uint32_t *out)
{
    char *end;
    unsigned long number = strtoul(value, &end, 10);
    if (!*value || *value == '-' || *end || number > UINT32_MAX) return false;
    *out = (uint32_t)number;
    return true;
}

int main(int argc, char **argv)
{
    orb_game_kind_t kind = ORB_GAME_MARBLE;
    orb_content_size_t size = ORB_CONTENT_SIZE_DEFAULT;
    bool reduce_motion = false, reduce_transparency = false, bold = false, controls = false;
    const char *scene = "start";
    uint32_t score = 0, stage = 1, keepsakes = 0;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i], *value = i + 1 < argc ? argv[i + 1] : NULL;
        bool ok = true;
        if (!strcmp(arg, "--headless")) s_headless = true;
        else if (!strcmp(arg, "--reduce-motion")) reduce_motion = true;
        else if (!strcmp(arg, "--reduce-transparency")) reduce_transparency = true;
        else if (!strcmp(arg, "--bold")) bold = true;
        else if (!strcmp(arg, "--game-over")) scene = "end";
        else if (!strcmp(arg, "--controls")) controls = true;
        else if (!strcmp(arg, "--design-frame")) s_design_frame = true;
        else if (!strcmp(arg, "--save-error")) s_save_error = true;
        else if (!strcmp(arg, "--help")) { usage(); return 0; }
        else if (value && !strcmp(arg, "--game")) {
            ok = orb_game_kind_from_id(value, &kind); ++i;
        } else if (value && !strcmp(arg, "--scene")) {
            ok = !strcmp(value, "start") || !strcmp(value, "play") ||
                 !strcmp(value, "paused") || !strcmp(value, "end") ||
                 !strcmp(value, "finished") || !strcmp(value, "cleared") || !strcmp(value, "garden") ||
                 !strcmp(value, "garden-empty");
            scene = value; ++i;
        } else if (value && !strcmp(arg, "--stage")) {
            ok = unsigned_option(value, &stage) && stage >= 1 && stage <= ORB_MARBLE_STAGES; ++i;
        } else if (value && !strcmp(arg, "--keepsakes")) {
            ok = unsigned_option(value, &keepsakes) && keepsakes <= ORB_MARBLE_KEEPSAKES_MAX; ++i;
        } else if (value && !strcmp(arg, "--best")) {
            ok = unsigned_option(value, &s_best); ++i;
        } else if (value && !strcmp(arg, "--score")) {
            ok = unsigned_option(value, &score); ++i;
        } else if (value && !strcmp(arg, "--end")) {
            ok = unsigned_option(value, &s_end_at); ++i;
        } else if (value && !strcmp(arg, "--pause")) {
            ok = unsigned_option(value, &s_pause_at); ++i;
        } else if (value && !strcmp(arg, "--reward")) {
            ok = unsigned_option(value, &s_reward_at); ++i;
        } else if (value && !strcmp(arg, "--text-size")) {
            if (!strcmp(value, "default")) size = ORB_CONTENT_SIZE_DEFAULT;
            else if (!strcmp(value, "larger")) size = ORB_CONTENT_SIZE_LARGER;
            else if (!strcmp(value, "ax3")) size = ORB_CONTENT_SIZE_AX3;
            else ok = false;
            ++i;
        } else if (value && !strcmp(arg, "--shot")) {
            ok = add_event(s_shots, &s_shot_count, value, true); ++i;
        } else if (value && !strcmp(arg, "--handheld-until")) {
            ok = unsigned_option(value, &s_handheld_until) && s_handheld_until <= 3600000; ++i;
        } else if (value && !strcmp(arg, "--tilt")) {
            ok = add_event(s_tilts, &s_tilt_count, value, false); ++i;
        } else if (value && !strcmp(arg, "--tap")) {
            ok = add_event(s_taps, &s_tap_count, value, false); ++i;
        } else if (value && !strcmp(arg, "--press")) {
            ok = add_event(s_presses, &s_press_count, value, false); ++i;
        } else if (value && !strcmp(arg, "--release")) {
            ok = add_event(s_releases, &s_release_count, value, false); ++i;
        } else if (value && !strcmp(arg, "--bubble-tap")) {
            ok = s_bubble_tap_count < MAX_EVENTS &&
                unsigned_option(value, &s_bubble_taps[s_bubble_tap_count]);
            if (ok) ++s_bubble_tap_count;
            ++i;
        } else ok = false;
        if (!ok) { fprintf(stderr, "bad game simulator option: %s\n", arg); usage(); return 2; }
    }
    if ((s_design_frame && !s_headless) || (s_headless && !s_shot_count) || (!s_headless &&
        (s_shot_count || s_tilt_count || s_tap_count || s_press_count || s_release_count))) {
        usage(); return 2;
    }
    qsort(s_shots, s_shot_count, sizeof(s_shots[0]), event_order);
    qsort(s_tilts, s_tilt_count, sizeof(s_tilts[0]), event_order);
    qsort(s_taps, s_tap_count, sizeof(s_taps[0]), event_order);
    qsort(s_presses, s_press_count, sizeof(s_presses[0]), event_order);
    qsort(s_releases, s_release_count, sizeof(s_releases[0]), event_order);
    clock_gettime(CLOCK_MONOTONIC, &s_epoch);
    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *display = sim_display_create();
    orb_ui_init(NULL);
    if (orb_fonts_set_content_size(size) != ORB_FONTS_OK ||
        orb_fonts_set_bold_text(bold) != ORB_FONTS_OK) return 1;
    orb_ui_set_reduce_motion(reduce_motion);
    orb_ui_set_reduce_transparency(reduce_transparency);
    orb_game_init(&s_game, kind, 0x4f5242u);
    if (kind == ORB_GAME_BEADS) orb_game_beads_set_time(&s_game, 10, 9);
    if (kind == ORB_GAME_MARBLE) {
        uint32_t fixture_stage = !strcmp(scene, "finished") ? ORB_MARBLE_STAGES : stage;
        for (uint32_t i = 1; i < fixture_stage; ++i) {
            reward_fixture();
            orb_game_action(&s_game, ORB_GAME_PRIMARY, 0, 0);
        }
        if (!strcmp(scene, "cleared") || !strcmp(scene, "finished")) reward_fixture();
        if (!strcmp(scene, "finished")) orb_game_action(&s_game, ORB_GAME_PRIMARY, 0, 0);
        bool garden = !strcmp(scene, "garden") || !strcmp(scene, "garden-empty");
        if (garden && !strcmp(scene, "garden") && !keepsakes) keepsakes = 6;
        for (uint32_t i = 0; i < keepsakes; ++i) {
            float angle = (float)i * 2.39996323f;
            float radius = 26 + 22 * sqrtf((float)i);
            s_game.game.marble.keepsakes[i] = (orb_marble_keepsake_t){
                .kind = (uint8_t)(i % 3), .stage = (uint8_t)(1 + i % 3),
                .x = (int16_t)lroundf(cosf(angle) * radius),
                .y = (int16_t)lroundf(sinf(angle) * radius)};
        }
        if (keepsakes) {
            s_game.game.marble.keepsake_count = (uint8_t)keepsakes;
            s_game.game.marble.stages_cleared = (uint8_t)(keepsakes < ORB_MARBLE_STAGES ? keepsakes : ORB_MARBLE_STAGES);
        }
        s_game.game.marble.garden_open = garden;
    }
    s_game.started = strcmp(scene, "start") && strcmp(scene, "garden") && strcmp(scene, "garden-empty");
    s_game.user_paused = !strcmp(scene, "paused");
    s_game.game_over = !strcmp(scene, "end") || !strcmp(scene, "finished");
    s_game.reduce_motion = reduce_motion;
    s_game.controls = controls;
    s_game.score = score;
    if (s_design_frame && !strcmp(scene, "play")) design_frame_fixture();
    if (s_game.game_over) s_game.lives = 0;
    s_view = orb_game_view_create(lv_screen_active(), kind, game_action, NULL);
    if (!s_view) return 1;
    orb_game_view_update(s_view, &s_game, s_best, false, s_save_error);
    /* Sample at the board game's 100 Hz rate; present interpolated fixed steps. */
    lv_timer_create(game_tick, 10, NULL);
    lv_timer_create(game_render, 17, NULL);
    if (!s_headless) {
        if (!sim_window_open(display, ".")) return 1;
        while (!s_closed) {
            sim_window_tilt(&s_right, &s_down);
            lv_lock();
            uint32_t wait = lv_timer_handler();
            lv_unlock();
            if (sim_display_take_changed()) sim_window_present();
            if (!sim_window_poll(wait < 16 ? wait : 16)) break;
        }
        sim_window_close();
    } else {
        s_pointer = lv_indev_create();
        lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(s_pointer, display);
        lv_indev_set_read_cb(s_pointer, pointer_read);
        size_t shot = 0, tilt = 0, tap_index = 0, press_index = 0, release_index = 0;
        while (shot < s_shot_count && !s_closed) {
            while (tilt < s_tilt_count && s_tilts[tilt].ms <= s_now) {
                s_right = s_tilts[tilt].x; s_down = s_tilts[tilt++].y;
            }
            while (tap_index < s_tap_count && s_taps[tap_index].ms <= s_now) {
                tap(s_taps[tap_index].x, s_taps[tap_index].y); ++tap_index;
            }
            while (press_index < s_press_count && s_presses[press_index].ms <= s_now) {
                pointer(s_presses[press_index].x, s_presses[press_index].y, true); ++press_index;
            }
            while (release_index < s_release_count && s_releases[release_index].ms <= s_now) {
                pointer(s_releases[release_index].x, s_releases[release_index].y, false); ++release_index;
            }
            for (size_t i = 0; i < s_bubble_tap_count; ++i) {
                if (s_bubble_taps[i] > s_now || s_game.kind != ORB_GAME_POP) continue;
                bool found = false;
                for (unsigned n = 0; n < ORB_GAME_BUBBLES; ++n) {
                    const orb_game_bubble_t *bubble = &s_game.game.pop.bubbles[n];
                    if (!bubble->active) continue;
                    uint32_t before = s_game.score;
                    tap(ORB_GAME_CX + bubble->x, ORB_GAME_CY + bubble->y);
                    if (s_game.score <= before) {
                        fputs("live bubble pointer tap missed its visible target\n", stderr);
                        return 1;
                    }
                    found = true;
                    break;
                }
                if (!found) return 1;
                s_bubble_taps[i] = UINT32_MAX;
            }
            lv_timer_handler();
            while (shot < s_shot_count && s_shots[shot].ms <= s_now) {
                lv_refr_now(display);
                if (!sim_display_save_png(s_shots[shot].path)) return 1;
                printf("orb_games_sim: %s %u ms: %s\n", orb_game_id(kind), s_now, s_shots[shot++].path);
            }
            ++s_now;
        }
        if (shot < s_shot_count) return 1;
    }
    orb_game_view_destroy(s_view);
    return 0;
}
