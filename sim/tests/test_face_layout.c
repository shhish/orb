/* FaceOrb 2.0 shell integration: kit geometry at every type/weight, truthful
 * unavailable states and equality of partial versus complete redraws. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "orb_faces.h"
#include "orb_fonts.h"
#include "orb_home.h"
#include "orb_kit.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "sim_display.h"

static unsigned s_checks;
static uint32_t s_now;
static lv_display_t *s_display;
static orb_shell_t *s_shell;
static uint8_t s_before[SIM_D * SIM_D * 3], s_full[SIM_D * SIM_D * 3];
#define CHECK(value) do { s_checks++; if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); exit(1); \
} } while (0)
static uint32_t tick(void) { return s_now; }
static void settle(void)
{
    for (unsigned i = 0; i < 60; i++) {
        s_now += 16; orb_shell_poll(s_shell); lv_timer_handler();
    }
    lv_refr_now(s_display);
}
static lv_obj_t *find(lv_obj_t *root, const char *text)
{
    if (!lv_obj_is_visible(root)) return NULL;
    const char *rim = orb_kit_rim_text_get(root);
    if (rim && !strcasecmp(rim, text)) return root;
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *control_at(lv_obj_t *root, int cx, int cy, int diameter)
{
    if (!lv_obj_is_visible(root)) return NULL;
    lv_area_t area; lv_obj_get_coords(root, &area);
    if (lv_obj_is_clickable(root) && !lv_obj_is_scrollable(root) &&
        lv_obj_get_width(root) == diameter && lv_obj_get_height(root) == diameter &&
        abs(area.x1 + diameter / 2 - cx) <= 1 && abs(area.y1 + diameter / 2 - cy) <= 1) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = control_at(lv_obj_get_child(root, i), cx, cy, diameter);
        if (found) return found;
    }
    return NULL;
}
static void equal_redraw(void)
{
    sim_display_rgb(s_before);
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(s_display); sim_display_rgb(s_full);
    CHECK(!memcmp(s_before, s_full, sizeof(s_full)));
}
static void waiting(bool enabled)
{
    orb_model_snapshot_t model; CHECK(orb_model_snapshot(&model));
    for (unsigned i = 0; i < model.card_count; ++i) CHECK(orb_model_cancel_card(model.cards[i].id));
    if (!enabled || !model.session_count) return;
    orb_card_t card = {.id = "face-awaiting", .kind = ORB_CARD_APPROVAL, .title = "Run?",
        .options = {{.id = "deny", .label = "Deny", .role = ORB_OPTION_CANCEL},
            {.id = "allow", .label = "Allow", .role = ORB_OPTION_DEFAULT}}, .options_count = 2};
    strcpy(card.session, model.sessions[0].id); CHECK(orb_model_upsert_card(&card));
}

static void usage_contract(void)
{
    orb_model_snapshot_t model = {.online = true, .usage = {.count = 4}, .session_count = 3};
    model.usage.windows[0] = (orb_usage_window_t){.provider = ORB_AGENT_CLAUDE, .label = "Sonnet", .used_percent = 91};
    model.usage.windows[1] = (orb_usage_window_t){.provider = ORB_AGENT_CLAUDE, .label = "week", .used_percent = 37.6};
    model.usage.windows[2] = (orb_usage_window_t){.provider = ORB_AGENT_CODEX, .label = "week", .used_percent = 23.4};
    model.usage.windows[3] = (orb_usage_window_t){.provider = ORB_AGENT_CLAUDE, .label = "5 hours", .used_percent = 19.6};
    model.sessions[0].state = ORB_SESSION_BUSY;
    model.sessions[1].state = ORB_SESSION_BUSY;
    model.sessions[2].state = ORB_SESSION_IDLE;
    orb_faces_snapshot_t data = {0};
    orb_faces_snapshot_model(&data, &model);
    CHECK(data.online && data.usage_percent[0] == 20 && data.usage_percent[1] == 23 && data.working == 2);
    /* An unavailable primary window falls back to the all-model week, never
     * whichever model-specific quota happened to arrive first. */
    model.usage.windows[3].stale = true;
    orb_faces_snapshot_model(&data, &model);
    CHECK(data.usage_percent[0] == 38 && data.usage_percent[1] == 23);
    model.usage.windows[1].stale = true;
    model.usage.windows[0].stale = true;
    model.usage.windows[2].used_percent = NAN;
    orb_faces_snapshot_model(&data, &model);
    CHECK(data.usage_percent[0] == 20 && data.usage_stale[0] && data.usage_percent[1] == -1);
    model.usage.unknown[0] = true;
    model.usage.windows[3].stale = false;
    orb_faces_snapshot_model(&data, &model);
    CHECK(data.usage_percent[0] == 20 && data.usage_stale[0]);
    model.online = false;
    model.usage.windows[2].used_percent = 23.4;
    orb_faces_snapshot_model(&data, &model);
    CHECK(!data.online && data.usage_percent[0] == 20 && data.usage_stale[0]);
    CHECK(data.usage_percent[1] == 23 && data.usage_stale[1]);
    model.usage.count = 0;
    orb_faces_snapshot_model(&data, &model);
    CHECK(data.usage_percent[0] == -1 && data.usage_percent[1] == -1);
    orb_faces_snapshot_model(&data, NULL);
    CHECK(!data.online && data.usage_percent[0] == -1 && data.usage_percent[1] == -1 && !data.working);
}

