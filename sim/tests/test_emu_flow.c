/* Cross-module desktop input checks: SDL events enter the real shell, app
 * registry and controllers. All model data/storage/audio are fixture-local. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu_fixture.h"
#include "emu_input.h"
#include "orb_apps.h"
#include "orb_fonts.h"
#include "orb_home.h"
#include "orb_model.h"
#include "orb_shell_services.h"
#include "orb_time.h"
#include "orb_ui.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks;
static const char *s_case = "startup";
static lv_display_t *s_display;
static emu_input_t *s_input;
static orb_shell_t *s_shell;
static bool s_screen_on = true;

#define CHECK(value) do { \
    ++s_checks; \
    if (!(value)) { \
        fprintf(stderr, "emu flow %s at %u ms, %s:%d: %s\n", \
            s_case, s_now, __FILE__, __LINE__, #value); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static uint32_t tick(void) { return s_now; }

static void action(emu_input_action_t kind, int value, void *ctx)
{
    (void)ctx;
    emu_fixture_input(kind, value);
}

static void advance(unsigned ms)
{
    while (ms) {
        unsigned step = LV_MIN(ms, 16);
        s_now += step;
        emu_input_step(s_input, s_now);
        float gravity[3];
        emu_input_gravity(s_input, gravity);
        orb_ui_set_gravity(gravity[0], gravity[1], gravity[2], step / 1000.0f);
        emu_fixture_step(s_now, emu_input_buttons(s_input), s_screen_on,
                         gravity[0], gravity[1], gravity[2]);
        lv_timer_handler();
        lv_refr_now(s_display);
        ms -= step;
    }
}

static void key(SDL_Keycode code, bool down)
{
    SDL_Event event = {.key = {.type = down ? SDL_KEYDOWN : SDL_KEYUP,
        .keysym = {.sym = code}}};
    emu_input_event(s_input, &event, s_now);
    advance(16);
}

static void mouse(uint32_t kind, int x, int y)
{
    SDL_Event event = {.button = {.type = kind, .button = SDL_BUTTON_LEFT, .x = x, .y = y}};
    if (kind == SDL_MOUSEMOTION)
        event.motion = (SDL_MouseMotionEvent){.type = kind, .x = x, .y = y};
    emu_input_event(s_input, &event, s_now);
    advance(16);
}

static void press_key(SDL_Keycode code)
{
    key(code, true);
    advance(64); /* Physical presses must survive the production debounce. */
    key(code, false);
    advance(64);
}

static void place(unsigned number)
{
    press_key(SDLK_1 + (SDL_Keycode)number - 1);
    advance(900);
}

static lv_obj_t *control(lv_obj_t *root, const char *caption)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (lv_obj_check_type(root, &lv_label_class)) {
        const char *text = lv_label_get_text(root);
        size_t n = strlen(text), v = strlen(caption);
        if (!strcmp(text, caption) || (n > v && text[n - v - 1] == ' ' && !strcmp(text + n - v, caption))) {
            if (lv_obj_is_clickable(root)) return root;
            for (lv_obj_t *parent = lv_obj_get_parent(root); parent; parent = lv_obj_get_parent(parent))
                if (lv_obj_is_clickable(parent)) return parent;
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = control(lv_obj_get_child(root, (int32_t)i), caption);
        if (found) return found;
    }
    if (root == lv_screen_active())
        return control(lv_display_get_layer_top(s_display), caption);
    return NULL;
}

static void shot(const char *name)
{
    const char *directory = getenv("ORB_EMU_FLOW_SHOT_DIR");
    if (!directory) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.png", directory, name);
    CHECK(sim_display_save_png(path));
}

static void tap(const char *caption)
{
    lv_obj_t *target = control(lv_screen_active(), caption);
    CHECK(target);
    lv_area_t box;
    lv_obj_get_coords(target, &box);
    int x = (box.x1 + box.x2) / 2, y = (box.y1 + box.y2) / 2;
    CHECK(x >= 0 && x < SIM_D && y >= 0 && y < SIM_D);
    mouse(SDL_MOUSEBUTTONDOWN, x, y);
    mouse(SDL_MOUSEBUTTONUP, x, y);
    advance(900);
}

static orb_time_snapshot_t time_state(void)
{
    orb_time_snapshot_t state;
    orb_time_snapshot(orb_time_now_us(), &state);
    return state;
}

