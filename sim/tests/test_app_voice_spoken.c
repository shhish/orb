/* Spoken presentation owns a copied caption and one framework root. Playback
 * and the shell's underlying-screen restoration remain independent owners. */
#include "orb_app_voice_spoken.h"
#include "orb_sound_overlay.h"
#include "orb_fonts.h"
#include "orb_input.h"
#include "orb_kit.h"
#include "orb_memory.h"
#include "orb_model.h"
#include "orb_screen_lifecycle.h"
#include "orb_ui.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t s_now = 100000;
static unsigned s_stops, s_intents, s_timer_baseline;
static size_t s_workspace_baseline;
static lv_display_t *s_display;
static lv_indev_data_t s_pointer_data;
static orb_app_voice_spoken_model_t s_initial = {.caption = "Initial spoken caption", .agent = ORB_AGENT_DOT};
static orb_app_voice_spoken_model_t s_changed = {.caption = "Changed spoken caption", .agent = ORB_AGENT_CODEX};

static uint32_t tick(void) { return s_now; }

static void advance(unsigned milliseconds)
{
    for (unsigned elapsed = 0; elapsed < milliseconds; elapsed += 16) {
        s_now += 16;
        lv_timer_handler();
    }
}

static unsigned timers(void)
{
    unsigned count = 0;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer)) count++;
    return count;
}

static size_t used(void)
{
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    return stats.used;
}

static void quiescent(void)
{
    advance(1600);
    assert(used() == s_workspace_baseline);
    assert(timers() == s_timer_baseline);
}

static lv_obj_t *find(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}

static bool stop(void *context)
{
    unsigned *stops = context;
    (*stops)++;
    return true;
}

static bool intent(void *context, const orb_intent_t *value)
{
    (void)context; (void)value;
    s_intents++;
    return true;
}

static void exercise(orb_screen_instance_t *instance)
{
    assert(!orb_screen_rim_acquire(instance));
    assert(orb_screen_back(instance) == ORB_BACK_CLOSE_LAYER);
    assert(!s_intents && !s_stops);
}

static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    lv_obj_t *root = orb_screen_root(instance);
    assert(root && lv_obj_is_clickable(root) && !lv_obj_is_scrollable(root));
    assert(lv_obj_get_width(root) == 466 && lv_obj_get_height(root) == 466);
    assert(lv_color_eq(lv_obj_get_style_bg_color(root, 0), lv_color_black()));
    assert(lv_obj_get_style_bg_opa(root, 0) == LV_OPA_COVER);
    orb_screen_chrome_t chrome;
    orb_screen_chrome(instance, &chrome);
    assert(!strcmp(chrome.title, "Spoken reply"));
    const char *expected = phase <= ORB_LIFECYCLE_IDEMPOTENT ? s_initial.caption : s_changed.caption;
    if (phase != ORB_LIFECYCLE_EMPTY) assert(find(root, expected));
    assert(!s_intents && !s_stops);
}

static void lifecycle(void)
{
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.intent = intent, .defer_rim = true});
    orb_screen_model_t initial_model = {&s_initial, sizeof(s_initial)};
    orb_screen_model_t changed_model = {&s_changed, sizeof(s_changed)};
    orb_screen_update_t initial = {.revisions.model = 1, .model = &initial_model};
    orb_screen_update_t changed = {.revisions.model = 2, .model = &changed_model};
    orb_screen_update_t removed = {.revisions.model = 3, .model = &changed_model};
    orb_screen_update_t empty = {.revisions.model = 4};
    orb_lifecycle_case_t test = {.descriptor = orb_app_voice_spoken_descriptor(),
        .initial = &initial, .changed = &changed, .removed = &removed, .empty = &empty,
        .exercise = exercise, .verify = verify, .quiescent = quiescent};
    orb_test_screen_lifecycle(&host, &test);
}

static void pointer_read(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input;
    *data = s_pointer_data;
}