static orb_faces_t *data_face(orb_faces_snapshot_t *data)
{
    orb_faces_t *faces = orb_faces_create(lv_screen_active(), &(orb_faces_config_t){.preview = true});
    CHECK(faces);
    data->weather_override = true;
    orb_faces_update(faces, data);
    return faces;
}

static void weather_contract(void)
{
    orb_faces_snapshot_t data = {.now = 1793208480, .online = true,
        .weather = {.configured = true, .available = true, .unit = ORB_TEMPERATURE_C,
            .temperature = -12, .low = -20, .high = 0, .condition = ORB_WEATHER_CLEAR, .is_day = false}};
    orb_faces_t *faces = data_face(&data);
    orb_face_complication_t value;
    CHECK(orb_faces_complication(faces, ORB_FACE_WEATHER, &value));
    CHECK(!strcmp(value.text, "-12°") && !strcmp(value.glyph, ORB_ICON_MOON) && value.percent == 40);
    data.weather.unit = ORB_TEMPERATURE_F; data.weather.temperature = 10;
    data.weather.low = 0; data.weather.high = 20; data.weather.is_day = true;
    orb_faces_update(faces, &data);
    CHECK(orb_faces_complication(faces, ORB_FACE_WEATHER, &value));
    CHECK(!strcmp(value.text, "10°") && !strcmp(value.glyph, ORB_ICON_SUN) && value.percent == 50);
    /* Retained observations become stale on fetch failure and link loss.
     * A glance must not pass either value off as current weather: polish
     * #14 keeps the last reading and range, marked stale by the clock glyph. */
    data.weather.stale = true; orb_faces_update(faces, &data);
    CHECK(orb_faces_complication(faces, ORB_FACE_WEATHER, &value));
    CHECK(!strcmp(value.text, "10°") && value.stale && !strcmp(value.glyph, ORB_ICON_CLOCK) && value.percent == 50);
    data.weather.stale = false; data.online = false; orb_faces_update(faces, &data);
    CHECK(orb_faces_complication(faces, ORB_FACE_WEATHER, &value));
    CHECK(!strcmp(value.text, "10°") && value.stale && value.percent == 50);
    data.weather.available = false; orb_faces_update(faces, &data);
    CHECK(orb_faces_complication(faces, ORB_FACE_WEATHER, &value));
    CHECK(!strcmp(value.text, "—") && !value.stale && value.percent == -1);
    data.weather.available = true;
    data.online = true; data.weather.available = false;
    strcpy(data.weather.label, "Loading weather"); orb_faces_update(faces, &data);
    CHECK(find(orb_faces_root(faces), "Loading weather"));
    data.weather.stale = true; strcpy(data.weather.label, "Weather unavailable");
    orb_faces_update(faces, &data); CHECK(find(orb_faces_root(faces), "Weather unavailable"));
    /* The Orb row names an absent provider; compact slots retain their glyph. */
    data.weather.configured = false; orb_faces_update(faces, &data);
    CHECK(find(orb_faces_root(faces), "Weather not set"));
    orb_faces_destroy(faces);
}

