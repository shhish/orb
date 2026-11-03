/* Open response is a visual contract, not a host stopwatch benchmark. The
 * simulator proves changed cached pixels, bounded construction and cancellation;
 * input-to-DMA timing and internal heap margins belong to the lead's board. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_fonts.h"
#include "orb_apps.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include "sim_display.h"
#include "../../firmware/components/orb_devtools/orb_open_source.h"

static uint32_t s_now;
static unsigned s_checks;
static lv_display_t *s_display;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;
static orb_shell_t *s_shell;
static uint8_t s_before[SIM_D * SIM_D * 3], s_after[SIM_D * SIM_D * 3];
static size_t s_material_bytes, s_material_allocations;
static const char *s_output;

#define CHECK(condition) do { \
    s_checks++; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static uint32_t tick(void) { return s_now; }

/* Orb's external material allocator has no platform heap ambiguity: each live
 * mask remains charged until teardown, even when its page is no longer shown. */
static void settle(void);

static void boot_back(void)
{
    orb_button_event_t event = {.button = ORB_BUTTON_BOOT, .gesture = ORB_BUTTON_SHORT, .synthetic = true};
    orb_shell_button(s_shell, &event);
    settle();
}

static void *material_alloc(size_t size)
{
    size_t *allocation = malloc(sizeof(*allocation) + size);
    if (!allocation) return NULL;
    *allocation = size;
    s_material_bytes += size;
    s_material_allocations++;
    return allocation + 1;
}

static void material_free(void *ptr)
{
    if (!ptr) return;
    size_t *allocation = (size_t *)ptr - 1;
    CHECK(s_material_bytes >= *allocation && s_material_allocations > 0);
    s_material_bytes -= *allocation;
    s_material_allocations--;
    free(allocation);
}

static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev);
    *data = s_input;
}

static void step(unsigned ms)
{
    s_now += ms;
    orb_shell_poll(s_shell);
    lv_timer_handler();
    lv_refr_now(s_display);
}

static void settle(void)
{
    for (unsigned i = 0; i < 60; i++) step(20);
}

static lv_obj_t *find_label(lv_obj_t *root, const char *caption)
{
    if (!lv_obj_is_visible(root)) return NULL;
    const char *rim = orb_kit_rim_text_get(root);
    if (rim && !strcasecmp(rim, caption)) return root;
    if (lv_obj_check_type(root, &lv_label_class) &&
        !strcmp(lv_label_get_text(root), caption)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_label(lv_obj_get_child(root, i), caption);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *face_apps(lv_obj_t *root)
{
    if (!lv_obj_is_visible(root)) return NULL;
    /* The Face launch token now uses the kit56 mini without a button subclass. */
    if (lv_obj_is_clickable(root) && lv_obj_get_width(root) == 56 &&
        lv_obj_get_height(root) == 56) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = face_apps(lv_obj_get_child(root, i));
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *control(const char *caption)
{
    if (!strcmp(caption, "Apps")) return face_apps(orb_shell_root(s_shell));
    lv_obj_t *label = find_label(orb_shell_root(s_shell), caption);
    for (lv_obj_t *obj = label; obj; obj = lv_obj_get_parent(obj))
        if (lv_obj_is_clickable(obj)) return obj;
    return NULL;
}

/* Release dispatches the actual CLICKED event. The subsequent timer pass admits
 * the cached navigation; no synthetic navigation call supplies its pixels. */
static void click(const char *caption)
{
    lv_obj_t *obj = control(caption);
    if (!obj) fprintf(stderr, "missing open control: %s\n", caption);
    CHECK(obj != NULL);
    lv_area_t bounds;
    lv_obj_get_coords(obj, &bounds);
    s_input.point = (lv_point_t){(bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2};
    s_input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_pointer);
    s_now++;
    s_input.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer);
}

static orb_shell_open_metrics_t metrics(void)
{
    orb_shell_open_metrics_t m = {0};
    orb_shell_open_metrics(s_shell, &m);
    /* interaction-model phase1 admits two peers plus the once-scaled sheet
     * background, with a separately bounded <=96x96 A8 launch glyph. */
    CHECK(m.cache_entries <= 3);
    CHECK(m.cache_bytes <= 3u * SIM_D * SIM_D * 2u + 96u * 96u);
    return m;
}

static unsigned object_count(lv_obj_t *obj)
{
    unsigned count = 1;
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); i++)
        count += object_count(lv_obj_get_child(obj, i));
    return count;
}

static void shot(orb_shell_surface_t surface, bool cold, const char *phase)
{
    if (!s_output) return;
    const char *name = surface == ORB_SHELL_APPS ? "apps" : surface == ORB_SHELL_PLAY ? "play" :
                       surface == ORB_SHELL_DOT ? "dot" : "agents";
    char path[512];
    snprintf(path, sizeof(path), "%s/open-%s-%s-%s.png", s_output, name, cold ? "cold" : "warm", phase);
    CHECK(sim_display_save_png(path));
}

