/* Deterministic SDL events test the transport boundary: no GUI, hardware,
 * synthetic gesture substitute, or owner service is involved. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "emu_input.h"
#include "orb_buttons.h"
#include "sim_display.h"

static unsigned s_checks, s_actions[EMU_INPUT_QUIT + 1];
static int s_value;
static uint32_t s_now;
static unsigned s_pressed, s_released, s_clicked, s_resets;
static lv_point_t s_point;

#define CHECK(value) do { \
    s_checks++; \
    if (!(value)) { \
        fprintf(stderr, "emu input %s:%d: %s\n", __FILE__, __LINE__, #value); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static uint32_t tick(void) { return s_now; }

static void action(emu_input_action_t kind, int value, void *ctx)
{
    (void)ctx;
    s_actions[kind]++;
    s_value = value;
}

static void touch_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) s_pressed++;
    if (code == LV_EVENT_RELEASED) s_released++;
    if (code == LV_EVENT_CLICKED) s_clicked++;
    if (code == LV_EVENT_INDEV_RESET) s_resets++;
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_get_point(indev, &s_point);
}

static void event(emu_input_t *input, SDL_Event *e, uint32_t now)
{
    s_now = now;
    emu_input_event(input, e, now);
}

static void key(emu_input_t *input, SDL_Keycode code, bool down, bool repeat, uint32_t now)
{
    SDL_Event e = {.key = {.type = down ? SDL_KEYDOWN : SDL_KEYUP,
        .repeat = repeat, .keysym = {.sym = code}}};
    event(input, &e, now);
}

static void mouse(emu_input_t *input, uint32_t type, int x, int y, uint32_t now)
{
    SDL_Event e = {.button = {.type = type, .button = SDL_BUTTON_LEFT, .x = x, .y = y}};
    if (type == SDL_MOUSEMOTION) e.motion = (SDL_MouseMotionEvent){.type = type, .x = x, .y = y};
    event(input, &e, now);
}

static void step(emu_input_t *input, uint32_t now)
{
    s_now = now;
    emu_input_step(input, now);
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *display = sim_display_create();
    CHECK(display);
    lv_obj_t *target = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(target, 0, 0);
    lv_obj_set_size(target, SIM_D, SIM_D);
    lv_obj_set_scrollable(target, false);
    lv_obj_add_event_cb(target, touch_event, LV_EVENT_ALL, NULL);
    lv_obj_update_layout(target);
    emu_input_t *input = emu_input_create(display, action, NULL);
    CHECK(input);
    emu_input_viewport(input, 20, 10, SIM_D * 2, SIM_D * 2);

    /* Same-poll press/release must be a complete real LVGL click at 2x. */
    mouse(input, SDL_MOUSEBUTTONDOWN, 20 + 233 * 2, 10 + 233 * 2, 0);
    mouse(input, SDL_MOUSEBUTTONUP, 20 + 233 * 2, 10 + 233 * 2, 0);
    CHECK(s_pressed == 1 && s_released == 1 && s_clicked == 1);
    CHECK(s_point.x == 233 && s_point.y == 233);
    mouse(input, SDL_MOUSEBUTTONDOWN, 20, 10, 10);
    mouse(input, SDL_MOUSEBUTTONUP, 20, 10, 10);
    CHECK(s_pressed == 1);

    mouse(input, SDL_MOUSEBUTTONDOWN, 20 + 450 * 2, 10 + 233 * 2, 20);
    mouse(input, SDL_MOUSEMOTION, 2000, 2000, 40);
    mouse(input, SDL_MOUSEBUTTONUP, 2000, 2000, 50);
    CHECK(s_pressed == 2 && s_released == 2);
    CHECK(hypotf(s_point.x - 233, s_point.y - 233) <= 233);

    /* SDL owns only raw state. Timing and gestures belong to orb_buttons. */
    key(input, SDLK_p, true, false, 100);
    CHECK(emu_input_buttons(input) == (1u << ORB_BUTTON_PWR));
    key(input, SDLK_p, true, true, 120);
    CHECK(emu_input_buttons(input) == (1u << ORB_BUTTON_PWR));
    key(input, SDLK_p, false, false, 150);
    CHECK(emu_input_buttons(input) == 0);
    step(input, 449);
    step(input, 450);
    key(input, SDLK_p, true, false, 500);
    key(input, SDLK_p, false, false, 550);
    key(input, SDLK_p, true, false, 700);
    key(input, SDLK_p, false, false, 750);
    step(input, 1100);
    CHECK(emu_input_buttons(input) == 0);

    key(input, SDLK_p, true, false, 1200);
    step(input, 3300);
    CHECK(emu_input_buttons(input) == (1u << ORB_BUTTON_PWR));
    key(input, SDLK_b, true, false, 3400);
    CHECK(emu_input_buttons(input) == ((1u << ORB_BUTTON_PWR) | (1u << ORB_BUTTON_BOOT)));
    key(input, SDLK_p, false, false, 3500);
    CHECK(emu_input_buttons(input) == (1u << ORB_BUTTON_BOOT));
    key(input, SDLK_b, false, false, 3800);
    CHECK(emu_input_buttons(input) == 0);
    for (unsigned i = 0; i <= EMU_INPUT_QUIT; i++) CHECK(s_actions[i] == 0);

    float before[3], gravity[3];
    emu_input_gravity(input, before);
    key(input, SDLK_RIGHT, true, false, 4000);
    step(input, 4016);
    emu_input_gravity(input, gravity);
    CHECK(gravity[0] < before[0] && gravity[0] > -0.1f);
    step(input, 4400);
    emu_input_gravity(input, gravity);
    CHECK(gravity[0] < -0.3f);
    key(input, SDLK_RIGHT, false, false, 4400);
    step(input, 5000);
    emu_input_gravity(input, gravity);
    CHECK(fabsf(gravity[0]) < 0.02f);

    /* Option-motion tilts without creating an accidental touch. */
    key(input, SDLK_LALT, true, false, 5100);
    mouse(input, SDL_MOUSEMOTION, 20 + 430 * 2, 10 + 233 * 2, 5100);
    mouse(input, SDL_MOUSEBUTTONDOWN, 20 + 430 * 2, 10 + 233 * 2, 5100);
    step(input, 5600);
    emu_input_gravity(input, gravity);
    CHECK(gravity[0] < -0.4f && s_pressed == 2);
    key(input, SDLK_LALT, false, false, 5600);
    step(input, 6200);
    emu_input_gravity(input, gravity);
    CHECK(fabsf(gravity[0]) < 0.02f);
    key(input, SDLK_f, true, false, 6300);
    key(input, SDLK_f, true, true, 6310);
    CHECK(emu_input_face_down(input) && s_actions[EMU_INPUT_FACE_DOWN] == 1 && s_value == 1);
    emu_input_gravity(input, gravity);
    CHECK(gravity[2] < -0.99f);

    key(input, SDLK_3, true, false, 6400);
    CHECK(s_actions[EMU_INPUT_NAVIGATE] == 1 && s_value == 3);
    key(input, SDLK_d, true, false, 6410);
    key(input, SDLK_s, true, false, 6420);
    key(input, SDLK_ESCAPE, true, false, 6430);
    CHECK(s_actions[EMU_INPUT_DIAGNOSTICS] == 1 && s_actions[EMU_INPUT_SCREENSHOT] == 1 &&
          s_actions[EMU_INPUT_QUIT] == 1);

    /* Losing focus releases raw buttons and cancels a pending touch. */
    key(input, SDLK_p, true, false, 6500);
    key(input, SDLK_b, true, false, 6500);
    step(input, 6900);
    mouse(input, SDL_MOUSEBUTTONDOWN, 20 + 233 * 2, 10 + 233 * 2, 6900);
    unsigned clicks_before_cancel = s_clicked;
    SDL_Event lost = {.window = {.type = SDL_WINDOWEVENT, .event = SDL_WINDOWEVENT_FOCUS_LOST}};
    event(input, &lost, 6910);
    CHECK(emu_input_buttons(input) == 0);
    CHECK(s_pressed == 3 && s_released == 2 && s_resets == 1);
    CHECK(s_clicked == clicks_before_cancel);
    step(input, 9000);
    CHECK(emu_input_buttons(input) == 0);

    mouse(input, SDL_MOUSEBUTTONDOWN, 20 + 233 * 2, 10 + 233 * 2, 9010);
    key(input, SDLK_LALT, true, false, 9020);
    mouse(input, SDL_MOUSEBUTTONUP, 20 + 233 * 2, 10 + 233 * 2, 9030);
    CHECK(s_resets == 2 && s_clicked == clicks_before_cancel);

    emu_input_destroy(input);
    lv_obj_delete(target);
    lv_display_delete(display);
    lv_deinit();
    printf("emu input: %u checks passed\n", s_checks);
    return 0;
}