static void aggregate_contract(void)
{
    orb_faces_snapshot_t data = {.now = 1793208480, .online = true,
        .usage_percent = {61, 24}, .home_on = 3, .home_total = 8, .battery_percent = -1};
    orb_faces_t *faces = data_face(&data);
    orb_face_complication_t value;
    CHECK(orb_faces_complication(faces, ORB_FACE_CLAUDE, &value));
    CHECK(value.percent == 61 && value.target == ORB_FACES_OPEN_CLAUDE);
    CHECK(orb_faces_complication(faces, ORB_FACE_CODEX, &value));
    CHECK(value.percent == 24 && value.target == ORB_FACES_OPEN_CODEX);
    CHECK(orb_faces_complication(faces, ORB_FACE_HOME, &value));
    CHECK(value.percent == 37 && !strcmp(value.text, "3 on"));
    CHECK(orb_faces_complication(faces, ORB_FACE_BATTERY, &value));
    CHECK(value.percent == -1 && !strcmp(value.text, "—"));
    static const int percentages[] = {0, 5, 20, 50, 80, 100};
    static const char *const glyphs[] = {ORB_ICON_BATTERY_EMPTY, ORB_ICON_BATTERY_LOW,
        ORB_ICON_BATTERY_LOW, ORB_ICON_BATTERY_MEDIUM, ORB_ICON_BATTERY_HIGH, ORB_ICON_BATTERY_FULL};
    for (unsigned i = 0; i < sizeof(percentages) / sizeof(percentages[0]); i++) {
        data.battery_percent = percentages[i]; orb_faces_update(faces, &data);
        CHECK(orb_faces_complication(faces, ORB_FACE_BATTERY, &value));
        CHECK(value.percent == percentages[i] && !strcmp(value.glyph, glyphs[i]));
    }
    data.charging = true; orb_faces_update(faces, &data);
    CHECK(orb_faces_complication(faces, ORB_FACE_BATTERY, &value));
    CHECK(!strcmp(value.glyph, ORB_ICON_BATTERY_CHARGING));
    data.home_total = 0; orb_faces_update(faces, &data);
    CHECK(orb_faces_complication(faces, ORB_FACE_HOME, &value));
    CHECK(value.percent == -1 && !strcmp(value.text, "—"));
    data.home_total = 8; data.online = false; orb_faces_update(faces, &data);
    CHECK(orb_faces_complication(faces, ORB_FACE_CLAUDE, &value));
    CHECK(value.percent == 61 && value.stale && !strcmp(value.text, "61"));
    CHECK(orb_faces_complication(faces, ORB_FACE_CODEX, &value));
    CHECK(value.percent == 24 && value.stale && !strcmp(value.text, "24"));
    CHECK(orb_faces_complication(faces, ORB_FACE_HOME, &value));
    CHECK(value.percent == 37 && value.stale && !strcmp(value.text, "3 on"));
    data.online = true; data.home_stale = true; data.usage_stale[0] = true; orb_faces_update(faces, &data);
    CHECK(orb_faces_complication(faces, ORB_FACE_HOME, &value) && value.stale);
    CHECK(orb_faces_complication(faces, ORB_FACE_CLAUDE, &value) && value.stale);
    CHECK(orb_faces_complication(faces, ORB_FACE_CODEX, &value) && !value.stale);
    /* Local battery telemetry remains useful with the hub disconnected. */
    CHECK(orb_faces_complication(faces, ORB_FACE_BATTERY, &value)); CHECK(value.percent == 100);
    orb_faces_destroy(faces);
}

