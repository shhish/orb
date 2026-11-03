/* Weather uses the same lifecycle driver as every production descriptor.
 * Assertions read semantic text and queries, never kit child indices. */
#include "orb_app_weather.h"
#include "orb_app_weather_registry.h"
#include "orb_fonts.h"
#include "orb_kit.h"
#include "orb_screen_lifecycle.h"
#include "orb_time.h"
#include "orb_weather_host.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_intents, s_timer_baseline;
static size_t s_workspace_baseline;
static int32_t s_saved_scroll;
static uint32_t s_now = 10000;

static uint32_t tick(void)
{
    return s_now;
}

static unsigned timer_count(void)
{
    unsigned count = 0;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer)) count++;
    return count;
}

static size_t workspace_used(void)
{
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    return stats.used;
}

static bool intent(void *context, const orb_intent_t *value)
{
    (void)context; (void)value;
    s_intents++;
    return true;
}

static void quiescent(void)
{
    assert(workspace_used() == s_workspace_baseline);
    assert(timer_count() == s_timer_baseline);
}

static lv_obj_t *find_text(lv_obj_t *root, const char *value)
{
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), value)) return root;
    const char *rim = orb_kit_rim_text_get(root);
    if (rim && !strcmp(rim, value)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_text(lv_obj_get_child(root, i), value);
        if (found) return found;
    }
    return NULL;
}

static bool has_text(orb_screen_instance_t *instance, const char *value)
{
    return find_text(orb_screen_root(instance), value) != NULL;
}

static orb_weather_screen_model_t current_model(void)
{
    orb_weather_screen_model_t model = {.online = true, .synced = true, .supported = true,
        .configured = true, .available = true, .temperature = 72.4f, .high = 78, .low = 61,
        .condition = ORB_WEATHER_PARTLY_CLOUDY, .is_day = true, .hourly_count = 6};
    strcpy(model.place, "Columbia, MD");
    strcpy(model.updated, "1:41 PM");
    static const char *times[] = {"3 PM", "4 PM", "5 PM", "6 PM", "7 PM", "8 PM"};
    static const orb_weather_condition_t conditions[] = {ORB_WEATHER_PARTLY_CLOUDY, ORB_WEATHER_CLOUDY,
        ORB_WEATHER_RAIN, ORB_WEATHER_THUNDER, ORB_WEATHER_CLOUDY, ORB_WEATHER_CLEAR};
    for (unsigned i = 0; i < 6; i++) {
        snprintf(model.hourly[i].time, sizeof(model.hourly[i].time), "%s", times[i]);
        model.hourly[i].temperature = 73.f - (float)i * 2;
        model.hourly[i].condition = conditions[i];
        model.hourly[i].is_day = i < 3;
    }
    return model;
}

static int32_t maximum(lv_obj_t *view)
{
    lv_obj_update_layout(view);
    return LV_MAX(0, lv_obj_get_scroll_y(view) + lv_obj_get_scroll_bottom(view));
}

static void exercise(orb_screen_instance_t *instance)
{
    lv_obj_t *view = orb_app_weather_scroll(instance);
    assert(view && maximum(view) > 0);
    s_saved_scroll = LV_MIN(24, maximum(view));
    lv_obj_scroll_to_y(view, s_saved_scroll, LV_ANIM_OFF);
    unsigned before = s_intents;
    /* No local detail and no network action: Back always leaves the screen. */
    assert(orb_screen_back(instance) == ORB_BACK_LEAVE_SCREEN);
    orb_rim_binding_t binding;
    assert(orb_screen_descriptor(instance)->ops->rim_binding(instance, &binding));
    assert(binding.kind == ORB_RIM_SCROLL && binding.maximum == maximum(view));
    assert(s_intents == before);
}