static unsigned white_pixels(int x1, int y1, int x2, int y2)
{
    sim_display_rgb(s_after);
    unsigned white = 0;
    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            const uint8_t *p = &s_after[(y * SIM_D + x) * 3];
            white += p[0] > 210 && p[1] > 210 && p[2] > 210;
        }
    }
    return white;
}

static lv_obj_t *arc_ink(lv_obj_t *root)
{
    if (lv_obj_check_type(root, &lv_arclabel_class)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = arc_ink(lv_obj_get_child(root, i));
        if (found) return found;
    }
    return NULL;
}

static void check_rim_pixels(const char *caption)
{
    lv_obj_t *title = find_label(orb_shell_root(s_shell), caption); CHECK(title);
    orb_kit_rim_text_info_t info; CHECK(orb_kit_rim_text_info(title, &info));
    CHECK(info.angle_degrees <= ORB_KIT_TITLE_MAX_DEGREES);
    lv_obj_t *ink = arc_ink(title); CHECK(ink);
    lv_color32_t colour = lv_color_to_32(lv_obj_get_style_text_color(ink, 0), LV_OPA_COVER);
    lv_area_t box; lv_obj_get_coords(title, &box);
    sim_display_rgb(s_after);
    unsigned count = 0;
    for (int y = LV_MAX(0, box.y1); y <= LV_MIN(SIM_D - 1, box.y2); ++y)
        for (int x = LV_MAX(0, box.x1); x <= LV_MIN(SIM_D - 1, box.x2); ++x) {
            const uint8_t *p = &s_after[(y * SIM_D + x) * 3];
            count += LV_MAX(p[0], LV_MAX(p[1], p[2])) > 96 &&
                abs((int)p[0] - colour.red) < 32 && abs((int)p[1] - colour.green) < 32 &&
                abs((int)p[2] - colour.blue) < 32;
        }
    CHECK(count > 20);
}

static void first_frame(const char *caption, orb_shell_surface_t dest, bool cold)
{
    sim_display_rgb(s_before);
    sim_display_take_changed();
    click(caption);
    CHECK(orb_shell_surface(s_shell) == dest);
    if (dest == ORB_SHELL_AGENTS || dest == ORB_SHELL_DOT) {
        /* Even the cheap opening scaffold must use the migrated rim family. */
        lv_obj_t *title = find_label(orb_shell_root(s_shell), dest == ORB_SHELL_DOT ? "Dot" : "Agents");
        if (!title && dest == ORB_SHELL_AGENTS) title = find_label(orb_shell_root(s_shell), "Firmware build");
        orb_kit_rim_text_info_t info;
        CHECK(title && orb_kit_rim_text_info(title, &info));
        CHECK(info.angle_degrees <= ORB_KIT_TITLE_MAX_DEGREES);
    }
    orb_shell_open_metrics_t m = metrics();
    CHECK(m.surface == dest && m.cold == cold);
    CHECK(m.active);
    uint32_t sequence = m.sequence;
    /* HIG §6: a tap changes its cached presentation within two 16ms host timer
     * opportunities. A cold body may be captured once before that frame; the
     * old deferred-scaffold requirement no longer describes iris navigation. */
    unsigned feedback_ms = 0;
    do {
        step(1);
        sim_display_rgb(s_after);
    } while (!memcmp(s_before, s_after, sizeof(s_before)) && ++feedback_ms < 32);
    CHECK(sim_display_take_changed());
    CHECK(memcmp(s_before, s_after, sizeof(s_before)) != 0);
    m = metrics();
    CHECK(m.sequence == sequence && m.surface == dest);
    shot(dest, cold, "first");
    /* An iris reveals the destination header only when its circle reaches it.
     * Check the moving pixels, then both heading and Back once it settles. */
    for (unsigned i = 0; i < 28; i++) {
        step(20);
        if (i == 3) {
            sim_display_rgb(s_after);
            CHECK(memcmp(s_before, s_after, sizeof(s_before)) != 0);
            shot(dest, cold, "moving");
        }
    }
    m = metrics();
    CHECK(m.content_ready);
    CHECK(m.transition_end_us >= m.input_us);
    /* Arrival ink may finish for at most 450ms after the transition; it may
     * never hold up construction, the first response, or the transition. */
    while (m.active && (int64_t)s_now * 1000 - m.transition_end_us < 450000) {
        step(8);
        m = metrics();
    }
    CHECK(!m.active);
    /* Play's Design 1.0 title sits beneath its arena miniature. */
    if (dest == ORB_SHELL_DOT || dest == ORB_SHELL_AGENTS)
        check_rim_pixels(dest == ORB_SHELL_DOT ? "Dot" : "Firmware build");
    else CHECK(dest == ORB_SHELL_APPS ? white_pixels(100, 390, 366, 460) > 20 :
               dest == ORB_SHELL_PLAY ? white_pixels(120, 270, 346, 310) > 20 :
               white_pixels(120, 34, 346, 130) > 20);
    shot(dest, cold, "final");
    /* Apps deliberately has no toolbar Back; all deeper places retain it. */
    CHECK(!find_label(orb_shell_root(s_shell), ORB_ICON_CHEVRON_LEFT));
}