static void civil_time_contract(void)
{
    const char *old = getenv("TZ");
    char *saved = old ? strdup(old) : NULL;
    CHECK(setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1) == 0); tzset();
    /* Spring gap and autumn repeated hour are actual Unix instants: the
     * component must delegate conversion to libc rather than add an offset. */
    static const struct { time_t now; const char *clock, *day; } cases[] = {
        {1772953140, "01:59", "08"}, {1772953200, "03:00", "08"},
        {1793512740, "01:59", "01"}, {1793512800, "01:00", "01"},
        {1709208000, "07:00", "29"},
    };
    orb_faces_snapshot_t data = {.now = cases[0].now};
    orb_faces_t *faces = data_face(&data);
    orb_face_complication_t value;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        data.now = cases[i].now; orb_faces_update(faces, &data);
        CHECK(find(orb_faces_root(faces), cases[i].clock));
        CHECK(orb_faces_complication(faces, ORB_FACE_DATE, &value));
        CHECK(!strcmp(value.text, cases[i].day));
    }
    CHECK(value.percent == 100); /* 29 February in a leap year. */
    data.now = 60; orb_faces_update(faces, &data);
    CHECK(find(orb_faces_root(faces), "Set time"));
    CHECK(orb_faces_complication(faces, ORB_FACE_DATE, &value));
    CHECK(!strcmp(value.text, "—") && value.percent == -1);
    orb_faces_destroy(faces);
    if (saved) { CHECK(setenv("TZ", saved, 1) == 0); free(saved); }
    else CHECK(unsetenv("TZ") == 0);
    tzset();
}

static size_t node_ids(lv_obj_t *root, uintptr_t *nodes, size_t count)
{
    CHECK(count < 4096);
    nodes[count++] = (uintptr_t)root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++)
        count = node_ids(lv_obj_get_child(root, i), nodes, count);
    return count;
}

static lv_obj_t *arc_value(lv_obj_t *root, lv_color_t colour, int value, int diameter)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (lv_obj_check_type(root, &lv_arc_class) &&
        lv_color_eq(lv_obj_get_style_arc_color(root, LV_PART_INDICATOR), colour) &&
        lv_arc_get_value(root) == value && (!diameter || lv_obj_get_width(root) == diameter)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = arc_value(lv_obj_get_child(root, i), colour, value, diameter);
        if (found) return found;
    }
    return NULL;
}