static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    assert(orb_screen_alive(instance));
    assert(!lv_obj_is_clickable(orb_screen_root(instance)));
    orb_screen_chrome_t chrome;
    orb_screen_chrome(instance, &chrome);
    assert(!strcmp(chrome.title, "Weather"));
    assert(has_text(instance, "WEATHER")); /* Rim titles store capitals. */
    lv_obj_t *view = orb_app_weather_scroll(instance);
    assert(view && lv_obj_is_scrollable(view));
    if (phase <= ORB_LIFECYCLE_IDEMPOTENT) {
        assert(orb_app_weather_view(instance) == ORB_WEATHER_VIEW_CURRENT);
        assert(has_text(instance, "72°") && has_text(instance, "Partly cloudy"));
        assert(has_text(instance, "H:78° L:61°") && has_text(instance, "3 PM") && has_text(instance, "63°"));
        assert(has_text(instance, "Columbia, MD") && has_text(instance, "Updated 1:41 PM"));
        assert(!orb_app_weather_hourly_rows(instance));
    } else if (phase <= ORB_LIFECYCLE_RESTORED) {
        assert(orb_app_weather_view(instance) == ORB_WEATHER_VIEW_STALE);
        assert(has_text(instance, "−18°") && has_text(instance, "Snow"));
        assert(has_text(instance, "Not updated since Tue 9:05 AM"));
        assert(has_text(instance, "Llanfairpwllgwyngyll, Isle of Anglesey, Wales"));
    } else if (phase == ORB_LIFECYCLE_REMOVED_ID) {
        assert(orb_app_weather_view(instance) == ORB_WEATHER_VIEW_UNAVAILABLE);
        assert(has_text(instance, "Weather unavailable") && has_text(instance, "Your hub will try again."));
        assert(!has_text(instance, "72°"));
    } else {
        assert(orb_app_weather_view(instance) == ORB_WEATHER_VIEW_OFFLINE);
        assert(has_text(instance, "Offline"));
    }
    if (phase >= ORB_LIFECYCLE_RESTORED)
        assert(lv_obj_get_scroll_y(view) == orb_screen_clamp_scroll(s_saved_scroll, maximum(view)));
}

static void classification(void)
{
    orb_weather_screen_model_t model = current_model();
    assert(orb_app_weather_classify(&model) == ORB_WEATHER_VIEW_CURRENT);
    model.stale = true;
    assert(orb_app_weather_classify(&model) == ORB_WEATHER_VIEW_STALE);
    model.stale = false; model.online = false;
    assert(orb_app_weather_classify(&model) == ORB_WEATHER_VIEW_STALE);
    model = (orb_weather_screen_model_t){.online = true, .synced = true, .supported = true, .configured = true};
    assert(orb_app_weather_classify(&model) == ORB_WEATHER_VIEW_LOADING);
    model.stale = true;
    assert(orb_app_weather_classify(&model) == ORB_WEATHER_VIEW_UNAVAILABLE);
    model = (orb_weather_screen_model_t){.online = true, .synced = true, .supported = true};
    assert(orb_app_weather_classify(&model) == ORB_WEATHER_VIEW_NOT_CONFIGURED);
    model.synced = false;
    assert(orb_app_weather_classify(&model) == ORB_WEATHER_VIEW_LOADING);
    model = (orb_weather_screen_model_t){.online = true, .synced = true};
    assert(orb_app_weather_classify(&model) == ORB_WEATHER_VIEW_UNAVAILABLE);
    model.online = false;
    assert(orb_app_weather_classify(&model) == ORB_WEATHER_VIEW_OFFLINE);
}

/* Live revisions keep the composition and its label objects: no rebuild, no
 * scroll jump. A state change is the only reason to compose again. */
