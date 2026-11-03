/* SDL is only an input transport. Orb's real LVGL pointer recogniser owns
 * swipes, rim arbitration, drags and long presses (interaction-model.md). */
#include "emu_input.h"

#include <math.h>
#include <stdlib.h>

#include "orb_buttons.h"
#include "sim_board.h"
#include "sim_display.h"

struct emu_input {
    lv_indev_t *pointer;
    lv_indev_data_t touch;
    emu_input_cb_t callback;
    void *ctx;
    int viewport_x, viewport_y, viewport_w, viewport_h;
    bool pwr_down, boot_down;
    bool left, right, up, down, alt_left, alt_right;
    bool face_down, have_step;
    uint32_t last_step;
    float right_deg, down_deg, mouse_right, mouse_down;
};

static void emit(emu_input_t *input, emu_input_action_t action, int value)
{
    if (input->callback) input->callback(action, value, input->ctx);
}

static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    emu_input_t *input = lv_indev_get_user_data(indev);
    *data = input->touch;
}

static bool option(const emu_input_t *input)
{
    return input->alt_left || input->alt_right;
}

static void release_touch(emu_input_t *input)
{
    if (input->touch.state == LV_INDEV_STATE_RELEASED) return;
    input->touch.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(input->pointer);
}

static void cancel_touch(emu_input_t *input)
{
    if (input->touch.state == LV_INDEV_STATE_RELEASED) return;
    input->touch.state = LV_INDEV_STATE_RELEASED;
    /* A focus change or a switch to tilt is a cancellation, never a click on
     * whatever happened to be beneath the mouse when it left the window. */
    lv_indev_reset(input->pointer, NULL);
    lv_indev_read(input->pointer);
}

emu_input_t *emu_input_create(lv_display_t *display, emu_input_cb_t cb, void *ctx)
{
    emu_input_t *input = calloc(1, sizeof(*input));
    if (!input) return NULL;
    input->pointer = lv_indev_create();
    if (!input->pointer) { free(input); return NULL; }
    input->callback = cb;
    input->ctx = ctx;
    input->viewport_w = input->viewport_h = SIM_D;
    input->touch.state = LV_INDEV_STATE_RELEASED;
    lv_indev_set_type(input->pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input->pointer, display);
    lv_indev_set_user_data(input->pointer, input);
    lv_indev_set_read_cb(input->pointer, read_pointer);
    /* Every SDL edge must reach LVGL, even if down and up share one poll.
     * step() still samples held touches so LVGL long-press timers advance. */
    lv_indev_set_mode(input->pointer, LV_INDEV_MODE_EVENT);
    return input;
}

void emu_input_destroy(emu_input_t *input)
{
    if (!input) return;
    cancel_touch(input);
    lv_indev_delete(input->pointer);
    free(input);
}

void emu_input_viewport(emu_input_t *input, int x, int y, int width, int height)
{
    input->viewport_x = x;
    input->viewport_y = y;
    input->viewport_w = width > 0 ? width : 1;
    input->viewport_h = height > 0 ? height : 1;
}

static void position(const emu_input_t *input, int x, int y, float *px, float *py)
{
    *px = (float)(x - input->viewport_x) * SIM_D / input->viewport_w;
    *py = (float)(y - input->viewport_y) * SIM_D / input->viewport_h;
}

static void touch_position(emu_input_t *input, float x, float y)
{
    /* Keep a finger that leaves the glass on its circular edge. Clamping only
     * the square would turn a rim drag into a touch outside the physical panel. */
    float dx = x - SIM_D / 2.0f, dy = y - SIM_D / 2.0f;
    float length = hypotf(dx, dy), radius = SIM_D / 2.0f - 1;
    if (length > radius) {
        x = SIM_D / 2.0f + dx * radius / length;
        y = SIM_D / 2.0f + dy * radius / length;
    }
    input->touch.point = (lv_point_t){(int32_t)lroundf(x), (int32_t)lroundf(y)};
}

static float clamp(float value, float limit)
{
    return fmaxf(-limit, fminf(limit, value));
}

static void mouse_tilt(emu_input_t *input, float x, float y)
{
    input->mouse_right = clamp((x - SIM_D / 2.0f) * 0.15f, 30);
    input->mouse_down = clamp((y - SIM_D / 2.0f) * 0.15f, 30);
}

static void clear_held(emu_input_t *input)
{
    cancel_touch(input);
    /* Release raw keys on focus loss so the shared recognizer cannot leave
     * a button held after SDL stops delivering keyboard events. */
    input->pwr_down = input->boot_down = false;
    input->left = input->right = input->up = input->down = false;
    input->alt_left = input->alt_right = false;
    input->mouse_right = input->mouse_down = 0;
}

