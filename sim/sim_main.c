/*
 * Orb's desktop simulator: the firmware's UI (orb_ui, orb_demo) on the
 * firmware's LVGL fork and settings, over a fake board. It starts the UI as
 * firmware/main/main.c does and feeds it the fake motion sensor through the
 * same hook, from an LVGL timer at the same rate.
 *
 * In a window (default) time is the wall clock. Headless (--headless) time is
 * virtual: it jumps from one LVGL timer to the next, so a run is the same
 * every time and screenshots land on exact moments, however fast this
 * machine renders.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "orb_demo.h"
#include "orb_fonts.h"
#include "orb_ui.h"
#include "sim_board.h"
#include "sim_display.h"
#include "sim_window.h"

#define TICK_MS 20            /* the motion sensor at 50 Hz, as main.c */
#define MAX_EVENTS 64
#define MAX_FRAMES 10000
#define MAX_WAIT_MS 16        /* the window checks input at least this often */

typedef struct {
    uint32_t ms;
    float right, down;        /* a tilt */
    const char *path;         /* or a screenshot */
} event_t;

static bool s_headless;
static uint32_t s_now;        /* virtual ms, headless */
static struct timespec s_t0;
static float s_tilt_right, s_tilt_down;
static event_t s_tilts[MAX_EVENTS], s_shots[MAX_EVENTS];
static int s_tilt_n, s_shot_n, s_frame_n;
static const char *s_out_dir = ".";

static uint32_t tick_ms(void)
{
    if (s_headless) {
        return s_now;
    }
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((t.tv_sec - s_t0.tv_sec) * 1000 + (t.tv_nsec - s_t0.tv_nsec) / 1000000);
}

/* The fake motion sensor into the demo, as main.c's motion_timer does with the real one. */
static void motion_timer(lv_timer_t *t)
{
    static uint32_t last;
    static bool primed;
    uint32_t now = lv_tick_get();
    float g[3];
    sim_board_gravity(s_tilt_right, s_tilt_down, g);
    orb_demo_tick(g[0], g[1], g[2], primed ? lv_tick_diff(now, last) / 1e3f : TICK_MS / 1e3f);
    last = now;
    primed = true;
}

static void usage(void)
{
    fputs("usage: orb_sim [options]\n"
          "  --headless            no window; virtual time, for screenshots\n"
          "  --shot MS=FILE        headless: save the screen MS ms after start as a PNG\n"
          "  --frames N            headless: save the first N frames (one per 16 ms) into --out\n"
          "                        as frame-0000.png, frame-0001.png, ...\n"
          "  --tilt MS=RIGHT,DOWN  headless: from MS ms on, dip the right and bottom edges by\n"
          "                        RIGHT and DOWN degrees (negative: the left and top edges)\n"
          "  --reduce-motion       start with Reduce Motion on\n"
          "  --reduce-transparency start with opaque accessibility surfaces\n"
          "  --text-size SIZE      default, larger or ax3 for visual review\n"
          "  --out DIR             where --frames and the window's 's' key save (default .)\n",
          stderr);
}

static int by_time(const void *a, const void *b)
{
    const event_t *x = a, *y = b;
    return x->ms < y->ms ? -1 : x->ms > y->ms;
}

static bool add_event(event_t *list, int *n, event_t ev)
{
    if (*n >= MAX_EVENTS) {
        fprintf(stderr, "orb_sim: more than %d events\n", MAX_EVENTS);
        return false;
    }
    list[(*n)++] = ev;
    return true;
}

static bool parse_shot(const char *arg)
{
    char *end;
    unsigned long ms = strtoul(arg, &end, 10);
    if (end == arg || *end != '=' || !end[1]) {
        return false;
    }
    return add_event(s_shots, &s_shot_n, (event_t){ .ms = (uint32_t)ms, .path = end + 1 });
}

static bool parse_tilt(const char *arg)
{
    unsigned long ms;
    float right, down;
    char tail;
    if (sscanf(arg, "%lu=%f,%f%c", &ms, &right, &down, &tail) != 3) {
        return false;
    }
    return add_event(s_tilts, &s_tilt_n, (event_t){ .ms = (uint32_t)ms, .right = right, .down = down });
}

static bool save(const char *path)
{
    if (!sim_display_save_png(path)) {
        return false;
    }
    printf("orb_sim: %u ms: %s\n", (unsigned)s_now, path);
    return true;
}

/*
 * Virtual time: run the timers due now, then jump to whichever comes first,
 * the next timer, tilt, screenshot or frame. A screenshot first renders what
 * the timers changed, as the next frame would.
 */
