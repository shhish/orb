/* Desktop frontend only: all product surfaces come from orb_shell and the
 * firmware app registry. SDL owns presentation/input; LVGL owns UI time. */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "emu_fixture.h"
#include "emu_input.h"
#include "orb_apps.h"
#include "orb_fonts.h"
#include "orb_shell_services.h"
#include "orb_ui.h"
#include "sim_board.h"
#include "sim_display.h"
#include "src/libs/lodepng/lodepng.h"

#ifndef ORB_EMU_SHOT_DIR
#define ORB_EMU_SHOT_DIR "sim/build/emu-shots"
#endif

static SDL_Window *s_window;
static SDL_Renderer *s_renderer;
static SDL_Texture *s_texture;
static emu_input_t *s_input;
static lv_obj_t *s_diagnostics;
static bool s_running = true, s_shot, s_failed, s_headless;
static uint32_t s_now, s_epoch, s_shots;
static const char *s_output = ORB_EMU_SHOT_DIR;
static struct {
    uint64_t pixels, render_start;
    double render_ms, frame_ms, fps;
    uint32_t interval, frames, rendered;
    double dirty_pct;
} s_metrics;

static uint32_t tick(void) { return s_headless ? s_now : SDL_GetTicks() - s_epoch; }
static double elapsed_ms(uint64_t start)
{
    return (SDL_GetPerformanceCounter() - start) * 1000.0 / SDL_GetPerformanceFrequency();
}

static bool make_directory(const char *directory)
{
    char path[PATH_MAX];
    if (!directory[0] || strlen(directory) >= sizeof(path)) return false;
    snprintf(path, sizeof(path), "%s", directory);
    for (char *p = path + 1; ; ++p) {
        if (*p && *p != '/') continue;
        char saved = *p;
        *p = 0;
        if (mkdir(path, 0755) != 0 && errno != EEXIST) return false;
        struct stat info;
        if (stat(path, &info) || !S_ISDIR(info.st_mode)) return false;
        *p = saved;
        if (!saved) return true;
    }
}

/* Capture the SDL presentation, including the dark-grey surround and optional
 * diagnostics. This also proves the dummy driver follows the visible path. */
static bool screenshot(const char *path)
{
    int width, height;
    if (SDL_GetRendererOutputSize(s_renderer, &width, &height) || width <= 0 || height <= 0) return false;
    size_t bytes = (size_t)width * height * 3;
    uint8_t *rgb = malloc(bytes);
    if (!rgb) return false;
    bool ok = SDL_RenderReadPixels(s_renderer, NULL, SDL_PIXELFORMAT_RGB24, rgb, width * 3) == 0;
    if (ok) {
        /* RGB8 keeps the same decoder behaviour as existing sim captures. */
        LodePNGState state;
        lodepng_state_init(&state);
        state.encoder.auto_convert = 0;
        state.info_raw.colortype = state.info_png.color.colortype = LCT_RGB;
        state.info_raw.bitdepth = state.info_png.color.bitdepth = 8;
        unsigned char *png = NULL;
        size_t size = 0;
        unsigned error = lodepng_encode(&png, &size, rgb, (unsigned)width, (unsigned)height, &state);
        lodepng_state_cleanup(&state);
        FILE *file = error ? NULL : fopen(path, "wb");
        ok = file && fwrite(png, 1, size, file) == size;
        if (file && fclose(file)) ok = false;
        lv_free(png);
    }
    free(rgb);
    if (!ok) fprintf(stderr, "orb_emu: screenshot failed: %s\n", path);
    return ok;
}

static void display_event(lv_event_t *event)
{
    switch (lv_event_get_code(event)) {
    case LV_EVENT_RENDER_START:
        s_metrics.render_start = SDL_GetPerformanceCounter();
        break;
    case LV_EVENT_RENDER_READY:
        s_metrics.render_ms = elapsed_ms(s_metrics.render_start);
        ++s_metrics.rendered;
        break;
    case LV_EVENT_FLUSH_START: {
        const lv_area_t *area = lv_event_get_param(event);
        s_metrics.pixels += (uint64_t)lv_area_get_width(area) * lv_area_get_height(area);
        break;
    }
    default: break;
    }
}