static lv_indev_t *pointer(bool synthetic)
{
    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input, s_display);
    lv_indev_set_read_cb(input, pointer_read);
    lv_indev_set_mode(input, LV_INDEV_MODE_EVENT);
    if (synthetic) orb_input_mark_synthetic(input);
    return input;
}

static void tap_stop(lv_indev_t *input, lv_obj_t *root)
{
    lv_obj_t *label = find(root, ORB_ICON_STOP_FILL);
    assert(label);
    lv_obj_t *control = label;
    while (control && !lv_obj_is_clickable(control)) control = lv_obj_get_parent(control);
    assert(control && control != root);
    lv_obj_update_layout(root);
    lv_area_t area;
    lv_obj_get_coords(control, &area);
    s_pointer_data.point = (lv_point_t){(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    s_pointer_data.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(input);
    advance(48);
    s_pointer_data.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(input);
    advance(48);
}

static void presentation(void)
{
    orb_app_voice_spoken_t *view = orb_app_voice_spoken_create(false, stop, &s_stops);
    assert(view && orb_memory_size(view) == orb_app_voice_spoken_size());
    assert(!orb_app_voice_spoken_root(view) && !orb_app_voice_spoken_stop(view));
    char caption[] = "Caption owned by LVGL";
    assert(orb_app_voice_spoken_present(view, s_display, true, false, caption, ORB_AGENT_CLAUDE));
    lv_obj_t *root = orb_app_voice_spoken_root(view);
    assert(root && lv_obj_get_parent(root) == lv_display_get_layer_top(s_display));
    memset(caption, 'x', sizeof(caption) - 1);
    assert(find(root, "Caption owned by LVGL"));
    lv_obj_t *label = find(root, "Caption owned by LVGL");
    int line = lv_obj_get_style_text_font(label, 0)->line_height;
    int spacing = lv_obj_get_style_text_line_space(label, 0);
    assert((lv_obj_get_height(lv_obj_get_parent(label)) + spacing) % (line + spacing) == 0);
    assert(orb_app_voice_spoken_present(view, s_display, true, false, "Caption owned by LVGL", ORB_AGENT_CLAUDE));
    assert(root == orb_app_voice_spoken_root(view));
    orb_app_voice_spoken_show(view, false);
    assert(lv_obj_is_hidden(root));
    orb_app_voice_spoken_show(view, true);
    assert(!lv_obj_is_hidden(root));

    lv_indev_t *physical = pointer(false), *synthetic = pointer(true);
    tap_stop(synthetic, root);
    assert(!s_stops && find(lv_display_get_layer_top(s_display), "Answer on Orb"));
    advance(1600);
    tap_stop(physical, root);
    assert(s_stops == 1);
    orb_app_voice_spoken_set_stop(view, NULL, NULL);
    assert(!orb_app_voice_spoken_stop(view));
    orb_app_voice_spoken_set_stop(view, stop, &s_stops);
    assert(orb_app_voice_spoken_stop(view) && s_stops == 2);
    assert(orb_app_voice_spoken_present(view, s_display, true, true, NULL, -1));
    assert(find(root, "Buffering…"));
    assert(orb_app_voice_spoken_present(view, s_display, true, false, "", -1));
    assert(find(root, "Waiting for captions…"));
    /* Parent deletion retires the framework instance without touching audio.
     * A later presentation safely creates a new modal root. */
    lv_obj_delete(root);
    assert(!orb_app_voice_spoken_root(view) && s_stops == 2);
    assert(orb_app_voice_spoken_present(view, s_display, true, false, "Replacement caption", ORB_AGENT_DOT));
    assert(find(orb_app_voice_spoken_root(view), "Replacement caption"));
    assert(orb_app_voice_spoken_present(view, s_display, false, false, NULL, -1));
    assert(!orb_app_voice_spoken_root(view) && !orb_app_voice_spoken_stop(view));
    lv_indev_delete(synthetic);
    lv_indev_delete(physical);
    orb_app_voice_spoken_destroy(view);
    quiescent();
}

static void preview(void)
{
    unsigned before = s_stops;
    orb_app_voice_spoken_t *view = orb_app_voice_spoken_create(true, stop, &s_stops);
    assert(view && orb_app_voice_spoken_present(view, s_display, true, false, "Read-only preview", ORB_AGENT_DOT));
    assert(!orb_app_voice_spoken_stop(view));
    lv_indev_t *physical = pointer(false);
    tap_stop(physical, orb_app_voice_spoken_root(view));
    assert(s_stops == before);
    lv_indev_delete(physical);
    orb_app_voice_spoken_destroy(view);
    quiescent();
}

static void admission(void)
{
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    size_t reserve = stats.budget - stats.used;
    assert(orb_memory_reserve(ORB_MEMORY_WORKSPACE, reserve));
    assert(!orb_app_voice_spoken_create(false, NULL, NULL));
    orb_memory_release(ORB_MEMORY_WORKSPACE, reserve);
    orb_app_voice_spoken_t *view = orb_app_voice_spoken_create(false, NULL, NULL);
    assert(view);
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    reserve = stats.budget - stats.used;
    assert(orb_memory_reserve(ORB_MEMORY_WORKSPACE, reserve));
    assert(!orb_app_voice_spoken_present(view, s_display, true, false, "Retry after admission", -1));
    assert(!orb_app_voice_spoken_root(view));
    orb_memory_release(ORB_MEMORY_WORKSPACE, reserve);
    assert(orb_app_voice_spoken_present(view, s_display, true, false, "Retry after admission", -1));
    orb_app_voice_spoken_destroy(view);
    quiescent();
}

/* The compatibility wrapper keeps the live callback and top-layer order. */
static void shell_wrapper(void)
{
    unsigned first = 0, second = 0;
    orb_sound_overlay_t overlay;
    orb_sound_overlay_init(&overlay);
    overlay.stop = stop; overlay.stop_ctx = &first;
    orb_sound_overlay_speech(&overlay, s_display, true, false, "Shell caption", ORB_AGENT_DOT);
    assert(orb_sound_overlay_has_speech(&overlay));
    assert(orb_sound_overlay_stop(&overlay) && first == 1);
    overlay.stop_ctx = &second;
    assert(orb_sound_overlay_stop(&overlay) && first == 1 && second == 1);
    overlay.stop = NULL;
    assert(!orb_sound_overlay_stop(&overlay) && second == 1);
    orb_sound_overlay_media(&overlay, s_display, 50);
    lv_obj_t *above = lv_obj_create(lv_display_get_layer_top(s_display));
    unsigned index = lv_obj_get_index(overlay.volume);
    orb_sound_overlay_speech(&overlay, s_display, false, false, NULL, -1);
    assert(lv_obj_get_index(overlay.volume) == index - 1);
    assert(lv_obj_get_index(above) > lv_obj_get_index(overlay.volume));
    lv_obj_delete(above);
    orb_sound_overlay_destroy(&overlay);
    quiescent();
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    s_display = sim_display_create();
    assert(s_display);
    orb_ui_init(NULL);
    assert(orb_screen_descriptor_valid(orb_app_voice_spoken_descriptor(), NULL));
    orb_app_voice_spoken_t *warm = orb_app_voice_spoken_create(false, NULL, NULL);
    assert(warm && orb_app_voice_spoken_present(warm, s_display, true, false, "Warm caption", -1));
    orb_app_voice_spoken_destroy(warm);
    advance(1600);
    s_workspace_baseline = used();
    s_timer_baseline = timers();
    lifecycle();
    presentation();
    preview();
    admission();
    shell_wrapper();
    orb_app_voice_spoken_destroy(NULL);
    printf("app_voice_spoken: descriptor lifecycle, copied captions, physical stop, preview and admission passed\n");
    return 0;
}