static void design_shots(void)
{
    /* One accessibility variant per process keeps screenshot runs independent
     * of the long interaction regression and its font/raster cache history. */
    const char *variant = getenv("ORB_EMU_FLOW_SHOT_VARIANT");
    if (!variant) variant = "default";
    bool ax3 = !strcmp(variant, "ax3-bold") || !strcmp(variant, "ax3-bold-rt");
    CHECK(ax3 || !strcmp(variant, "default"));
    CHECK(orb_fonts_set_content_size(ax3 ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(ax3) == ORB_FONTS_OK);
    orb_ui_set_reduce_transparency(!strcmp(variant, "ax3-bold-rt"));
    char name[80];
    place(1);
    snprintf(name, sizeof(name), "face-%s", variant); shot(name);
    place(2);
    snprintf(name, sizeof(name), "agents-%s", variant); shot(name);
    orb_shell_select_card(s_shell, "approval-build");
    advance(900);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CARD);
    snprintf(name, sizeof(name), "card-%s", variant); shot(name);
    place(1);
    press_key(SDLK_p); advance(60); press_key(SDLK_p); advance(900);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CONTROL_CENTER);
    snprintf(name, sizeof(name), "control-center-%s", variant); shot(name);
    key(SDLK_p, true); advance(2200); key(SDLK_p, false); advance(900);
    CHECK(control(lv_screen_active(), "Sleep display"));
    snprintf(name, sizeof(name), "power-sheet-%s", variant); shot(name);
    press_key(SDLK_b); advance(900);
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    s_display = sim_display_create();
    CHECK(s_display && emu_fixture_init(s_display));
    s_shell = emu_fixture_shell();
    s_input = emu_input_create(s_display, action, NULL);
    CHECK(s_input);
    emu_input_viewport(s_input, 0, 0, SIM_D, SIM_D);
    advance(900);
    CHECK(orb_apps_count() == 6 && orb_apps_find("orbit") && orb_apps_find("beads") && orb_apps_find("time"));
    orb_model_snapshot_t model;
    CHECK(orb_model_snapshot(&model) && model.session_count == 5 && model.card_count == 4);
    if (getenv("ORB_EMU_FLOW_SHOT_DIR")) {
        s_case = "design shots";
        design_shots();
        goto cleanup;
    }

    s_case = "numbered places";
    static const orb_shell_surface_t places[] = {
        ORB_SHELL_FACE, ORB_SHELL_AGENTS, ORB_SHELL_HOME,
        ORB_SHELL_APPS, ORB_SHELL_STACK, ORB_SHELL_NOTIFICATIONS,
    };
    for (unsigned i = 0; i < 6; ++i) {
        place(i + 1);
        CHECK(orb_shell_surface(s_shell) == places[i]);
    }

    s_case = "mouse swipe follows real nav";
    place(1);
    mouse(SDL_MOUSEBUTTONDOWN, 400, 233);
    mouse(SDL_MOUSEMOTION, 300, 233);
    mouse(SDL_MOUSEMOTION, 150, 233);
    CHECK(sim_display_snapshot_transition_active());
    mouse(SDL_MOUSEBUTTONUP, 150, 233);
    advance(1000);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_HOME);

    s_case = "P single locks and wakes through the board recognizer";
    press_key(SDLK_p);
    advance(900);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_HOME &&
          orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    press_key(SDLK_p);
    advance(900);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_HOME &&
          orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
    press_key(SDLK_b); advance(900);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    press_key(SDLK_b); advance(900);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_APPS);
    s_case = "P double opens Control Center";
    advance(800);
    press_key(SDLK_p);
    advance(60);
    press_key(SDLK_p);
    advance(900);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CONTROL_CENTER);
    shot("control-center");

    s_case = "BOOT short is Back";
    press_key(SDLK_b);
    advance(900);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_APPS);

    s_case = "P hold opens power only at two seconds";
    place(1);
    key(SDLK_p, true);
    advance(1900);
    CHECK(!control(lv_screen_active(), "Sleep display"));
    advance(200);
    CHECK(control(lv_screen_active(), "Sleep display") && control(lv_screen_active(), "Restart"));
    key(SDLK_p, false);
    advance(900);
    CHECK(control(lv_screen_active(), "Sleep display"));
    shot("power-sheet");
    press_key(SDLK_b);
    advance(900);
    CHECK(!control(lv_screen_active(), "Sleep display"));
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);

    s_case = "P wake consumes the complete physical hold";
    s_screen_on = false;
    advance(800); /* Let the shared sleep transition reach its panel-off endpoint. */
    CHECK(orb_shell_brightness(s_shell) == 0);
    key(SDLK_p, true);
    advance(64);
    CHECK(orb_shell_brightness(s_shell) > 10);
    advance(2100);
    CHECK(orb_shell_brightness(s_shell) > 10);
    CHECK(!control(lv_screen_active(), "Sleep display"));
    key(SDLK_p, false);
    advance(900);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE && orb_shell_brightness(s_shell) > 10);
    /* Wake is latched even while the external input still reports off. Only
     * the waking press is consumed; the next complete PWR press locks. */
    press_key(SDLK_p);
    advance(900);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE &&
          orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    press_key(SDLK_p); advance(900);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
    s_screen_on = true;
    advance(64);

    s_case = "BOOT production hold and no-speech receipt";
    press_key(SDLK_b); /* Short BOOT never starts capture. */
    advance(400);
    orb_voice_state_t voice;
    CHECK(orb_model_get_voice(&voice) && voice.phase == ORB_VOICE_IDLE);
    key(SDLK_p, true);
    advance(2200);
    key(SDLK_p, false);
    advance(900);
    tap("Sleep display");
    CHECK(orb_shell_brightness(s_shell) == 0);
    CHECK(!control(lv_screen_active(), "Sleep display"));
    key(SDLK_b, true);
    advance(64);
    CHECK(orb_shell_brightness(s_shell) > 10);
    CHECK(orb_model_get_voice(&voice) && voice.phase == ORB_VOICE_IDLE);
    advance(586); /* BOOT wakes and still talks; only PWR is consumed. */
    CHECK(orb_model_get_voice(&voice) && voice.phase == ORB_VOICE_RECORDING);
    key(SDLK_b, false);
    advance(800);
    CHECK(orb_model_get_voice(&voice) && voice.phase == ORB_VOICE_ERROR &&
          !strcmp(voice.error_code, "no_speech"));

    s_case = "same-poll BOOT tap is filtered by physical debounce";
    SDL_Event boot = {.key = {.type = SDL_KEYDOWN, .keysym = {.sym = SDLK_b}}};
    emu_input_event(s_input, &boot, s_now);
    boot.key.type = SDL_KEYUP;
    emu_input_event(s_input, &boot, s_now);
    advance(100);
    CHECK(orb_model_get_voice(&voice) && voice.phase == ORB_VOICE_ERROR &&
          !strcmp(voice.error_code, "no_speech"));

    s_case = "Home touch changes only fixture state";
    place(3);
    orb_home_snapshot_t home;
    CHECK(orb_home_snapshot(&home));
    const orb_home_accessory_t *lamp = orb_home_find(&home, "floor-lamp");
    CHECK(lamp && home.demo);
    bool was_on = lamp->state.on;
    CHECK(control(lv_screen_active(), "Living Room") && !control(lv_screen_active(), "Floor Lamp"));
    tap("Living Room");
    tap("Floor Lamp");
    CHECK(orb_home_snapshot(&home));
    lamp = orb_home_find(&home, "floor-lamp");
    CHECK(lamp && lamp->state.on == was_on && !lamp->pending);
    /* The template's identity disc at (233,140) is the light's power. */
    mouse(SDL_MOUSEBUTTONDOWN, 233, 140); mouse(SDL_MOUSEBUTTONUP, 233, 140); advance(900);
    CHECK(orb_home_snapshot(&home));
    lamp = orb_home_find(&home, "floor-lamp");
    CHECK(lamp && lamp->state.on != was_on && !lamp->pending);

    s_case = "Time touch and rim owner";
    place(4);
    tap(ORB_ICON_TIMER);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_TIME && orb_apps_active() &&
          !strcmp(orb_apps_active()->id, "time"));
    CHECK(orb_time_timer_set(120));
    advance(260);
    int64_t before = time_state().timer_duration_us;
    mouse(SDL_MOUSEBUTTONDOWN, 449, 233);
    mouse(SDL_MOUSEMOTION, 449, 250);
    mouse(SDL_MOUSEMOTION, 386, 386);
    advance(160);
    mouse(SDL_MOUSEMOTION, 386, 386);
    mouse(SDL_MOUSEBUTTONUP, 386, 386);
    advance(300);
    CHECK(time_state().timer_duration_us > before);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_TIME);
    tap("Start");
    CHECK(time_state().timer_state == ORB_TIME_RUNNING);
    tap("Pause");
    CHECK(time_state().timer_state == ORB_TIME_PAUSED);
    /* Optional review artifact from the real SDL-to-shell Time flow, rather
     * than another synthetic view. Ordinary CTest leaves the filesystem alone. */
    const char *time_shot = getenv("ORB_EMU_TIME_SHOT");
    if (time_shot && time_shot[0]) CHECK(sim_display_save_png(time_shot));

    s_case = "game face-down pause/wake and shared PWR power sheet";
    place(1);
    orb_shell_navigate(s_shell, ORB_SHELL_PLAY);
    advance(900);
    tap("Play");
    CHECK(orb_apps_active() && !strcmp(orb_apps_active()->id, "marble"));
    orb_marble_snapshot_t proof;
    CHECK(!orb_marble_snapshot(&proof)); /* The old proof controller is absent. */
    CHECK(!control(lv_screen_active(), "Tap to Play"));
    CHECK(!control(lv_screen_active(), "Play"));
    CHECK(!control(lv_screen_active(), "Resume"));
    CHECK(!control(lv_screen_active(), "Garden"));
    key(SDLK_RIGHT, true);
    advance(400);
    float gravity[3];
    emu_input_gravity(s_input, gravity);
    CHECK(gravity[0] < -.3f); /* Production gravity reaches the running game. */
    key(SDLK_RIGHT, false);
    press_key(SDLK_f);
    advance(100);
    CHECK(emu_input_face_down(s_input) && orb_apps_paused());
    press_key(SDLK_f);
    advance(100);
    CHECK(!emu_input_face_down(s_input) && !orb_apps_paused());
    key(SDLK_p, true);
    advance(1264);
    CHECK(orb_apps_active() && !control(lv_screen_active(), "Sleep display"));
    advance(900);
    CHECK(orb_apps_active() && orb_apps_paused());
    CHECK(control(lv_screen_active(), "Sleep display"));
    key(SDLK_p, false);
    advance(900);
    CHECK(orb_apps_active() && control(lv_screen_active(), "Restart"));
    press_key(SDLK_b);
    advance(900);
    CHECK(orb_apps_active() && !orb_apps_paused());
    CHECK(!control(lv_screen_active(), "Restart"));
    press_key(SDLK_b);
    advance(900);
    CHECK(!orb_apps_active());
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_PLAY);

    s_case = "numbered debug shortcuts exit a full-screen game before routing";
    lv_obj_t *shell_screen = lv_screen_active();
    for (unsigned i = 0; i < 6; ++i) {
        if (orb_shell_surface(s_shell) != ORB_SHELL_PLAY)
            orb_shell_navigate(s_shell, ORB_SHELL_PLAY);
        advance(900);
        tap("Play");
        CHECK(orb_apps_active() && lv_screen_active() != shell_screen);
        CHECK(!control(lv_screen_active(), "Play"));
        CHECK(!control(lv_screen_active(), "Tap to Play"));
        CHECK(!control(lv_screen_active(), "Resume"));
        place(i + 1);
        CHECK(!orb_apps_active() && lv_screen_active() == shell_screen);
        CHECK(orb_shell_surface(s_shell) == places[i]);
    }
    orb_shell_navigate(s_shell, ORB_SHELL_PLAY);
    advance(900);
    tap("Play");
    CHECK(!control(lv_screen_active(), "Play"));
    CHECK(!control(lv_screen_active(), "Resume"));
    s_case = "P short pauses and locks the game; wake restores it before Back";
    press_key(SDLK_p);
    advance(1300);
    CHECK(orb_apps_active() && lv_screen_active() != shell_screen);
    CHECK(orb_apps_paused() && orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    press_key(SDLK_p); advance(900);
    CHECK(orb_apps_active() &&
          orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
    press_key(SDLK_b);
    advance(900);
    CHECK(!orb_apps_active() && lv_screen_active() == shell_screen);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_PLAY);

    s_case = "P double overlays Control Center without exiting the game";
    orb_shell_navigate(s_shell, ORB_SHELL_PLAY);
    advance(900);
    tap("Play");
    press_key(SDLK_p);
    advance(60);
    press_key(SDLK_p);
    advance(1000);
    CHECK(orb_apps_active() && lv_screen_active() != shell_screen && orb_apps_paused());
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CONTROL_CENTER);
    press_key(SDLK_b);
    advance(900);
    CHECK(orb_apps_active() && !orb_apps_paused());
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_PLAY);
    press_key(SDLK_b);
    advance(900);
    CHECK(!orb_apps_active() && lv_screen_active() == shell_screen);

    /* Time stays alive beneath the shared lock, including its selected page. */
    place(4);
    tap(ORB_ICON_TIMER);
    CHECK(orb_apps_active() && !strcmp(orb_apps_active()->id, "time"));
    press_key(SDLK_p);
    advance(1300);
    CHECK(orb_apps_active() && !strcmp(orb_apps_active()->id, "time") &&
          orb_shell_surface(s_shell) == ORB_SHELL_TIME &&
          orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    press_key(SDLK_p); advance(900);
    CHECK(orb_apps_active() && orb_shell_surface(s_shell) == ORB_SHELL_TIME &&
          orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);

cleanup:
    emu_input_destroy(s_input);
    emu_fixture_shutdown();
    printf("emu flow: %u checks passed; SDL pointer/keys, nav, Home, Time, real games and voice\n", s_checks);
    return 0;
}