static void diagnostics_update(void)
{
    if (lv_obj_is_hidden(s_diagnostics)) return;
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    char text[320];
    snprintf(text, sizeof(text), "Mac only\nNot board speed\n%.1f fps / %.1f ms\nUI %.1f ms / dirty %.1f%%\nLVGL %zu KiB\nPeak %zu KiB",
        s_metrics.fps, s_metrics.frame_ms, s_metrics.render_ms, s_metrics.dirty_pct,
        memory.total_size / 1024, memory.max_used / 1024);
    lv_label_set_text(s_diagnostics, text);
}

static void input_action(emu_input_action_t action, int value, void *context)
{
    (void)context;
    switch (action) {
    case EMU_INPUT_DIAGNOSTICS:
        if (lv_obj_is_hidden(s_diagnostics)) {
            lv_obj_set_hidden(s_diagnostics, false);
            diagnostics_update();
        } else lv_obj_set_hidden(s_diagnostics, true);
        break;
    case EMU_INPUT_SCREENSHOT: s_shot = true; break;
    case EMU_INPUT_QUIT: s_running = false; break;
    default: emu_fixture_input(action, value); break;
    }
}

static void usage(void)
{
    puts("usage: orb_emu [--scale 1|2] [--headless-frames N --out DIR]\n"
         "  --scene face|agents|home|apps|stack|notifications|card|play|marble\n"
         "  --design-scene Screen (deterministic Design 1.0 frame fixture)\n"
         "  --text-size default|larger|ax3 --bold --reduce-transparency\n"
         "  --reduce-motion --diagnostics --hub (documented, unavailable stub)\n"
         "Mouse: touch / drag; Option+mouse: tilt; arrows: tilt; F: face down\n"
         "P: PWR (double: Control Center, hold 2 s: power sheet); B: Back / hold to talk\n"
         "1..6: places; D: diagnostics; S: PNG; Esc/Q: quit");
}

static bool number(const char *value, unsigned *result)
{
    char *end;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 10);
    if (!value[0] || value[0] == '-' || *end || errno || parsed > 100000) return false;
    *result = (unsigned)parsed;
    return true;
}

int main(int argc, char **argv)
{
    unsigned scale = 1, frames = 0;
    const char *scene = "face", *design_scene = NULL;
    orb_content_size_t text_size = ORB_CONTENT_SIZE_DEFAULT;
    bool bold = false, opaque = false, reduce_motion = false, diagnostics = false, hub = false;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i], *value = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(arg, "--help")) { usage(); return 0; }
        else if (!strcmp(arg, "--hub")) hub = true;
        else if (!strcmp(arg, "--bold")) bold = true;
        else if (!strcmp(arg, "--reduce-transparency")) opaque = true;
        else if (!strcmp(arg, "--reduce-motion")) reduce_motion = true;
        else if (!strcmp(arg, "--diagnostics")) diagnostics = true;
        else if (value && !strcmp(arg, "--scale")) {
            if (!number(value, &scale) || (scale != 1 && scale != 2)) goto invalid;
            ++i;
        } else if (value && !strcmp(arg, "--headless-frames")) {
            if (!number(value, &frames) || !frames) goto invalid;
            s_headless = true; ++i;
        } else if (value && !strcmp(arg, "--out")) { s_output = value; ++i; }
        else if (value && !strcmp(arg, "--scene")) { scene = value; ++i; }
        else if (value && !strcmp(arg, "--design-scene")) { design_scene = value; ++i; }
        else if (value && !strcmp(arg, "--text-size")) {
            if (!strcmp(value, "default")) text_size = ORB_CONTENT_SIZE_DEFAULT;
            else if (!strcmp(value, "larger")) text_size = ORB_CONTENT_SIZE_LARGER;
            else if (!strcmp(value, "ax3")) text_size = ORB_CONTENT_SIZE_AX3;
            else goto invalid;
            ++i;
        } else goto invalid;
        continue;