void emu_input_step(emu_input_t *input, uint32_t now_ms)
{
    uint32_t elapsed = input->have_step ? now_ms - input->last_step : 0;
    input->have_step = true;
    input->last_step = now_ms;
    float target_right = 22.0f * ((int)input->right - (int)input->left);
    float target_down = 22.0f * ((int)input->down - (int)input->up);
    if (option(input)) {
        target_right += input->mouse_right;
        target_down += input->mouse_down;
    }
    /* HIG §6: ease sensor pose changes; motion never navigates or confirms. */
    float blend = 1.0f - expf(-(float)elapsed / 150.0f);
    input->right_deg += (clamp(target_right, 40) - input->right_deg) * blend;
    input->down_deg += (clamp(target_down, 40) - input->down_deg) * blend;
    /* Screen replacement asks LVGL to wait for a release. Sample released
     * state too, so the next click after a keyboard game exit is not lost. */
    lv_indev_read(input->pointer);
}

static void key_down(emu_input_t *input, SDL_Keycode key)
{
    switch (key) {
    case SDLK_p: input->pwr_down = true; break;
    case SDLK_b: input->boot_down = true; break;
    case SDLK_LEFT: input->left = true; break;
    case SDLK_RIGHT: input->right = true; break;
    case SDLK_UP: input->up = true; break;
    case SDLK_DOWN: input->down = true; break;
    case SDLK_LALT: input->alt_left = true; cancel_touch(input); break;
    case SDLK_RALT: input->alt_right = true; cancel_touch(input); break;
    case SDLK_f:
        input->face_down = !input->face_down;
        emit(input, EMU_INPUT_FACE_DOWN, input->face_down);
        break;
    case SDLK_d: emit(input, EMU_INPUT_DIAGNOSTICS, 0); break;
    case SDLK_s: emit(input, EMU_INPUT_SCREENSHOT, 0); break;
    case SDLK_q:
    case SDLK_ESCAPE: emit(input, EMU_INPUT_QUIT, 0); break;
    default:
        if (key >= SDLK_1 && key <= SDLK_6)
            emit(input, EMU_INPUT_NAVIGATE, (int)(key - SDLK_1 + 1));
        break;
    }
}

static void key_up(emu_input_t *input, SDL_Keycode key)
{
    switch (key) {
    case SDLK_p: input->pwr_down = false; break;
    case SDLK_b: input->boot_down = false; break;
    case SDLK_LEFT: input->left = false; break;
    case SDLK_RIGHT: input->right = false; break;
    case SDLK_UP: input->up = false; break;
    case SDLK_DOWN: input->down = false; break;
    case SDLK_LALT: input->alt_left = false; break;
    case SDLK_RALT: input->alt_right = false; break;
    default: break;
    }
    if (!option(input)) input->mouse_right = input->mouse_down = 0;
}

void emu_input_event(emu_input_t *input, const SDL_Event *event, uint32_t now_ms)
{
    emu_input_step(input, now_ms);
    switch (event->type) {
    case SDL_QUIT: emit(input, EMU_INPUT_QUIT, 0); break;
    case SDL_KEYDOWN:
        if (!event->key.repeat) key_down(input, event->key.keysym.sym);
        break;
    case SDL_KEYUP: key_up(input, event->key.keysym.sym); break;
    case SDL_MOUSEBUTTONDOWN:
        if (event->button.button == SDL_BUTTON_LEFT) {
            float x, y;
            position(input, event->button.x, event->button.y, &x, &y);
            if (option(input)) mouse_tilt(input, x, y);
            else if (x >= 0 && x < SIM_D && y >= 0 && y < SIM_D &&
                     sim_board_cover((int)x, (int)y, SIM_D)) {
                touch_position(input, x, y);
                input->touch.state = LV_INDEV_STATE_PRESSED;
                lv_indev_read(input->pointer);
            }
        }
        break;
    case SDL_MOUSEMOTION: {
        float x, y;
        position(input, event->motion.x, event->motion.y, &x, &y);
        if (option(input)) mouse_tilt(input, x, y);
        else if (input->touch.state == LV_INDEV_STATE_PRESSED) {
            touch_position(input, x, y);
            lv_indev_read(input->pointer);
        }
        break;
    }
    case SDL_MOUSEBUTTONUP:
        if (event->button.button == SDL_BUTTON_LEFT) {
            if (input->touch.state == LV_INDEV_STATE_PRESSED) {
                float x, y;
                position(input, event->button.x, event->button.y, &x, &y);
                touch_position(input, x, y);
            }
            release_touch(input);
        }
        break;
    case SDL_WINDOWEVENT:
        if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
            event->window.event == SDL_WINDOWEVENT_HIDDEN) clear_held(input);
        break;
    default: break;
    }
}

unsigned emu_input_buttons(const emu_input_t *input)
{
    return (input->pwr_down ? 1u << ORB_BUTTON_PWR : 0) |
           (input->boot_down ? 1u << ORB_BUTTON_BOOT : 0);
}
bool emu_input_face_down(const emu_input_t *input) { return input->face_down; }

void emu_input_gravity(const emu_input_t *input, float gravity[3])
{
    sim_board_gravity(input->right_deg, input->down_deg, gravity);
    if (input->face_down) gravity[2] = -gravity[2];
}