static void editor_contract(void)
{
    static const orb_faces_mode_t modes[] = {ORB_FACES_GALLERY, ORB_FACES_COLOUR, ORB_FACES_SLOTS};
    static uintptr_t before[4096], after[4096];
    for (unsigned kind = 0; kind < ORB_FACE_COUNT; kind++) {
        for (unsigned mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++) {
            orb_faces_snapshot_t data = {.now = 1793208480, .clock = "17:28", .weekday = "Wednesday", .day = "28",
                .online = true, .usage_percent = {62, 62}, .home_on = 3, .home_total = 8,
                .battery_percent = 100, .working = 3,
                .weather = {.configured = true, .available = true, .unit = ORB_TEMPERATURE_F,
                    .temperature = 72, .low = 60, .high = 80, .condition = ORB_WEATHER_CLEAR,
                    .is_day = true, .label = "Sunny"}};
            orb_faces_t *faces = data_face(&data);
            orb_faces_prefs_t prefs; orb_faces_preferences(faces, &prefs);
            prefs.face = kind; prefs.meridian_slot[3] = ORB_FACE_HOME;
            orb_faces_set_preferences(faces, &prefs);
            orb_faces_set_mode(faces, ORB_FACES_GALLERY);
            orb_faces_set_mode(faces, modes[mode]);
            lv_obj_t *root = orb_faces_root(faces);
            size_t before_count = node_ids(root, before, 0);
            /* A source update must not retire a pressed editor control or
             * leave old quota/Home values in its retained artwork/handles. */
            data.now += 60; strcpy(data.clock, "17:29");
            data.usage_percent[0] = data.usage_percent[1] = 69;
            data.home_on = data.working = 2;
            data.weather.condition = ORB_WEATHER_RAIN; strcpy(data.weather.label, "Rain");
            orb_faces_update(faces, &data);
            size_t after_count = node_ids(root, after, 0);
            CHECK(before_count == after_count && !memcmp(before, after, before_count * sizeof(*before)));
            CHECK(!find(root, "62") && !find(root, "3 on") && !find(root, "3 working"));
            CHECK(!arc_value(root, ORB_KIT_CLAUDE, 620, 0));
            if (kind == ORB_FACE_ORB)
                CHECK(find(root, ORB_ICON_BATTERY_FULL));
            if (kind == ORB_FACE_ORB && modes[mode] == ORB_FACES_SLOTS)
                CHECK(find(root, "17:29") && find(root, "Rain"));
            if (kind != ORB_FACE_ACTIVITY || modes[mode] == ORB_FACES_SLOTS)
                CHECK(find(root, "69") && find(root, "2 on"));
            else CHECK(find(root, "2 working") && arc_value(root, ORB_KIT_CLAUDE, 690, 0));
            data.online = false; data.weather.stale = true;
            orb_faces_update(faces, &data);
            if (kind != ORB_FACE_ACTIVITY || modes[mode] == ORB_FACES_SLOTS)
                CHECK(find(root, "69") && find(root, "2 on"));
            CHECK(arc_value(root, ORB_KIT_CLAUDE, 690, 0));
            orb_faces_destroy(faces);
        }
    }
    /* Artwork uses each customized provider's real accent, not the default
     * Claude/Codex/Home palette reintroduced while scaling the three gauges. */
    for (unsigned mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++) {
        orb_faces_snapshot_t data = {.now = 1793208480, .online = true, .battery_percent = 60,
            .weather = {.configured = true, .available = true, .temperature = 72, .low = 60,
                .high = 80, .unit = ORB_TEMPERATURE_F, .condition = ORB_WEATHER_CLEAR, .is_day = true}};
        orb_faces_t *faces = data_face(&data);
        orb_faces_prefs_t prefs; orb_faces_preferences(faces, &prefs);
        prefs.face = ORB_FACE_ACTIVITY;
        prefs.activity_slot[0] = ORB_FACE_BATTERY;
        prefs.activity_slot[1] = ORB_FACE_DATE;
        prefs.activity_slot[2] = ORB_FACE_WEATHER;
        orb_faces_set_preferences(faces, &prefs);
        orb_faces_set_mode(faces, ORB_FACES_GALLERY);
        orb_faces_set_mode(faces, modes[mode]);
        /* Color's artwork is 88/256 (polish #17: 8px clear of the swatches). */
        int scale = modes[mode] == ORB_FACES_GALLERY ? 143 : modes[mode] == ORB_FACES_COLOUR ? 88 : 99;
        for (unsigned i = 0; i < 3; i++) {
            orb_face_complication_t source;
            CHECK(orb_faces_complication(faces, prefs.activity_slot[i], &source));
            int diameter = 2 * ((213 - 20 * (int)i) * scale / 256) + LV_MAX(2, 14 * scale / 256);
            CHECK(arc_value(orb_faces_root(faces), source.accent, source.percent * 10, diameter));
        }
        orb_faces_destroy(faces);
    }
}

static unsigned s_invalidations;
static void invalidated(lv_event_t *event)
{
    (void)event;
    s_invalidations++;
}

static void always_on_damage_contract(void)
{
    for (unsigned kind = 0; kind < ORB_FACE_COUNT; kind++) {
        orb_faces_snapshot_t data = {.now = 1793208480, .online = true,
            .usage_percent = {62, 38}, .home_on = 1, .home_total = 8, .battery_percent = 100};
        orb_faces_t *faces = data_face(&data);
        orb_faces_prefs_t prefs; orb_faces_preferences(faces, &prefs);
        prefs.face = kind; orb_faces_set_preferences(faces, &prefs);
        orb_faces_set_always_on(faces, true);
        lv_refr_now(s_display);
        lv_display_add_event_cb(s_display, invalidated, LV_EVENT_INVALIDATE_AREA, NULL);
        s_invalidations = 0;
        for (unsigned second = 1; second < 60; second++) {
            data.now++;
            orb_faces_update(faces, &data);
        }
        CHECK(!s_invalidations);
        data.now++;
        orb_faces_update(faces, &data);
        CHECK(s_invalidations > 0); /* The minute clock still advances. */
        CHECK(lv_display_remove_event_cb_with_user_data(s_display, invalidated, NULL) == 1);
        orb_faces_destroy(faces);
    }
}

