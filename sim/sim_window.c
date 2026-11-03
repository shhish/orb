/*
 * SDL2 window for the simulator. The panel is shown pixel for pixel (doubled
 * on a Retina screen, never smoothed), black outside the circle.
 *
 * Input, as the device would get it:
 *  - mouse: the finger (touches start only inside the circle, as on the glass);
 *  - arrow keys: dip that edge of the device 5 degrees further, 0 levels it;
 *  - Shift- or Option-drag: dips the edges as far as the mouse moves (a quarter
 *    degree a point) on top of that, until the button is let go;
 *  - m: Reduce Motion on or off; s: a screenshot; q or Esc: quit.
 */
#include "sim_window.h"

#include <stdio.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "orb_ui.h"
#include "sim_board.h"
#include "sim_display.h"

#define KEY_STEP_DEG 5.0f
#define DRAG_DEG_PER_PT 0.25f
#define MAX_TILT_DEG 45.0f

static SDL_Window *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture *s_tex;
static const char *s_out_dir;
static int s_shots;

static struct {
    int32_t x, y;
    bool pressed;
} s_touch;

static struct {
    float key_right, key_down;    /* from the arrow keys, kept */
    float drag_right, drag_down;  /* from a modifier-drag, while it lasts */
    bool dragging;
    int x0, y0;
} s_tilt;

static float clamp_tilt(float deg)
{
    return deg < -MAX_TILT_DEG ? -MAX_TILT_DEG : deg > MAX_TILT_DEG ? MAX_TILT_DEG : deg;
}

void sim_window_tilt(float *right_deg, float *down_deg)
{
    *right_deg = clamp_tilt(s_tilt.key_right + s_tilt.drag_right);
    *down_deg = clamp_tilt(s_tilt.key_down + s_tilt.drag_down);
}

/* The state in the title bar, so a tilt or Reduce Motion is never a guess. */
static void update_title(void)
{
    float right, down;
    sim_window_tilt(&right, &down);
    char title[128];
    snprintf(title, sizeof(title), "Orb  -  tilt %+.0f right, %+.0f down%s", right, down,
             orb_ui_reduce_motion() ? "  -  Reduce Motion" : "");
    SDL_SetWindowTitle(s_win, title);
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->point.x = s_touch.x;
    data->point.y = s_touch.y;
    data->state = s_touch.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

bool sim_window_open(lv_display_t *disp, const char *out_dir)
{
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "orb_sim: SDL: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    s_win = SDL_CreateWindow("Orb", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SIM_D, SIM_D,
                             SDL_WINDOW_ALLOW_HIGHDPI);
    s_ren = s_win ? SDL_CreateRenderer(s_win, -1, 0) : NULL;
    s_tex = s_ren ? SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, SIM_D, SIM_D) : NULL;
    if (!s_tex) {
        fprintf(stderr, "orb_sim: SDL: %s\n", SDL_GetError());
        sim_window_close();
        return false;
    }
    s_out_dir = out_dir;
    update_title();

    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touch_read);
    lv_indev_set_display(touch, disp);
    return true;
}

static void screenshot(void)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/orb-sim-%02d.png", s_out_dir, ++s_shots);
    if (sim_display_save_png(path)) {
        printf("orb_sim: wrote %s\n", path);
    }
}

static bool on_key(SDL_Keycode key)
{
    switch (key) {
    case SDLK_RIGHT:
        s_tilt.key_right = clamp_tilt(s_tilt.key_right + KEY_STEP_DEG);
        break;
    case SDLK_LEFT:
        s_tilt.key_right = clamp_tilt(s_tilt.key_right - KEY_STEP_DEG);
        break;
    case SDLK_DOWN:
        s_tilt.key_down = clamp_tilt(s_tilt.key_down + KEY_STEP_DEG);
        break;
    case SDLK_UP:
        s_tilt.key_down = clamp_tilt(s_tilt.key_down - KEY_STEP_DEG);
        break;
    case SDLK_0:
        s_tilt.key_right = s_tilt.key_down = 0;
        break;
    case SDLK_m:
        lv_lock();
        orb_ui_set_reduce_motion(!orb_ui_reduce_motion());
        lv_unlock();
        break;
    case SDLK_s:
        screenshot();
        break;
    case SDLK_q:
    case SDLK_ESCAPE:
        return false;
    default:
        return true;
    }
    update_title();
    return true;
}

static void on_press(const SDL_MouseButtonEvent *b)
{
    if (b->button != SDL_BUTTON_LEFT) {
        return;
    }
    if (SDL_GetModState() & (KMOD_SHIFT | KMOD_ALT)) {
        s_tilt.dragging = true;
        s_tilt.x0 = b->x;
        s_tilt.y0 = b->y;
    } else if (sim_board_cover(b->x, b->y, SIM_D) > 0) {
        s_touch.x = b->x;
        s_touch.y = b->y;
        s_touch.pressed = true;
    }
}

static void on_motion(const SDL_MouseMotionEvent *m)
{
    if (s_tilt.dragging) {
        s_tilt.drag_right = (m->x - s_tilt.x0) * DRAG_DEG_PER_PT;
        s_tilt.drag_down = (m->y - s_tilt.y0) * DRAG_DEG_PER_PT;
        update_title();
    } else if (s_touch.pressed) {
        /* A finger dragged off the glass stays at its edge. */
        s_touch.x = m->x < 0 ? 0 : m->x >= SIM_D ? SIM_D - 1 : m->x;
        s_touch.y = m->y < 0 ? 0 : m->y >= SIM_D ? SIM_D - 1 : m->y;
    }
}

static void on_release(const SDL_MouseButtonEvent *b)
{
    if (b->button != SDL_BUTTON_LEFT) {
        return;
    }
    s_touch.pressed = false;
    if (s_tilt.dragging) {
        s_tilt.dragging = false;
        s_tilt.drag_right = s_tilt.drag_down = 0;
        update_title();
    }
}

static bool handle(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_QUIT:
        return false;
    case SDL_KEYDOWN:
        return on_key(e->key.keysym.sym);
    case SDL_MOUSEBUTTONDOWN:
        on_press(&e->button);
        break;
    case SDL_MOUSEMOTION:
        on_motion(&e->motion);
        break;
    case SDL_MOUSEBUTTONUP:
        on_release(&e->button);
        break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_EXPOSED) {
            sim_window_present();
        }
        break;
    default:
        break;
    }
    return true;
}

bool sim_window_poll(uint32_t ms)
{
    SDL_Event e;
    if (!SDL_WaitEventTimeout(&e, (int)ms)) {
        return true;
    }
    do {
        if (!handle(&e)) {
            return false;
        }
    } while (SDL_PollEvent(&e));
    return true;
}

void sim_window_present(void)
{
    static uint8_t rgb[SIM_D * SIM_D * 3];
    sim_display_rgb(rgb);
    SDL_UpdateTexture(s_tex, NULL, rgb, SIM_D * 3);
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    SDL_RenderPresent(s_ren);
}

void sim_window_close(void)
{
    if (s_tex) {
        SDL_DestroyTexture(s_tex);
    }
    if (s_ren) {
        SDL_DestroyRenderer(s_ren);
    }
    if (s_win) {
        SDL_DestroyWindow(s_win);
    }
    s_tex = NULL;
    s_ren = NULL;
    s_win = NULL;
    SDL_Quit();
}