static void live_updates(orb_screen_host_t *host)
{
    orb_weather_screen_model_t model = current_model();
    orb_screen_model_t view_model = {&model, sizeof(model)};
    orb_screen_update_t update = {.revisions.model = 1, .model = &view_model};
    orb_screen_instance_t instance = {0};
    assert(orb_screen_create(&instance, host, orb_app_weather_descriptor(), lv_screen_active(), NULL, false));
    assert(orb_screen_update(&instance, &update));
    unsigned builds = orb_app_weather_builds(&instance);
    lv_obj_t *number = find_text(orb_screen_root(&instance), "72°");
    lv_obj_t *hour = find_text(orb_screen_root(&instance), "73°");
    assert(number && hour);
    assert(orb_screen_update(&instance, &update));
    assert(orb_app_weather_builds(&instance) == builds);

    model.temperature = 73.2f; model.high = 79; model.hourly[0].temperature = 75;
    model.condition = ORB_WEATHER_CLOUDY; strcpy(model.updated, "2:01 PM");
    update.revisions.model = 2;
    assert(orb_screen_update(&instance, &update));
    assert(orb_app_weather_builds(&instance) == builds);
    assert(find_text(orb_screen_root(&instance), "73°") == number);
    assert(find_text(orb_screen_root(&instance), "75°") == hour);
    assert(has_text(&instance, "Cloudy") && has_text(&instance, "H:79° L:61°") && has_text(&instance, "Updated 2:01 PM"));

    model.stale = true;
    update.revisions.model = 3;
    assert(orb_screen_update(&instance, &update));
    assert(orb_app_weather_builds(&instance) == builds);
    assert(orb_app_weather_view(&instance) == ORB_WEATHER_VIEW_STALE);
    assert(find_text(orb_screen_root(&instance), "73°") == number);
    assert(has_text(&instance, "Not updated since 2:01 PM"));

    model.stale = false; model.hourly_count = 2;
    update.revisions.model = 4;
    assert(orb_screen_update(&instance, &update));
    assert(orb_app_weather_builds(&instance) == builds + 1);
    assert(!has_text(&instance, "5 PM"));

    model = (orb_weather_screen_model_t){.online = true, .synced = true, .supported = true, .configured = true};
    strcpy(model.place, "Columbia, MD");
    update.revisions.model = 5;
    assert(orb_screen_update(&instance, &update));
    assert(orb_app_weather_view(&instance) == ORB_WEATHER_VIEW_LOADING);
    assert(has_text(&instance, "Loading weather") && has_text(&instance, "Columbia, MD"));
    assert(s_intents == 0);
    orb_screen_destroy(&instance);
    quiescent();
}

/* A stale reading always says so, even when the hub gave no update time. */
static void stale_notice(orb_screen_host_t *host)
{
    orb_weather_screen_model_t model = current_model();
    model.updated[0] = '\0';
    orb_screen_model_t view_model = {&model, sizeof(model)};
    orb_screen_update_t update = {.revisions.model = 1, .model = &view_model};
    orb_screen_instance_t instance = {0};
    assert(orb_screen_create(&instance, host, orb_app_weather_descriptor(), lv_screen_active(), NULL, false));
    assert(orb_screen_update(&instance, &update));
    assert(!has_text(&instance, "Not up to date"));
    model.stale = true;
    update.revisions.model = 2;
    assert(orb_screen_update(&instance, &update));
    assert(orb_app_weather_view(&instance) == ORB_WEATHER_VIEW_STALE && has_text(&instance, "Not up to date"));
    model.stale = false;
    update.revisions.model = 3;
    assert(orb_screen_update(&instance, &update));
    assert(!has_text(&instance, "Not up to date") && has_text(&instance, "72°"));
    orb_screen_destroy(&instance);
    quiescent();
}