int main(int argc, char **argv)
{
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); orb_glass_cache_prewarm(); CHECK(orb_model_init());
    if (argc == 3 && !strcmp(argv[1], "--qa-data")) {
        if (!strcmp(argv[2], "usage")) usage_contract();
        else if (!strcmp(argv[2], "weather")) weather_contract();
        else if (!strcmp(argv[2], "aggregates")) aggregate_contract();
        else if (!strcmp(argv[2], "civil-time")) civil_time_contract();
        else if (!strcmp(argv[2], "editors")) editor_contract();
        else if (!strcmp(argv[2], "always-on-damage")) always_on_damage_contract();
        else CHECK(false);
        printf("Face QA %s: %u checks passed\n", argv[2], s_checks);
        return 0;
    }
    usage_contract(); weather_contract(); aggregate_contract(); civil_time_contract(); editor_contract();
    always_on_damage_contract();
    orb_model_load_demo(); orb_home_load_demo(); orb_model_set_connection(true, true);
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL); CHECK(s_shell);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 100, .brightness = 70, .clock_time = "10:09", .clock_date = "Tuesday, October 27"};
    orb_shell_update(s_shell, &status); orb_shell_navigate(s_shell, ORB_SHELL_FACE); settle();
    for (unsigned size = 0; size < ORB_CONTENT_SIZE_COUNT; ++size) {
        for (unsigned bold = 0; bold < 2; ++bold) {
            CHECK(orb_fonts_set_content_size(size) == ORB_FONTS_OK);
            CHECK(orb_fonts_set_bold_text(bold) == ORB_FONTS_OK);
            orb_ui_set_reduce_transparency(size == ORB_CONTENT_SIZE_AX3);
            waiting(false); settle();
            lv_obj_t *root = orb_shell_root(s_shell);
            CHECK(find(root, "10:09") && find(root, "Tuesday"));
            CHECK(find(root, ORB_ICON_BATTERY_FULL));
            CHECK(control_at(root, 128, 316, ORB_KIT_DISC));
            CHECK(control_at(root, 233, 316, ORB_KIT_DISC));
            CHECK(control_at(root, 338, 316, ORB_KIT_DISC));
            CHECK(control_at(root, 233, 408, ORB_KIT_DISC_MINI));
            CHECK(!find(root, "Apps") && !find(root, "Connected"));
            equal_redraw();
            /* Pending cards no longer replace Calendar or complications. */
            waiting(true); settle();
            CHECK(find(root, "Tuesday") && find(root, ORB_ICON_BATTERY_FULL));
            CHECK(!find(root, ORB_ICON_HAND " 2 waiting"));
            equal_redraw();
            if (argc > 1) {
                char path[512]; snprintf(path, sizeof(path), "%s/face-%u-%u.png", argv[1], size, bold);
                CHECK(sim_display_save_png(path));
            }
        }
    }
    status.link = ORB_SHELL_LINK_OFFLINE; orb_model_set_connection(false, false);
    orb_shell_update(s_shell, &status); settle();
    CHECK(find(orb_shell_root(s_shell), "Tuesday") && find(orb_shell_root(s_shell), ORB_ICON_BATTERY_FULL));
    CHECK(find(orb_shell_root(s_shell), "—"));
    equal_redraw();
    status.battery_pct = -1; orb_shell_update(s_shell, &status); settle();
    CHECK(!find(orb_shell_root(s_shell), ORB_ICON_BATTERY_FULL));
    CHECK(control_at(orb_shell_root(s_shell), 338, 316, ORB_KIT_DISC));
    equal_redraw();
    orb_shell_destroy(s_shell); orb_glass_cache_clear();
    printf("FaceOrb shell layout: %u checks passed\n", s_checks);
    return 0;
}