static void pointer_entries(void)
{
    first_frame("Apps", ORB_SHELL_APPS, true);
    first_frame(ORB_ICON_GAME_CONTROLLER, ORB_SHELL_PLAY, true);
    CHECK(find_label(orb_shell_root(s_shell), "Play"));
    boot_back();
    settle();
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_APPS);
    first_frame(ORB_ICON_CHAT, ORB_SHELL_DOT, true);
    CHECK(find_label(orb_shell_root(s_shell), "Dot"));
    boot_back();
    settle();
    first_frame(ORB_ICON_PROMPT, ORB_SHELL_AGENTS, true);
    CHECK(find_label(orb_shell_root(s_shell), "Firmware build"));
    boot_back();
    settle();
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    /* Agents is a Face neighbour even when launched from Apps. Re-enter the
     * launcher explicitly before measuring its already-visited warm open. */
    orb_shell_navigate(s_shell, ORB_SHELL_APPS);
    settle();
    first_frame(ORB_ICON_PROMPT, ORB_SHELL_AGENTS, false);
}

static void interruption_and_updates(void)
{
    orb_shell_navigate(s_shell, ORB_SHELL_APPS);
    settle();
    click(ORB_ICON_CHAT);
    step(1);
    orb_shell_open_metrics_t opening = metrics();
    CHECK(opening.active && opening.surface == ORB_SHELL_DOT);
    orb_shell_back(s_shell);
    step(1);
    orb_shell_open_metrics_t reversing = metrics();
    CHECK(reversing.sequence != opening.sequence);
    CHECK(reversing.surface == ORB_SHELL_APPS);
    settle();
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_APPS);
    CHECK(control(ORB_ICON_CHAT));
    CHECK(!find_label(orb_shell_root(s_shell), "No conversation"));

    /* A warm cached image must not mask a changed title. Update while the
     * destination is absent, then open it through the same pointer control. */
    orb_model_snapshot_t snapshot;
    CHECK(orb_model_snapshot(&snapshot));
    CHECK(snapshot.session_count > 0);
    snprintf(snapshot.sessions[0].title, sizeof(snapshot.sessions[0].title), "New title");
    CHECK(orb_model_upsert_session(&snapshot.sessions[0]));
    step(20);
    click(ORB_ICON_PROMPT);
    step(1);
    /* A content revision must invalidate the old body before capture. The new
     * live destination exists behind the iris and its settled pixels follow. */
    CHECK(find_label(orb_shell_root(s_shell), "New title"));
    CHECK(!find_label(orb_shell_root(s_shell), "Firmware build"));
    settle();
    CHECK(find_label(orb_shell_root(s_shell), "New title"));
    CHECK(!find_label(orb_shell_root(s_shell), "Firmware build"));

    /* Font epoch changes invalidate snapshots before they can be presented
     * again; the visible page follows the shared typography's actual face. */
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK);
    settle();
    CHECK(find_label(orb_shell_root(s_shell), "New title"));
    CHECK(orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3);
    CHECK(orb_fonts_bold_text());
    metrics();
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(false) == ORB_FONTS_OK);
    settle();
}

static void bounded_lifetimes(void)
{
    orb_shell_navigate(s_shell, ORB_SHELL_APPS);
    settle();
    size_t plateau_bytes = 0, plateau_allocations = 0, plateau_cache = 0;
    unsigned plateau_objects = 0;
    for (unsigned cycle = 0; cycle < 22; cycle++) {
        click(ORB_ICON_GAME_CONTROLLER);
        settle();
        boot_back();
        settle();
        orb_shell_open_metrics_t m = metrics();
        unsigned objects = object_count(orb_shell_root(s_shell));
        if (cycle == 1) {
            plateau_bytes = s_material_bytes;
            plateau_allocations = s_material_allocations;
            plateau_cache = m.cache_bytes;
            plateau_objects = objects;
        } else if (cycle > 1) {
            CHECK(s_material_bytes == plateau_bytes);
            CHECK(s_material_allocations == plateau_allocations);
            CHECK(m.cache_bytes == plateau_cache);
            CHECK(objects == plateau_objects);
        }
    }
    /* Neither a pending build nor an in-flight spring may outlive its owner. */
    click(ORB_ICON_CHAT);
    step(1);
    orb_shell_destroy(s_shell);
    s_shell = NULL;
    for (unsigned i = 0; i < 60; i++) {
        s_now += 20;
        lv_timer_handler();
        lv_refr_now(s_display);
    }
    orb_glass_cache_clear();
    CHECK(s_material_bytes == 0);
    CHECK(s_material_allocations == 0);
}