static int run_headless(lv_display_t *disp)
{
    if (!s_shot_n && !s_frame_n) {
        fputs("orb_sim: --headless needs --shot or --frames\n", stderr);
        return 2;
    }
    qsort(s_tilts, (size_t)s_tilt_n, sizeof(event_t), by_time);
    qsort(s_shots, (size_t)s_shot_n, sizeof(event_t), by_time);
    int tilt = 0, shot = 0, frame = 0, failed = 0;
    while (shot < s_shot_n || frame < s_frame_n) {
        for (; tilt < s_tilt_n && s_tilts[tilt].ms <= s_now; tilt++) {
            s_tilt_right = s_tilts[tilt].right;
            s_tilt_down = s_tilts[tilt].down;
        }
        uint32_t frame_ms = (uint32_t)frame * LV_DEF_REFR_PERIOD;
        bool due = (shot < s_shot_n && s_shots[shot].ms <= s_now) || (frame < s_frame_n && frame_ms <= s_now);
        lv_lock();
        uint32_t wait = lv_timer_handler();
        if (due) {
            lv_refr_now(disp);
        }
        lv_unlock();
        for (; shot < s_shot_n && s_shots[shot].ms <= s_now; shot++) {
            failed += !save(s_shots[shot].path);
        }
        if (frame < s_frame_n && frame_ms <= s_now) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/frame-%04d.png", s_out_dir, frame++);
            failed += !save(path);
        }

        /* At least a millisecond on; LVGL says LV_NO_TIMER_READY when nothing is scheduled. */
        uint32_t next = s_now + (wait ? LV_MIN(wait, 1000) : 1);
        if (shot < s_shot_n && s_shots[shot].ms < next) {
            next = s_shots[shot].ms;
        }
        if (frame < s_frame_n && (uint32_t)frame * LV_DEF_REFR_PERIOD < next) {
            next = (uint32_t)frame * LV_DEF_REFR_PERIOD;
        }
        if (tilt < s_tilt_n && s_tilts[tilt].ms < next) {
            next = s_tilts[tilt].ms;
        }
        s_now = next;
    }
    return failed ? 1 : 0;
}

static int run_window(lv_display_t *disp)
{
    if (!sim_window_open(disp, s_out_dir)) {
        return 1;
    }
    puts("orb_sim: drag to touch; arrows or Shift-drag to tilt, 0 to level; m Reduce Motion; s screenshot; q quit");
    for (;;) {
        sim_window_tilt(&s_tilt_right, &s_tilt_down);
        lv_lock();
        uint32_t wait = lv_timer_handler();
        lv_unlock();
        if (sim_display_take_changed()) {
            sim_window_present();
        }
        if (!sim_window_poll(wait < MAX_WAIT_MS ? wait : MAX_WAIT_MS)) {
            break;
        }
    }
    sim_window_close();
    return 0;
}

int main(int argc, char **argv)
{
    bool reduce_motion = false;
    bool reduce_transparency = false;
    orb_content_size_t text_size = ORB_CONTENT_SIZE_DEFAULT;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        int first = i;
        bool ok = true;
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage();
            return 0;
        } else if (!strcmp(a, "--headless")) {
            s_headless = true;
        } else if (!strcmp(a, "--reduce-motion")) {
            reduce_motion = true;
        } else if (!strcmp(a, "--reduce-transparency")) {
            reduce_transparency = true;
        } else if (v && !strcmp(a, "--text-size")) {
            if (!strcmp(v, "default")) text_size = ORB_CONTENT_SIZE_DEFAULT;
            else if (!strcmp(v, "larger")) text_size = ORB_CONTENT_SIZE_LARGER;
            else if (!strcmp(v, "ax3")) text_size = ORB_CONTENT_SIZE_AX3;
            else ok = false;
            i++;
        } else if (v && !strcmp(a, "--shot")) {
            ok = parse_shot(v);
            i++;
        } else if (v && !strcmp(a, "--tilt")) {
            ok = parse_tilt(v);
            i++;
        } else if (v && !strcmp(a, "--frames")) {
            char *end;
            long n = strtol(v, &end, 10);
            ok = end != v && !*end && n > 0 && n <= MAX_FRAMES;
            s_frame_n = (int)n;
            i++;
        } else if (v && !strcmp(a, "--out")) {
            s_out_dir = v;
            i++;
        } else {
            ok = false;
        }
        if (!ok) {
            fprintf(stderr, "orb_sim: bad argument: %s%s%s\n", a, i > first ? " " : "", i > first ? v : "");
            usage();
            return 2;
        }
    }
    if (!s_headless && (s_shot_n || s_tilt_n || s_frame_n)) {
        fputs("orb_sim: --shot, --frames and --tilt need --headless\n", stderr);
        return 2;
    }

    clock_gettime(CLOCK_MONOTONIC, &s_t0);
    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *disp = sim_display_create();

    /* As main.c: the UI kit, then the demo on the LVGL thread (this one), then the motion timer. */
    lv_lock();
    orb_ui_init(NULL);
    if (orb_fonts_set_content_size(text_size) != ORB_FONTS_OK) return 1;
    orb_ui_set_reduce_motion(reduce_motion);
    orb_ui_set_reduce_transparency(reduce_transparency);
    orb_demo_build(lv_screen_active());
    lv_timer_create(motion_timer, TICK_MS, NULL);
    lv_unlock();

    return s_headless ? run_headless(disp) : run_window(disp);
}