/* AX3 restacks the strip into rows and scrolls, rather than clipping. */
static void accessibility_sizes(orb_screen_host_t *host)
{
    assert(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    orb_weather_screen_model_t model = current_model();
    orb_screen_model_t view_model = {&model, sizeof(model)};
    orb_screen_update_t update = {.revisions.model = 1, .model = &view_model};
    orb_screen_instance_t instance = {0};
    assert(orb_screen_create(&instance, host, orb_app_weather_descriptor(), lv_screen_active(), NULL, false));
    assert(orb_screen_update(&instance, &update));
    assert(orb_app_weather_hourly_rows(&instance));
    lv_obj_t *view = orb_app_weather_scroll(&instance);
    assert(maximum(view) > 0);
    /* Every label keeps its complete semantic text: wrapping, never ellipsis. */
    lv_obj_t *place = find_text(orb_screen_root(&instance), "Columbia, MD");
    assert(place && lv_label_get_long_mode(place) == LV_LABEL_LONG_MODE_WRAP);
    orb_screen_destroy(&instance);
    assert(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
}

static void projection(void)
{
    orb_model_snapshot_t *model = calloc(1, sizeof(*model));
    assert(model);
    model->online = model->synced = model->weather_supported = model->weather_valid = true;
    model->weather = (orb_weather_snapshot_t){.configured = true, .available = true, .unit = ORB_TEMPERATURE_F,
        .temperature = 72, .high = 78, .low = 61, .condition = ORB_WEATHER_CLEAR, .is_day = true,
        .updated_at = 1793194860, .hourly_count = 2};
    strcpy(model->weather.place, "Columbia, MD");
    model->weather.hourly[0] = (orb_weather_hour_t){.hour = 1793199600, .temperature = 71, .condition = ORB_WEATHER_CLEAR};
    model->weather.hourly[1] = (orb_weather_hour_t){.hour = 1793242800, .temperature = 60, .condition = ORB_WEATHER_RAIN};
    orb_weather_screen_model_t out;
    orb_time_set_clock_format(false);
    orb_weather_host_project(model, &out);
    assert(out.available && out.online && out.synced && out.supported && out.hourly_count == 2);
    assert(!strcmp(out.place, "Columbia, MD") && !strcmp(out.updated, "1:41 PM"));
    assert(!strcmp(out.hourly[0].time, "3 PM") && !strcmp(out.hourly[1].time, "3 AM"));
    orb_time_set_clock_format(true);
    orb_weather_host_project(model, &out);
    assert(!strcmp(out.updated, "13:41") && !strcmp(out.hourly[0].time, "15"));
    /* Without a reading nothing but the state and place reaches the screen. */
    model->weather = (orb_weather_snapshot_t){.configured = true, .unit = ORB_TEMPERATURE_F};
    strcpy(model->weather.place, "Columbia, MD");
    orb_weather_host_project(model, &out);
    assert(!out.available && !out.updated[0] && !out.hourly_count && !strcmp(out.place, "Columbia, MD"));
    free(model);
}

int main(void)
{
    setenv("TZ", "UTC", 1);
    lv_init();
    lv_tick_set_cb(tick);
    assert(sim_display_create());
    orb_ui_init(NULL);
    orb_time_init();
    orb_nav_t nav;
    orb_nav_init(&nav, ORB_WEATHER_SCREEN_ID);
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.nav = &nav, .intent = intent});

    orb_weather_screen_model_t first = current_model();
    orb_weather_screen_model_t changed = current_model();
    changed.stale = true; changed.temperature = -18.2f; changed.high = -12; changed.low = -23;
    changed.condition = ORB_WEATHER_SNOW; changed.is_day = false;
    strcpy(changed.updated, "Tue 9:05 AM");
    strcpy(changed.place, "Llanfairpwllgwyngyll, Isle of Anglesey, Wales");
    orb_weather_screen_model_t removed = {.online = true, .synced = true, .supported = true,
        .configured = true, .stale = true};
    strcpy(removed.place, "Columbia, MD");
    orb_screen_model_t first_model = {&first, sizeof(first)};
    orb_screen_model_t changed_model = {&changed, sizeof(changed)};
    orb_screen_model_t removed_model = {&removed, sizeof(removed)};
    orb_screen_update_t initial = {.revisions.model = 1, .model = &first_model};
    orb_screen_update_t next = {.revisions.model = 2, .model = &changed_model};
    orb_screen_update_t gone = {.revisions.model = 3, .model = &removed_model};
    orb_screen_update_t empty = {.revisions.model = 4};

    orb_screen_instance_t warm = {0};
    assert(orb_screen_create(&warm, &host, orb_app_weather_descriptor(), lv_screen_active(), NULL, false));
    assert(orb_screen_update(&warm, &initial));
    orb_screen_destroy(&warm);
    s_timer_baseline = timer_count();
    s_workspace_baseline = workspace_used();

    classification();
    orb_lifecycle_case_t test = {.descriptor = orb_app_weather_descriptor(),
        .initial = &initial, .changed = &next, .removed = &gone, .empty = &empty,
        .exercise = exercise, .verify = verify, .quiescent = quiescent};
    orb_test_screen_lifecycle(&host, &test);
    live_updates(&host);
    stale_notice(&host);
    accessibility_sizes(&host);
    projection();

    orb_screen_registry_t registry;
    orb_screen_registry_init(&registry);
    const char *reason = NULL;
    assert(orb_screen_registry_add(&registry, orb_app_weather_manifest(), &reason));
    assert(orb_screen_registry_find(&registry, ORB_WEATHER_SCREEN_ID) == orb_app_weather_descriptor());
    /* Previews render the same composition and emit nothing. */
    orb_screen_instance_t preview = {0};
    assert(orb_screen_create(&preview, &host, orb_app_weather_descriptor(), lv_screen_active(), NULL, true));
    assert(orb_screen_update(&preview, &initial));
    assert(has_text(&preview, "72°"));
    orb_screen_destroy(&preview);
    quiescent();
    assert(s_intents == 0);
    puts("Weather: lifecycle, states, live in-place updates, AX3 reflow, projection and registry passed");
    return 0;
}