static void repeated_benchmark_sources(void)
{
    static const orb_shell_surface_t targets[] = {
        ORB_SHELL_APPS, ORB_SHELL_PLAY, ORB_SHELL_DOT, ORB_SHELL_AGENTS, ORB_SHELL_APPS,
    };
    for (unsigned surface = 0; surface < sizeof(targets) / sizeof(targets[0]); surface++) {
        bool back = surface == 4;
        orb_shell_surface_t target = targets[surface];
        orb_shell_surface_t source = back ? ORB_SHELL_PLAY :
                                     target == ORB_SHELL_APPS ? ORB_SHELL_FACE : ORB_SHELL_APPS;
        /* Reproduce the board report: Apps (or the measured destination) is
         * already open when the next sample starts, including warm restores. */
        orb_shell_navigate(s_shell, target);
        settle();
        for (unsigned run = 0; run < 20; run++) {
            orb_open_source_t setup;
            orb_open_source_begin(&setup, s_shell, source);
            unsigned ticks = 0;
            while (!orb_open_source_ready(&setup, s_shell) && ticks++ < 160) step(20);
            CHECK(ticks < 160);
            CHECK(orb_open_source_settled(s_shell, source));
            uint32_t previous_sequence = metrics().sequence;
            if (back) orb_shell_back(s_shell);
            else orb_shell_navigate(s_shell, target);
            CHECK(metrics().sequence != previous_sequence);
            CHECK(metrics().surface == target);
            CHECK(metrics().active);
            settle();
            CHECK(orb_open_source_settled(s_shell, target));
            /* Same destination restoration must finish before the next begin. */
            orb_shell_navigate(s_shell, target);
            settle();
            CHECK(orb_open_source_settled(s_shell, target));
        }
    }
}

int main(int argc, char **argv)
{
    s_output = argc > 1 ? argv[1] : NULL;
    lv_init();
    lv_tick_set_cb(tick);
    s_display = sim_display_create();
    CHECK(s_display);
    orb_ui_config_t ui = {.alloc = material_alloc, .free = material_free};
    orb_ui_init(&ui);
    orb_apps_init(ORB_APP_NEEDS_IMU);
    CHECK(orb_model_init());
    orb_model_load_demo();
    orb_model_set_connection(true, true);
    s_pointer = lv_indev_create();
    lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_pointer, s_display);
    lv_indev_set_read_cb(s_pointer, read_pointer);
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL);
    CHECK(s_shell);
    orb_shell_snapshot_t status = {
        .net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .rssi = -54, .usb = true, .brightness = 70,
    };
    snprintf(status.clock_time, sizeof(status.clock_time), "10:09");
    snprintf(status.clock_date, sizeof(status.clock_date), "Tuesday, October 27");
    orb_shell_update(s_shell, &status);
    settle();
    pointer_entries();
    interruption_and_updates();
    repeated_benchmark_sources();
    orb_ui_set_reduce_motion(true);
    orb_shell_navigate(s_shell, ORB_SHELL_APPS);
    settle();
    sim_display_rgb(s_before);
    sim_display_take_changed();
    click(ORB_ICON_CHAT);
    step(20);
    CHECK(sim_display_take_changed());
    sim_display_rgb(s_after);
    CHECK(memcmp(s_before, s_after, sizeof(s_before)) != 0);
    CHECK(find_label(orb_shell_root(s_shell), "Dot"));
    /* Reduce Motion cross-fades for150ms; title ink settles only after
     * that bounded fade. The20ms feedback above already checks changed pixels. */
    CHECK(metrics().content_ready); /* Captured once; motion only reads pixels. */
    if (s_output) {
        char path[512];
        snprintf(path, sizeof(path), "%s/open-dot-rm-first.png", s_output);
        CHECK(sim_display_save_png(path));
    }
    for (unsigned i = 0; i < 9; i++) step(20);
    CHECK(!metrics().active); /* HIG §6: Reduce Motion is a 150 ms fade. */
    check_rim_pixels("Dot");
    settle();
    orb_ui_set_reduce_motion(false);
    bounded_lifetimes();
    lv_indev_delete(s_pointer);
    printf("open_latency: %u visual/lifecycle checks passed\n", s_checks);
    return 0;
}