invalid:
        fprintf(stderr, "orb_emu: invalid option: %s\n", arg); usage(); return 2;
    }
    static const struct { const char *name; orb_shell_surface_t surface; } scenes[] = {
        {"face", ORB_SHELL_FACE}, {"agents", ORB_SHELL_AGENTS}, {"home", ORB_SHELL_HOME},
        {"apps", ORB_SHELL_APPS}, {"stack", ORB_SHELL_STACK},
        {"notifications", ORB_SHELL_NOTIFICATIONS}, {"card", ORB_SHELL_CARD},
        {"play", ORB_SHELL_PLAY}, {"marble", ORB_SHELL_PLAY},
    };
    size_t scene_index;
    for (scene_index = 0; scene_index < sizeof(scenes) / sizeof(scenes[0]); ++scene_index)
        if (!strcmp(scene, scenes[scene_index].name)) break;
    if (scene_index == sizeof(scenes) / sizeof(scenes[0])) {
        fprintf(stderr, "orb_emu: unknown scene: %s\n", scene); return 2;
    }
    if (hub) {
        fputs("orb_emu: --hub is not implemented; see docs/emulator.md (pinned TLS, separate pairing and snapshot transport required).\n", stderr);
        return 2;
    }
    if (!make_directory(s_output)) {
        fprintf(stderr, "orb_emu: cannot create output directory: %s\n", s_output); return 1;
    }
    SDL_SetMainReady();
    if (s_headless) SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER)) goto sdl_error;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    s_window = SDL_CreateWindow("Orb emulator · fixtures · P power / B talk / D diagnostics",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SIM_D * (int)scale, SIM_D * (int)scale,
        s_headless ? SDL_WINDOW_HIDDEN : SDL_WINDOW_ALLOW_HIGHDPI);
    if (!s_window) goto sdl_error;
    s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_SOFTWARE);
    if (!s_renderer || SDL_RenderSetLogicalSize(s_renderer, SIM_D, SIM_D)) goto sdl_error;
    s_texture = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STREAMING, SIM_D, SIM_D);
    if (!s_texture || SDL_SetTextureBlendMode(s_texture, SDL_BLENDMODE_BLEND)) goto sdl_error;
    s_epoch = SDL_GetTicks();
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *display = sim_display_create();
    if (!display || !emu_fixture_init(display)) {
        fputs("orb_emu: firmware fixture initialization failed\n", stderr); s_failed = true; goto cleanup;
    }
    if (orb_fonts_set_content_size(text_size) != ORB_FONTS_OK ||
        orb_fonts_set_bold_text(bold) != ORB_FONTS_OK) {
        fputs("orb_emu: font settings unavailable\n", stderr); s_failed = true; goto cleanup;
    }
    orb_ui_set_reduce_transparency(opaque);
    orb_ui_set_reduce_motion(reduce_motion);
    orb_shell_navigate(emu_fixture_shell(), scenes[scene_index].surface);
    if (!strcmp(scene, "card")) orb_shell_select_card(emu_fixture_shell(), "approval-build");
    if (!strcmp(scene, "marble")) (void)orb_shell_app_request("marble");
    if (design_scene && !emu_fixture_design_scene(design_scene)) {
        fprintf(stderr, "orb_emu: unknown design fixture: %s\n", design_scene);
        s_failed = true; goto cleanup;
    }
    s_input = emu_input_create(display, input_action, NULL);
    if (!s_input) { s_failed = true; goto cleanup; }
    /* SDL_RenderSetLogicalSize already transforms pointer events into this
     * 466-unit space, including Retina and --scale 2. Don't divide twice. */
    emu_input_viewport(s_input, 0, 0, SIM_D, SIM_D);
    s_diagnostics = lv_label_create(lv_display_get_layer_sys(display));
    orb_text_apply(s_diagnostics, ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, 0);
    lv_obj_set_pos(s_diagnostics, 91, 141);
    lv_obj_set_width(s_diagnostics, 284);
    lv_obj_set_style_bg_color(s_diagnostics, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_diagnostics, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_diagnostics, lv_color_white(), 0);
    lv_obj_set_clickable(s_diagnostics, false);
    lv_obj_set_scrollable(s_diagnostics, false);
    if (!diagnostics) lv_obj_set_hidden(s_diagnostics, true);
    /* HIG §3/4: even diagnostics fit the circle chord and use Inter >=20 px. */
    lv_display_add_event_cb(display, display_event, LV_EVENT_ALL, NULL);
    uint8_t rgb[SIM_D * SIM_D * 3], rgba[SIM_D * SIM_D * 4];
    uint8_t coverage[SIM_D * SIM_D];
    for (int y = 0; y < SIM_D; ++y)
        for (int x = 0; x < SIM_D; ++x) coverage[y * SIM_D + x] = sim_board_cover(x, y, SIM_D);
    uint64_t next = SDL_GetPerformanceCounter();
    uint64_t shot_session = next;
    uint64_t frequency = SDL_GetPerformanceFrequency();
    unsigned frame = 0;
    while (s_running && (!s_headless || frame < frames)) {
        uint64_t started = SDL_GetPerformanceCounter();
        s_metrics.pixels = 0;
        s_now = s_headless ? frame * 1000 / 60 : tick();
        lv_lock();
        SDL_Event event;
        while (SDL_PollEvent(&event)) emu_input_event(s_input, &event, s_now);
        emu_input_step(s_input, s_now);
        float gravity[3];
        emu_input_gravity(s_input, gravity);
        orb_ui_set_gravity(gravity[0], gravity[1], gravity[2], 1.0f / 60);
        emu_fixture_step(s_now, emu_input_buttons(s_input), true, gravity[0], gravity[1], gravity[2]);
        if (s_now - s_metrics.interval >= 250 || frame == 0) {
            diagnostics_update();
            s_metrics.interval = s_now;
        }
        lv_timer_handler();
        if (frame == 0) lv_refr_now(display);
        lv_unlock();
        s_metrics.dirty_pct = s_metrics.pixels * 100.0 / (SIM_D * SIM_D);
        if (sim_display_take_changed() || frame == 0) {
            sim_display_rgb(rgb);
            for (unsigned i = 0; i < SIM_D * SIM_D; ++i) {
                /* sim_display_rgb already premultiplies the edge by coverage;
                 * composite grey ourselves to avoid multiplying it twice. */
                for (unsigned c = 0; c < 3; ++c)
                    rgba[i * 4 + c] = rgb[i * 3 + c] + (44 * (255 - coverage[i]) + 127) / 255;
                rgba[i * 4 + 3] = 255;
            }
            if (SDL_UpdateTexture(s_texture, NULL, rgba, SIM_D * 4)) goto sdl_error;
        }
        SDL_SetRenderDrawColor(s_renderer, 44, 44, 44, 255);
        SDL_RenderClear(s_renderer);
        SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
        if (s_headless || s_shot) {
            char path[PATH_MAX];
            int count = s_headless ?
                snprintf(path, sizeof(path), "%s/frame-%06u.png", s_output, frame + 1) :
                snprintf(path, sizeof(path), "%s/orb-emu-%llu-%06u.png", s_output,
                    (unsigned long long)shot_session, ++s_shots);
            if (count < 0 || count >= sizeof(path) || !screenshot(path)) s_failed = true;
            else if (!s_headless) printf("orb_emu: wrote %s\n", path);
            s_shot = false;
        }
        SDL_RenderPresent(s_renderer);
        ++frame;
        ++s_metrics.frames;
        s_metrics.frame_ms = elapsed_ms(started);
        if (!s_headless) {
            next += frequency / 60;
            uint64_t now = SDL_GetPerformanceCounter();
            if (next > now) SDL_Delay((uint32_t)((next - now) * 1000 / frequency));
            else next = now;
            s_metrics.frame_ms = elapsed_ms(started);
        }
        s_metrics.fps = s_metrics.frame_ms > 0 ? 1000.0 / s_metrics.frame_ms : 0;
    }
    if (s_headless) {
        lv_mem_monitor_t memory;
        lv_mem_monitor(&memory);
        printf("orb_emu: %u SDL frames, %u LVGL renders; LVGL %zu bytes, peak %zu; output %s\n",
            frame, s_metrics.rendered, memory.total_size, memory.max_used, s_output);
    }
    goto cleanup;
sdl_error:
    fprintf(stderr, "orb_emu: SDL: %s\n", SDL_GetError()); s_failed = true;
cleanup:
    if (s_input) emu_input_destroy(s_input);
    if (s_diagnostics) lv_obj_delete(s_diagnostics);
    emu_fixture_shutdown();
    /* Match existing simulators: LVGL owns process-lifetime draw workers. */
    if (s_texture) SDL_DestroyTexture(s_texture);
    if (s_renderer) SDL_DestroyRenderer(s_renderer);
    if (s_window) SDL_DestroyWindow(s_window);
    SDL_Quit();
    return s_failed ? 1 : 0;
}
