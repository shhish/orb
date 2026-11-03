/* The runtime is tested with deterministic one-step logic and a minimal fake
 * UI: this isolates task ownership, pacing and LittleFS failure behavior from
 * the separately tested real game rules and native LVGL presentation. */
#include "orb_games.h"
#include "orb_games_presentation.h"
#include "orb_apps.h"
#include "orb_fs.h"
#include "orb_game_logic.h"
#include "orb_game_view.h"
#include "orb_sound.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_checks;
#define CHECK(condition) do { ++s_checks; if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); } } while (0)

struct lv_timer_t { lv_timer_cb_t callback; void *context; bool alive; uint32_t period; int64_t reset_us; bool paused; };
struct lv_display_t { bool pending; };
struct lv_event_t { void *context; int code; };
struct lv_event_dsc_t { lv_event_cb_t callback; void *context; bool alive; };
static struct lv_display_t s_display;
static struct lv_event_dsc_t s_refresh_event;
static struct lv_event_dsc_t s_refresh_ready_event;
struct orb_game_view { orb_game_view_action_cb_t callback; void *context; bool alive; };
static struct lv_timer_t s_timer;
static struct orb_game_view s_view;
static lv_obj_t s_top;
static lv_obj_t s_original_screen = {true}, s_game_screen;
static lv_obj_t *s_active_screen = &s_original_screen;
static unsigned s_screen_creates, s_screen_deletes, s_screen_loads;
static bool s_screen_fail;
static unsigned s_lock_depth, s_allocations, s_frees, s_live_allocations;
static unsigned s_view_creates, s_view_destroys, s_timer_creates, s_timer_deletes;
static unsigned s_view_updates;
static bool s_event_fail;
static bool s_ready_event_fail;
static unsigned s_timer_ready;
static bool s_te_pacing = true;
static int64_t s_ui_now_us;
static int64_t s_view_work_us;
static unsigned s_reads, s_writes, s_prepares, s_actions, s_steps;
static unsigned s_sound_counts[ORB_SOUND_CUE_COUNT];
static bool s_alloc_fail, s_view_fail, s_timer_fail, s_reduce_motion;
static bool s_last_paused, s_last_failed, s_last_motion_valid, s_last_reduce_motion;
static float s_last_gx, s_last_gy;
static uint32_t s_last_high;
static orb_game_state_t s_last_model;
static esp_err_t s_prepare_error = ESP_OK, s_read_error = ESP_OK, s_write_error = ESP_OK;
static bool s_storage_ready = true;
typedef struct { uint8_t bytes[12]; size_t size; bool exists; } score_record_t;
static score_record_t s_records[ORB_GAME_COUNT], s_legacy_snake;
static struct { uint8_t bytes[ORB_KEEPSAKE_RECORD_SIZE]; size_t size; bool exists; } s_garden;
static bool s_complete_stage;
static float s_bead_flow;
static unsigned s_garden_reads, s_garden_writes;
static bool s_step_interleave;
static float s_interleaved_angle;
static void render_now(void);
static bool s_real_rewind_fixture;
void orb_test_real_init(orb_game_state_t *state, orb_game_kind_t kind, uint32_t seed);
void orb_test_real_step(orb_game_state_t *state, float ax, float ay, bool valid, bool reduced);

void *orb_test_calloc(size_t count, size_t bytes)
{
    ++s_allocations;
    if (s_alloc_fail) return NULL;
    void *p = calloc(count, bytes);
    if (p) ++s_live_allocations;
    return p;
}

void orb_test_free(void *p)
{
    if (p) {
        ++s_frees;
        CHECK(s_live_allocations > 0);
        --s_live_allocations;
    }
    free(p);
}

void lv_lock(void) { ++s_lock_depth; }
void lv_unlock(void) { CHECK(s_lock_depth > 0); --s_lock_depth; }
lv_obj_t *lv_layer_top(void) { CHECK(s_lock_depth > 0); return &s_top; }
lv_obj_t *lv_screen_active(void) { CHECK(s_lock_depth > 0); return s_active_screen; }
lv_obj_t *lv_obj_create(lv_obj_t *parent)
{
    CHECK(s_lock_depth > 0 && parent == NULL && !s_game_screen.alive);
    if (s_screen_fail) return NULL;
    ++s_screen_creates;
    s_game_screen.alive = true;
    return &s_game_screen;
}
void lv_screen_load(lv_obj_t *screen)
{
    CHECK(s_lock_depth > 0 && screen && screen->alive);
    CHECK(screen == &s_original_screen || screen == &s_game_screen);
    s_active_screen = screen;
    ++s_screen_loads;
}
void lv_obj_delete(lv_obj_t *object)
{
    CHECK(s_lock_depth > 0 && object == &s_game_screen && object->alive);
    CHECK(s_active_screen != object && !s_view.alive);
    object->alive = false;
    ++s_screen_deletes;
}
lv_timer_t *lv_timer_create(lv_timer_cb_t callback, uint32_t period, void *context)
{
    CHECK(s_lock_depth > 0);
    CHECK(period == 17);
    ++s_timer_creates;
    if (s_timer_fail) return NULL;
    CHECK(!s_timer.alive);
    s_timer = (struct lv_timer_t){callback, context, true, period, s_ui_now_us, false};
    return &s_timer;
}
void lv_timer_delete(lv_timer_t *timer)
{
    CHECK(s_lock_depth > 0 && timer == &s_timer && timer->alive);
    timer->alive = false;
    ++s_timer_deletes;
}
void lv_timer_ready(lv_timer_t *timer)
{
    CHECK(s_lock_depth > 0 && timer == &s_timer && timer->alive);
    ++s_timer_ready;
}
uint32_t lv_tick_get(void) { return (uint32_t)(s_ui_now_us / 1000); }
void lv_timer_set_period(lv_timer_t *timer, uint32_t period)
{
    CHECK(s_lock_depth > 0 && timer == &s_timer && timer->alive && period > 0);
    timer->period = period;
}
void lv_timer_reset(lv_timer_t *timer)
{
    CHECK(s_lock_depth > 0 && timer == &s_timer && timer->alive);
    timer->reset_us = s_ui_now_us;
}
void lv_timer_pause(lv_timer_t *timer)
{
    CHECK(s_lock_depth > 0 && timer == &s_timer && timer->alive);
    timer->paused = true;
}
void lv_timer_resume(lv_timer_t *timer)
{
    CHECK(s_lock_depth > 0 && timer == &s_timer && timer->alive);
    timer->paused = false;
}
bool orb_display_te_pacing_active(void) { return s_te_pacing; }
uint32_t orb_display_panel_period_us(void) { return 16920; }
void *lv_timer_get_user_data(lv_timer_t *timer) { CHECK(timer->alive); return timer->context; }
lv_display_t *lv_display_get_default(void) { return &s_display; }
bool orb_display_refresh_pending(void) { CHECK(s_lock_depth > 0); return s_display.pending; }
lv_event_dsc_t *lv_display_add_event_cb(lv_display_t *display, lv_event_cb_t callback,
                                       int filter, void *context)
{
    CHECK(s_lock_depth > 0 && display == &s_display);
    CHECK(filter == LV_EVENT_REFR_START || filter == LV_EVENT_REFR_READY);
    struct lv_event_dsc_t *event = filter == LV_EVENT_REFR_START ? &s_refresh_event : &s_refresh_ready_event;
    CHECK(!event->alive);
    if (s_event_fail || (filter == LV_EVENT_REFR_READY && s_ready_event_fail)) return NULL;
    *event = (struct lv_event_dsc_t){callback, context, true};
    return event;
}
uint32_t lv_display_remove_event_cb_with_user_data(lv_display_t *display,
                                                 lv_event_cb_t callback, void *context)
{
    CHECK(s_lock_depth > 0 && display == &s_display && s_refresh_event.alive);
    CHECK(callback == s_refresh_event.callback && context == s_refresh_event.context);
    s_refresh_event.alive = false;
    uint32_t removed = 1;
    if (s_refresh_ready_event.alive) {
        CHECK(callback == s_refresh_ready_event.callback && context == s_refresh_ready_event.context);
        s_refresh_ready_event.alive = false;
        ++removed;
    }
    return removed;
}
void *lv_event_get_user_data(lv_event_t *event) { return event->context; }
int lv_event_get_code(lv_event_t *event) { return event->code; }
bool orb_ui_reduce_motion(void) { CHECK(s_lock_depth > 0); return s_reduce_motion; }
bool orb_sound_play(orb_sound_cue_t cue)
{
    CHECK(s_lock_depth == 0 && (unsigned)cue < ORB_SOUND_CUE_COUNT);
    ++s_sound_counts[cue];
    return true;
}

orb_game_view_t *orb_game_view_create(lv_obj_t *parent, orb_game_kind_t kind,
                                     orb_game_view_action_cb_t callback, void *context)
{
    CHECK(s_lock_depth > 0 && parent == &s_game_screen && parent->alive);
    CHECK((unsigned)kind < ORB_GAME_COUNT);
    ++s_view_creates;
    if (s_view_fail) return NULL;
    CHECK(!s_view.alive);
    s_view = (struct orb_game_view){callback, context, true};
    return &s_view;
}
void orb_game_view_update(orb_game_view_t *view, const orb_game_state_t *state,
                          uint32_t high, bool paused, bool failed)
{
    CHECK(s_lock_depth > 0 && view == &s_view && view->alive);
    ++s_view_updates;
    s_ui_now_us += s_view_work_us;
    s_last_model = *state;
    s_last_high = high;
    s_last_paused = paused;
    s_last_failed = failed;
}
void orb_game_view_destroy(orb_game_view_t *view)
{
    CHECK(s_lock_depth > 0);
    if (!view) return;
    CHECK(view == &s_view && view->alive);
    view->alive = false;
    ++s_view_destroys;
}

const char *orb_game_id(orb_game_kind_t kind)
{
    static const char *const ids[] = {"marble", "beads", "orbit", "ring-snake", "pop"};
    return (unsigned)kind < ORB_GAME_COUNT ? ids[kind] : NULL;
}
void orb_game_init(orb_game_state_t *state, orb_game_kind_t kind, uint32_t seed)
{
    if (s_real_rewind_fixture) {
        orb_test_real_init(state, kind, seed);
        state->controls = true;
        state->game.marble.x = 20;
        state->game.marble.y = 0;
        state->game.marble.history_head = state->game.marble.history_count = 3;
        state->game.marble.history[0] = (orb_marble_sample_t){0, 0, 0, 0};
        state->game.marble.history[1] = (orb_marble_sample_t){160, 80, 0, 0};
        state->game.marble.history[2] = (orb_marble_sample_t){320, 0, 0, 0};
        return;
    }
    memset(state, 0, sizeof(*state));
    state->kind = kind;
    state->random = seed;
    state->lives = 3;
    state->started = true;
    if (kind == ORB_GAME_MARBLE) state->game.marble.stage_count = ORB_MARBLE_STAGES;
}
void orb_game_step(orb_game_state_t *state, float gx, float gy, bool valid, bool reduced)
{
    CHECK(s_lock_depth == 0);
    if (s_real_rewind_fixture) {
        ++s_steps;
        orb_test_real_step(state, gx, gy, valid, reduced);
        return;
    }
    if (!state->started || state->user_paused || state->game_over) return;
    if (state->kind == ORB_GAME_MARBLE && state->game.marble.stage_cleared) return;
    ++s_steps;
    ++state->ticks;
    if (s_step_interleave) {
        CHECK(state->kind == ORB_GAME_ORBIT);
        render_now();
        CHECK(fabsf(s_last_model.game.orbit.angle - s_interleaved_angle) < .00001f);
        state->game.orbit.angle += .12f;
        render_now();
        CHECK(fabsf(s_last_model.game.orbit.angle - s_interleaved_angle) < .00001f);
    }
    s_last_gx = gx; s_last_gy = gy;
    s_last_motion_valid = valid; s_last_reduce_motion = reduced;
    if (state->kind == ORB_GAME_BEADS) state->game.beads.flow_speed = s_bead_flow;
    if (s_complete_stage && state->kind == ORB_GAME_MARBLE) {
        s_complete_stage = false;
        state->game.marble.stage_cleared = true;
        state->game.marble.rewind_active = false;
        unsigned i = state->game.marble.keepsake_count;
        if (i == ORB_MARBLE_KEEPSAKES_MAX) {
            memmove(state->game.marble.keepsakes, state->game.marble.keepsakes + 1,
                (ORB_MARBLE_KEEPSAKES_MAX - 1) * sizeof(state->game.marble.keepsakes[0]));
            --i;
        } else ++state->game.marble.keepsake_count;
        state->game.marble.keepsakes[i] = (orb_marble_keepsake_t){
            1, (uint8_t)(state->game.marble.stage + 1), 80, 60};
        if (state->game.marble.stages_cleared < state->game.marble.stage + 1)
            state->game.marble.stages_cleared = (uint8_t)(state->game.marble.stage + 1);
    }
}
void orb_game_action(orb_game_state_t *state, orb_game_action_t action, float x, float y)
{
    CHECK(s_lock_depth == 0);
    (void)x; (void)y;
    if (action == ORB_GAME_START) { state->started = true; return; }
    if (action == ORB_GAME_GARDEN && state->kind == ORB_GAME_MARBLE) {
        if (!state->started || state->game_over || state->user_paused)
            state->game.marble.garden_open = !state->game.marble.garden_open;
        return;
    }
    if (action == ORB_GAME_REWIND_STOP) { state->game.marble.rewind_active = false; return; }
    if (action == ORB_GAME_STEER_STOP) {
        if (state->kind == ORB_GAME_SNAKE) state->game.snake.touch_held = false;
        if (state->kind == ORB_GAME_BEADS) state->game.beads.touch_held = false;
        return;
    }
    if (action == ORB_GAME_PAUSE) {
        state->user_paused = !state->user_paused;
        if (state->kind == ORB_GAME_MARBLE) state->game.marble.rewind_active = false;
        if (state->kind == ORB_GAME_SNAKE) state->game.snake.touch_held = false;
        if (state->kind == ORB_GAME_BEADS) state->game.beads.touch_held = false;
        return;
    }
    if (action == ORB_GAME_CONTROLS) { state->controls = !state->controls; return; }
    if (action == ORB_GAME_FINISH) {
        if (state->started && !state->game_over && state->user_paused) {
            state->game_over = true;
            state->user_paused = false;
        }
        return;
    }
    if (action == ORB_GAME_RESTART) {
        ++s_actions;
        bool controls = state->controls;
        orb_game_state_t saved = *state;
        orb_game_init(state, state->kind, state->random);
        state->controls = controls;
        if (state->kind == ORB_GAME_MARBLE) {
            state->game.marble.keepsake_count = saved.game.marble.keepsake_count;
            state->game.marble.stages_cleared = saved.game.marble.stages_cleared;
            memcpy(state->game.marble.keepsakes, saved.game.marble.keepsakes,
                    sizeof(state->game.marble.keepsakes));
        }
        return;
    }
    if (!state->started || state->user_paused || state->game_over) return;
    if (action == ORB_GAME_STEER_START && state->kind == ORB_GAME_SNAKE) {
        state->game.snake.touch_held = true;
        state->game.snake.finger = (orb_game_point_t){x - ORB_GAME_CX, y - ORB_GAME_CY};
        return;
    }
    if (action == ORB_GAME_STEER_START && state->kind == ORB_GAME_BEADS) {
        state->game.beads.touch_held = true;
        state->game.beads.touch_x = x - ORB_GAME_CX;
        state->game.beads.touch_y = y - ORB_GAME_CY;
        return;
    }
    if (state->kind == ORB_GAME_MARBLE && state->game.marble.stage_cleared &&
        action == ORB_GAME_PRIMARY) {
        ++state->game.marble.stage;
        state->game.marble.stage_cleared = false;
        return;
    }
    if (action == ORB_GAME_REWIND_START) { state->game.marble.rewind_active = true; return; }
    ++s_actions;
    if (action == ORB_GAME_TAP && state->kind == ORB_GAME_ORBIT) state->game.orbit.ring ^= 1;
    else if (action == ORB_GAME_UP) state->game.marble.vx = 60;
    else if (action == ORB_GAME_LEFT) --state->lives;
    else if (action == ORB_GAME_RIGHT) state->game_over = true;
    else state->score += 10;
}

static int record_index(const char *id)
{
    for (unsigned i = 0; i < ORB_GAME_COUNT; ++i)
        if (!strcmp(id, orb_game_id((orb_game_kind_t)i))) return (int)i;
    return -1;
}
bool orb_fs_valid_app_id(const char *id)
{
    if (!id || !*id || strlen(id) > ORB_FS_APP_ID_MAX) return false;
    for (const char *p = id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return false;
    return true;
}
bool orb_fs_ready(void) { return s_storage_ready; }
esp_err_t orb_fs_app_prepare(const char *id)
{
    CHECK(s_lock_depth == 0 && record_index(id) >= 0);
    ++s_prepares;
    return s_prepare_error;
}
esp_err_t orb_fs_read(const char *id, const char *key, void *bytes, size_t capacity, size_t *size)
{
    CHECK(s_lock_depth == 0);
    if (!strcmp(key, "keepsakes")) {
        CHECK(!strcmp(id, "marble"));
        ++s_garden_reads;
        if (s_read_error != ESP_OK) return s_read_error;
        if (!s_garden.exists) return ESP_ERR_NOT_FOUND;
        CHECK(capacity >= s_garden.size);
        *size = s_garden.size;
        memcpy(bytes, s_garden.bytes, *size);
        return ESP_OK;
    }
    CHECK(!strcmp(key, "high-score") || (!strcmp(id, "ring-snake") && !strcmp(key, "high-score-v2")));
    ++s_reads;
    if (s_read_error != ESP_OK) return s_read_error;
    int index = record_index(id);
    CHECK(index >= 0);
    score_record_t *record = index == ORB_GAME_SNAKE && !strcmp(key, "high-score") ?
        &s_legacy_snake : &s_records[index];
    if (!record->exists) return ESP_ERR_NOT_FOUND;
    CHECK(capacity >= record->size);
    *size = record->size;
    memcpy(bytes, record->bytes, *size);
    return ESP_OK;
}
esp_err_t orb_fs_write(const char *id, const char *key, const void *bytes, size_t size)
{
    CHECK(s_lock_depth == 0);
    if (!strcmp(key, "keepsakes")) {
        CHECK(!strcmp(id, "marble") && size == ORB_KEEPSAKE_RECORD_SIZE);
        ++s_garden_writes;
        if (s_write_error != ESP_OK) return s_write_error;
        memcpy(s_garden.bytes, bytes, size);
        s_garden.size = size;
        s_garden.exists = true;
        return ESP_OK;
    }
    CHECK(!strcmp(key, !strcmp(id, "ring-snake") ? "high-score-v2" : "high-score") && size == 12);
    ++s_writes;
    if (s_write_error != ESP_OK) return s_write_error;
    int index = record_index(id);
    CHECK(index >= 0);
    memcpy(s_records[index].bytes, bytes, size);
    s_records[index].size = size;
    s_records[index].exists = true;
    return ESP_OK;
}

static uint32_t record_score(unsigned index)
{
    const uint8_t *p = s_records[index].bytes + 4;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void render_now(void)
{
    CHECK(s_timer.alive);
    if (s_timer.paused) return;
    lv_lock();
    s_timer.callback(&s_timer);
    lv_unlock();
}
static void ui_action(int action)
{
    CHECK(s_view.alive);
    lv_lock();
    s_view.callback(action, 233, 253, s_view.context);
    lv_unlock();
}
static void poll(int64_t now, bool screen_on)
{
    orb_apps_poll(now, false, screen_on, 0, 0, 1, false);
}
static void poll_launch(void)
{
    CHECK(s_last_model.started && !s_last_model.user_paused && !s_last_model.game_over);
    poll(0, true);
}
/* This fixture already fakes the native view and LVGL. Adapt only the new
 * presentation boundary; real descriptor ownership is tested in app_play. */
static orb_games_view_port_t s_presentation_view;
static void *presentation_open(lv_obj_t *parent, const orb_games_view_port_t *view)
{
    s_presentation_view = *view;
    if (!view->create(parent, view->context)) {
        view->destroy(view->context);
        return NULL;
    }
    return &s_presentation_view;
}
static void presentation_close(void *context)
{
    orb_games_view_port_t *view = context;
    view->destroy(view->context);
    memset(view, 0, sizeof(*view));
}

static void initialize(uint32_t capabilities)
{
    orb_apps_exit();
    CHECK(s_live_allocations == 0 && !s_timer.alive && !s_view.alive && !s_lock_depth);
    CHECK(s_active_screen == &s_original_screen && !s_game_screen.alive);
    CHECK(s_screen_creates == s_screen_deletes);
    CHECK(!s_refresh_event.alive && !s_refresh_ready_event.alive);
    s_alloc_fail = s_view_fail = s_timer_fail = s_screen_fail = false;
    s_reduce_motion = false;
    s_display.pending = false;
    s_event_fail = false;
    s_ready_event_fail = false;
    s_timer_ready = 0;
    s_view_updates = 0;
    s_te_pacing = true;
    s_ui_now_us = s_view_work_us = 0;
    s_prepare_error = s_read_error = s_write_error = ESP_OK;
    s_storage_ready = true;
    s_allocations = s_frees = s_view_creates = s_view_destroys = s_timer_creates = s_timer_deletes = 0;
    s_reads = s_writes = s_prepares = s_steps = s_actions = 0;
    s_garden_reads = s_garden_writes = 0;
    s_complete_stage = false;
    s_bead_flow = 0;
    s_step_interleave = false;
    s_real_rewind_fixture = false;
    memset(s_sound_counts, 0, sizeof(s_sound_counts));
    memset(s_records, 0, sizeof(s_records));
    memset(&s_legacy_snake, 0, sizeof(s_legacy_snake));
    memset(&s_garden, 0, sizeof(s_garden));
    static const orb_games_presentation_t s_port = {presentation_open, presentation_close};
    CHECK(orb_games_set_presentation(&s_port));
    orb_apps_init(capabilities);
    /* Lifecycle counters exclude the bounded boot progress snapshot reads. */
    s_reads = s_garden_reads = 0;
}

static void catalogue_and_requests(void)
{
    initialize(ORB_APP_NEEDS_STORAGE | ORB_APP_NEEDS_IMU);
    CHECK(orb_apps_count() == 5);
    for (unsigned i = 0; i < ORB_GAME_COUNT; ++i) {
        const orb_app_manifest_t *manifest = orb_apps_at(i);
        CHECK(manifest && !strcmp(manifest->id, orb_game_id((orb_game_kind_t)i)));
        CHECK(manifest->needs == (i == ORB_GAME_BEADS ? ORB_APP_NEEDS_IMU : ORB_APP_NEEDS_STORAGE));
        static const uint32_t colors[] = {0x30d158, 0xff375f, 0x6d7cff, 0xff9230, 0xffd600};
        CHECK(manifest->colour == colors[i]);
        for (unsigned j = 0; j < i; j++) {
            CHECK(strcmp(manifest->icon, orb_apps_at(j)->icon));
            CHECK(manifest->colour != orb_apps_at(j)->colour);
        }
        unsigned opens = s_sound_counts[ORB_SOUND_OPEN];
        CHECK(orb_apps_enter(manifest->id) == ESP_OK); /* No NET/AUDIO capability required. */
        CHECK(orb_apps_active() == manifest);
        CHECK(s_active_screen == &s_game_screen && s_game_screen.alive);
        CHECK(s_last_model.started && !s_last_model.user_paused && !s_last_model.game_over);
        CHECK(!s_last_model.ticks && !s_last_model.score);
        CHECK(s_sound_counts[ORB_SOUND_OPEN] == opens + (i == ORB_GAME_BEADS ? 0u : 1u));
        CHECK(!orb_games_request_back());
        CHECK(orb_games_request_pause()); /* PWR pauses even before the first physics step. */
        poll(0, true); render_now();
        CHECK(s_last_model.user_paused && !s_last_model.ticks && !orb_apps_paused());
        if (i == ORB_GAME_MARBLE) {
            ui_action(ORB_GAME_GARDEN); poll(1, true); render_now();
            CHECK(s_last_model.game.marble.garden_open && s_last_model.user_paused);
            ui_action(ORB_GAME_GARDEN); poll(2, true); render_now();
            CHECK(!s_last_model.game.marble.garden_open && s_last_model.user_paused);
        }
        ui_action(ORB_GAME_PAUSE); poll(3, true); render_now();
        CHECK(s_last_model.started && !s_last_model.user_paused && !s_last_model.ticks);
        CHECK(s_sound_counts[ORB_SOUND_OPEN] == opens + (i == ORB_GAME_BEADS ? 0u : 2u));
        CHECK(orb_games_request_pause()); poll(4, true); render_now();
        CHECK(s_last_model.user_paused && orb_games_request_back()); /* BOOT exits Pause. */
        poll(5, true);
        CHECK(!orb_apps_active());
        CHECK(s_active_screen == &s_original_screen && !s_game_screen.alive);
    }
    CHECK(orb_apps_request_enter("missing") == ESP_ERR_NOT_FOUND);
    CHECK(orb_apps_request_enter("pop") == ESP_OK);
    CHECK(!orb_apps_active()); /* UI never mutates main-owned lifecycle. */
    CHECK(orb_apps_request_enter("orbit") == ESP_ERR_INVALID_STATE);
    poll(0, true);
    CHECK(orb_apps_active() && !strcmp(orb_apps_active()->id, "pop"));
    render_now();
    CHECK(s_last_model.started && !s_last_model.user_paused && !s_last_model.game_over);
    ui_action(ORB_GAME_CLOSE);
    CHECK(orb_apps_active());
    poll(1, true);
    CHECK(!orb_apps_active() && !s_view.alive && !s_timer.alive);
    CHECK(orb_apps_request_enter("marble") == ESP_OK);
    orb_apps_request_exit();
    CHECK(orb_apps_request_enter("orbit") == ESP_ERR_INVALID_STATE);
    poll(2, true);
    CHECK(!orb_apps_active()); /* Close overrides pending launch. */
    initialize(0);
    CHECK(orb_apps_enter("marble") == ESP_ERR_NOT_SUPPORTED);
    CHECK(s_allocations == 0 && s_reads == 0);
    initialize(ORB_APP_NEEDS_STORAGE);
    s_prepare_error = ESP_FAIL;
    CHECK(orb_apps_enter("pop") == ESP_FAIL);
    CHECK(!orb_apps_active() && s_allocations == 0 && s_view_creates == 0);
}

static void fixed_step_and_pause(void)
{
    initialize(ORB_APP_NEEDS_STORAGE);
    CHECK(orb_apps_enter("pop") == ESP_OK);
    CHECK(s_last_model.started && !s_last_model.ticks && !s_last_model.user_paused);
    CHECK(s_sound_counts[ORB_SOUND_OPEN] == 1);
    poll_launch();
    CHECK(s_sound_counts[ORB_SOUND_OPEN] == 1); /* First tick never repeats the launch cue. */
    unsigned allocations = s_allocations, reads = s_reads;
    poll(0, true); poll(10000, true); render_now();
    CHECK(s_last_model.ticks == 0);
    poll(16667, true); render_now();
    CHECK(s_last_model.ticks == 1);
    poll(33334, true); render_now();
    CHECK(s_last_model.ticks == 2);
    poll(2000000, true); render_now();
    CHECK(s_last_model.ticks == 8); /* A hitch never runs unbounded catch-up. */
    poll(2000000, true); render_now();
    CHECK(s_last_model.ticks == 8);
    poll(1000, true); render_now();
    CHECK(s_last_model.ticks == 8);
    poll(17667, true); render_now();
    CHECK(s_last_model.ticks == 9);
    s_reduce_motion = true; render_now();
    orb_apps_poll(34334, false, true, NAN, INFINITY, 1, true);
    CHECK(!s_last_motion_valid && s_last_gx == 0 && s_last_gy == 0 && s_last_reduce_motion);
    orb_apps_poll(51001, false, true, 0.3f, -0.4f, .866f, true);
    CHECK(s_last_motion_valid && s_last_gx == 0.3f && s_last_gy == -0.4f);
    ui_action(ORB_GAME_PRIMARY);
    poll(60000, false); render_now();
    CHECK(orb_apps_paused() && s_last_paused && s_last_model.score == 0);
    CHECK(s_timer.paused);
    unsigned paused_updates = s_view_updates;
    uint32_t ticks = s_last_model.ticks;
    for (unsigned i = 0; i < 100; ++i) {
        ui_action(ORB_GAME_PRIMARY);
        poll(1000000 + (int64_t)i * 100000, false);
        render_now();
        CHECK(s_last_model.ticks == ticks && s_last_model.score == 0);
        CHECK(s_timer.paused && s_view_updates == paused_updates);
    }
    poll(3600000000LL, true); render_now();
    CHECK(!orb_apps_paused() && !s_last_paused && s_last_model.ticks == ticks);
    CHECK(!s_timer.paused);
    poll(3600016667LL, true); render_now();
    CHECK(s_last_model.ticks == ticks + 1);
    CHECK(s_allocations == allocations && s_reads == reads && s_writes == 0);
    poll(3600100000LL, false);
    ui_action(ORB_GAME_CLOSE);
    poll(3600100001LL, false);
    CHECK(!orb_apps_active() && s_live_allocations == 0);
}

static void power_exit(void)
{
    initialize(ORB_APP_NEEDS_STORAGE);
    CHECK(orb_apps_enter("orbit") == ESP_OK);
    orb_apps_poll(0, true, false, 0, 0, 1, false);
    CHECK(orb_apps_paused());
    orb_apps_poll(ORB_APPS_PWR_EXIT_US - 1, true, false, 0, 0, 1, false);
    CHECK(orb_apps_active());
    orb_apps_poll(ORB_APPS_PWR_EXIT_US, true, false, 0, 0, 1, false);
    CHECK(!orb_apps_active() && !s_view.alive && !s_timer.alive);
    CHECK(orb_apps_enter("orbit") == ESP_OK);
    orb_apps_poll(0, true, true, 0, 0, 1, false);
    orb_apps_poll(1000000, false, true, 0, 0, 1, false);
    orb_apps_poll(1100000, true, true, 0, 0, 1, false);
    orb_apps_poll(2299999, true, true, 0, 0, 1, false);
    CHECK(orb_apps_active());
    orb_apps_poll(2300000, true, true, 0, 0, 1, false);
    CHECK(!orb_apps_active());
}

static void bounded_actions(void)
{
    initialize(ORB_APP_NEEDS_STORAGE);
    CHECK(orb_apps_enter("marble") == ESP_OK);
    poll_launch();
    for (unsigned i = 0; i < 1000; ++i) ui_action(ORB_GAME_PRIMARY);
    CHECK(s_actions == 0);
    poll(0, true); render_now();
    CHECK(s_actions == 15 && s_last_model.score == 150);
    CHECK(s_allocations == 1 && s_writes == 0 && s_reads == 1);
    for (unsigned i = 0; i < 1000; ++i) {
        ui_action(ORB_GAME_PRIMARY);
        poll((int64_t)(i + 1) * 20000, true);
        render_now();
    }
    CHECK(s_actions == 1015 && s_last_model.score == 10150);
    CHECK(s_allocations == 1 && s_writes == 0 && s_reads == 1);
    orb_apps_exit();
    CHECK(s_writes == 1 && record_score(0) == 10150);
}

static void per_game_storage(void)
{
    initialize(ORB_APP_NEEDS_STORAGE | ORB_APP_NEEDS_IMU);
    for (unsigned i = 0; i < ORB_GAME_COUNT; ++i) {
        CHECK(orb_apps_enter(orb_game_id((orb_game_kind_t)i)) == ESP_OK);
        poll_launch();
        for (unsigned n = 0; n <= i; ++n) ui_action(ORB_GAME_PRIMARY);
        poll(0, true); render_now();
        CHECK(s_last_high == (i + 1) * 10);
        CHECK(s_writes == i); /* Only lifecycle writes, never per-step/frame. */
        ui_action(ORB_GAME_RESTART); poll(1, true); render_now();
        CHECK(s_last_model.score == 0 && s_last_high == (i + 1) * 10);
        orb_apps_exit();
        CHECK(s_records[i].exists && record_score(i) == (i + 1) * 10);
        CHECK(orb_games_last_save_error() == ESP_OK);
    }
    unsigned writes = s_writes;
    for (unsigned i = 0; i < ORB_GAME_COUNT; ++i) {
        CHECK(orb_apps_enter(orb_game_id((orb_game_kind_t)i)) == ESP_OK);
        CHECK(s_last_model.score == 0 && s_last_high == (i + 1) * 10 && !s_last_failed);
        orb_apps_exit();
    }
    CHECK(s_writes == writes); /* Unchanged scores never wear flash. */
    CHECK(orb_apps_enter("marble") == ESP_OK);
    poll_launch();
    for (unsigned i = 0; i < 3; ++i) ui_action(ORB_GAME_PRIMARY);
    poll(0, true);
    s_write_error = ESP_FAIL;
    orb_apps_exit();
    CHECK(orb_games_last_save_error() == ESP_FAIL && record_score(0) == 10);
    s_write_error = ESP_OK;
    CHECK(orb_apps_enter("marble") == ESP_OK);
    CHECK(s_last_high == 10);
    orb_apps_exit();
    /* Truncated, incompatible and corrupted scores fail visibly and can be
     * replaced only by a new actual score, without trusting arbitrary bytes. */
    for (unsigned failure = 0; failure < 4; ++failure) {
        uint8_t original[12]; memcpy(original, s_records[0].bytes, 12);
        size_t original_size = s_records[0].size;
        if (failure == 0) s_records[0].size = 7;
        else if (failure == 1) s_records[0].bytes[0] = 'X';
        else if (failure == 2) s_records[0].bytes[8] ^= 1;
        else s_read_error = ESP_FAIL;
        CHECK(orb_apps_enter("marble") == ESP_OK);
        CHECK(s_last_high == 0 && s_last_failed);
        poll_launch();
        s_read_error = ESP_OK;
        ui_action(ORB_GAME_PRIMARY); poll(0, true);
        orb_apps_exit();
        CHECK(orb_games_last_save_error() == ESP_OK && record_score(0) == 10);
        memcpy(s_records[0].bytes, original, 12); s_records[0].size = original_size;
    }
}

static void partial_entry_cleanup(void)
{
    const char *tilt_ids[] = {"marble", "beads", "ring-snake"};
    for (unsigned game = 0; game < 3; ++game) {
        for (unsigned fault = 0; fault < 4; ++fault) {
            initialize(ORB_APP_NEEDS_STORAGE | ORB_APP_NEEDS_IMU);
            s_alloc_fail = fault == 0;
            s_screen_fail = fault == 1;
            s_view_fail = fault == 2;
            s_timer_fail = fault == 3;
            CHECK(orb_apps_enter(tilt_ids[game]) == ESP_ERR_NO_MEM);
            CHECK(!orb_games_motion_active() && !orb_apps_active() && !s_live_allocations);
        }
    }
    initialize(ORB_APP_NEEDS_STORAGE);
    s_alloc_fail = true;
    CHECK(orb_apps_enter("pop") == ESP_ERR_NO_MEM);
    CHECK(!orb_apps_active() && s_live_allocations == 0 && s_view_creates == 0);
    s_alloc_fail = false; s_screen_fail = true;
    CHECK(orb_apps_enter("pop") == ESP_ERR_NO_MEM);
    CHECK(!orb_apps_active() && s_live_allocations == 0 && !s_game_screen.alive);
    CHECK(s_active_screen == &s_original_screen && s_view_creates == 0);
    s_screen_fail = false; s_view_fail = true;
    CHECK(orb_apps_enter("pop") == ESP_ERR_NO_MEM);
    CHECK(!orb_apps_active() && s_live_allocations == 0 && !s_view.alive && !s_timer.alive);
    CHECK(s_timer_creates == 0 && s_writes == 0);
    s_view_fail = false; s_timer_fail = true;
    CHECK(orb_apps_enter("pop") == ESP_ERR_NO_MEM);
    CHECK(!orb_apps_active() && s_live_allocations == 0 && !s_view.alive && !s_timer.alive);
    CHECK(s_view_destroys == 1 && s_timer_deletes == 0 && s_writes == 0);
    s_timer_fail = false;
    CHECK(orb_apps_enter("pop") == ESP_OK);
    CHECK(s_live_allocations == 1 && s_timer.alive && s_view.alive);
    orb_apps_exit(); orb_apps_exit();
    CHECK(s_live_allocations == 0 && s_view_destroys == 2 && s_timer_deletes == 1);
    CHECK(s_active_screen == &s_original_screen && !s_game_screen.alive);
    CHECK(s_screen_creates == s_screen_deletes);
}

static void benchmark_ownership(void)
{
    initialize(ORB_APP_NEEDS_STORAGE);
    CHECK(orb_games_benchmark_reserve());
    CHECK(!orb_games_benchmark_reserve());
    CHECK(orb_apps_enter("pop") == ESP_ERR_INVALID_STATE);
    CHECK(!orb_apps_active() && s_allocations == 0);
    orb_games_benchmark_release();
    CHECK(orb_apps_enter("pop") == ESP_OK);
    CHECK(!orb_games_benchmark_reserve());
    orb_games_benchmark_release(); /* Releasing another owner cannot unlock it. */
    CHECK(!orb_games_benchmark_reserve() && orb_apps_active());
    orb_apps_exit();
    CHECK(orb_games_benchmark_reserve());
    orb_games_benchmark_release();
}

static void user_pause_and_sound(void)
{
    initialize(ORB_APP_NEEDS_STORAGE);
    CHECK(orb_apps_enter("marble") == ESP_OK);
    poll_launch(); render_now();
    CHECK(s_last_model.started && s_sound_counts[ORB_SOUND_OPEN] == 1);
    ui_action(ORB_GAME_CONTROLS); poll(1, true); render_now();
    CHECK(s_last_model.controls); /* Unavailable motion retains its safe fallback. */
    ui_action(ORB_GAME_PRIMARY); poll(2, true);
    CHECK(s_sound_counts[ORB_SOUND_GAME_COLLECT] == 1);
    ui_action(ORB_GAME_PAUSE); poll(1000000, true); render_now();
    uint32_t ticks = s_last_model.ticks, score = s_last_model.score;
    CHECK(s_last_model.user_paused && !orb_apps_paused());
    CHECK(s_sound_counts[ORB_SOUND_OPEN] == 2); /* One pause-sheet entrance cue. */
    ui_action(ORB_GAME_PRIMARY); poll(2000000, true); render_now();
    CHECK(s_last_model.score == score && s_last_model.ticks == ticks);
    CHECK(s_sound_counts[ORB_SOUND_GAME_COLLECT] == 1);
    poll(3000000, false); render_now();
    CHECK(s_last_paused && s_last_model.user_paused);
    poll(4000000, true); render_now();
    CHECK(!s_last_paused && s_last_model.user_paused && s_last_model.ticks == ticks);
    ui_action(ORB_GAME_PAUSE); poll(5000000, true); render_now();
    CHECK(!s_last_model.user_paused && s_last_model.ticks == ticks);
    CHECK(s_sound_counts[ORB_SOUND_CLOSE] == 1); /* One explicit Resume cue. */
    poll(5016667, true); render_now();
    CHECK(s_last_model.ticks == ticks + 1);
    ui_action(ORB_GAME_UP); poll(5016668, true);
    for (unsigned i = 1; i <= 90; ++i) poll(5016668 + (int64_t)i * ORB_GAME_STEP_US, true);
    CHECK(s_sound_counts[ORB_SOUND_GAME_ROLL] == 0);
    for (unsigned i = 0; i < 50; ++i) poll(5016668 + 90LL * ORB_GAME_STEP_US, true);
    CHECK(s_sound_counts[ORB_SOUND_GAME_ROLL] == 0); /* No sound on duplicate polls. */
    ui_action(ORB_GAME_LEFT); poll(7000000, true);
    CHECK(s_sound_counts[ORB_SOUND_DENY] == 1);
    ui_action(ORB_GAME_RIGHT); poll(7000001, true); render_now();
    CHECK(s_last_model.game_over && s_sound_counts[ORB_SOUND_DONE] == 1);
    poll(8000000, true);
    CHECK(s_sound_counts[ORB_SOUND_DONE] == 1);
    ui_action(ORB_GAME_RESTART); poll(8000001, true); render_now();
    CHECK(s_last_model.started && !s_last_model.game_over && s_last_model.controls);
    CHECK(s_allocations == 1 && s_writes == 0 && s_reads == 1);
    orb_apps_exit();

    CHECK(orb_apps_enter("orbit") == ESP_OK);
    poll_launch();
    ui_action(ORB_GAME_TAP); poll(1, true); render_now();
    CHECK(s_last_model.game.orbit.ring == 1 && s_sound_counts[ORB_SOUND_GAME_SWITCH] == 1);
    ui_action(ORB_GAME_TAP); ui_action(ORB_GAME_TAP); poll(2, true); render_now();
    CHECK(s_last_model.game.orbit.ring == 1 && s_sound_counts[ORB_SOUND_GAME_SWITCH] == 1);
    orb_apps_exit();
    CHECK(orb_apps_enter("pop") == ESP_OK);
    poll_launch();
    ui_action(ORB_GAME_PRIMARY); poll(1, true);
    CHECK(s_sound_counts[ORB_SOUND_GAME_POP] == 1);
    ui_action(ORB_GAME_FINISH); poll(2, true); render_now();
    CHECK(!s_last_model.game_over && s_sound_counts[ORB_SOUND_DONE] == 1);
    ui_action(ORB_GAME_PAUSE); poll(3, true); render_now();
    CHECK(s_last_model.user_paused && s_last_model.score == 10);
    ui_action(ORB_GAME_FINISH); poll(4, true); render_now();
    CHECK(s_last_model.game_over && !s_last_model.user_paused);
    CHECK(s_last_model.score == 10 && s_last_high == 10 && s_last_model.lives == 3);
    CHECK(s_sound_counts[ORB_SOUND_DONE] == 1 && s_writes == 1);
    ui_action(ORB_GAME_FINISH); poll(5, true); render_now();
    CHECK(s_sound_counts[ORB_SOUND_DONE] == 1 && s_last_model.score == 10);
    orb_apps_exit();
    CHECK(record_score(ORB_GAME_POP) == 10);
}

static void game_over_restart_and_done(void)
{
    initialize(ORB_APP_NEEDS_STORAGE | ORB_APP_NEEDS_IMU);
    for (unsigned i = ORB_GAME_ORBIT; i < ORB_GAME_COUNT; ++i) {
        CHECK(orb_apps_enter(orb_game_id((orb_game_kind_t)i)) == ESP_OK);
        poll_launch();
        unsigned failures = s_sound_counts[ORB_SOUND_FAILED], successes = s_sound_counts[ORB_SOUND_DONE];
        ui_action(ORB_GAME_RIGHT); poll(1, true); render_now();
        CHECK(s_last_model.started && s_last_model.game_over && !s_last_model.user_paused);
        CHECK(s_sound_counts[ORB_SOUND_FAILED] == failures + (i == ORB_GAME_POP ? 0u : 1u));
        CHECK(s_sound_counts[ORB_SOUND_DONE] == successes + (i == ORB_GAME_POP ? 1u : 0u));
        poll(1, true);
        CHECK(s_sound_counts[ORB_SOUND_FAILED] == failures + (i == ORB_GAME_POP ? 0u : 1u));
        ui_action(ORB_GAME_RESTART); poll(2, true); render_now();
        CHECK(s_last_model.started && !s_last_model.game_over && !s_last_model.user_paused);
        CHECK(!s_last_model.ticks && !s_last_model.score);
        ui_action(ORB_GAME_RIGHT); poll(3, true); render_now();
        CHECK(s_last_model.game_over && orb_apps_active());
        ui_action(ORB_GAME_CLOSE); /* Done queues the registry exit for the shell picker. */
        CHECK(orb_apps_active());
        poll(4, true);
        CHECK(!orb_apps_active() && !s_view.alive && !s_timer.alive);
        CHECK(s_active_screen == &s_original_screen && !s_game_screen.alive);
    }
}

static void garden_lifecycle(void)
{
    initialize(ORB_APP_NEEDS_STORAGE);
    CHECK(orb_apps_enter("marble") == ESP_OK);
    CHECK(s_garden_reads == 1 && s_garden_writes == 0 && !s_last_failed);
    poll_launch();
    s_complete_stage = true;
    poll(ORB_GAME_STEP_US, true); render_now();
    CHECK(s_last_model.game.marble.stage_cleared);
    CHECK(s_last_model.game.marble.keepsake_count == 1);
    CHECK(s_last_model.game.marble.stages_cleared == 1 && s_garden_writes == 1);
    orb_keepsake_record_t record;
    CHECK(orb_keepsake_decode(s_garden.bytes, s_garden.size, &record));
    CHECK(record.count == 1 && record.stages_cleared == 1 && record.items[0].kind == 1);
    for (unsigned i = 2; i <= 102; ++i) {
        poll((int64_t)i * ORB_GAME_STEP_US, true); render_now();
    }
    CHECK(s_garden_writes == 1 && s_allocations == 1);
    orb_apps_exit();
    CHECK(s_garden_writes == 1); /* Already durable before exit; no flash wear. */
    CHECK(orb_apps_enter("marble") == ESP_OK);
    CHECK(s_last_model.started && s_last_model.game.marble.keepsake_count == 1);
    CHECK(s_last_model.game.marble.keepsakes[0].x == 80 && !s_last_failed);
    poll_launch();
    ui_action(ORB_GAME_RESTART); poll(0, true); render_now();
    CHECK(s_last_model.game.marble.keepsake_count == 1);
    uint8_t original[ORB_KEEPSAKE_RECORD_SIZE];
    memcpy(original, s_garden.bytes, sizeof(original));
    s_write_error = ESP_FAIL;
    s_complete_stage = true;
    poll(ORB_GAME_STEP_US, true); render_now();
    CHECK(s_garden_writes == 2 && s_last_failed);
    CHECK(orb_games_last_save_error() == ESP_FAIL);
    CHECK(!memcmp(original, s_garden.bytes, sizeof(original)));
    for (unsigned i = 2; i <= 102; ++i) poll((int64_t)i * ORB_GAME_STEP_US, true);
    CHECK(s_garden_writes == 2); /* Failure never retries on moving frames. */
    s_write_error = ESP_OK;
    orb_apps_exit();
    CHECK(s_garden_writes == 3 && orb_games_last_save_error() == ESP_OK);
    CHECK(orb_apps_enter("marble") == ESP_OK);
    CHECK(s_last_model.game.marble.keepsake_count == 2 && !s_last_failed);
    orb_apps_exit();
    CHECK(orb_apps_enter("marble") == ESP_OK);
    poll_launch();
    s_complete_stage = true; s_write_error = ESP_FAIL;
    poll(ORB_GAME_STEP_US, true); render_now();
    CHECK(s_last_model.game.marble.keepsake_count == 3 && s_last_failed);
    orb_apps_exit();
    CHECK(orb_games_last_save_error() == ESP_FAIL && s_garden_writes == 5);
    s_write_error = ESP_OK;
    CHECK(orb_apps_enter("marble") == ESP_OK);
    CHECK(s_last_model.game.marble.keepsake_count == 2 && !s_last_failed);
    orb_apps_exit(); /* Both failed writes preserved the complete prior file. */
    unsigned writes = s_garden_writes;
    for (unsigned failure = 0; failure < 4; ++failure) {
        memcpy(original, s_garden.bytes, sizeof(original));
        size_t size = s_garden.size;
        if (failure == 0) s_garden.size = 7;
        else if (failure == 1) s_garden.bytes[0] = 'X';
        else if (failure == 2) s_garden.bytes[s_garden.size - 1] ^= 1;
        else s_read_error = ESP_FAIL;
        CHECK(orb_apps_enter("marble") == ESP_OK);
        CHECK(s_last_failed && s_last_model.game.marble.keepsake_count == 0);
        s_read_error = ESP_OK;
        orb_apps_exit();
        CHECK(s_garden_writes == writes); /* Reading damage never overwrites it. */
        memcpy(s_garden.bytes, original, sizeof(original)); s_garden.size = size;
    }
    unsigned reads = s_garden_reads;
    CHECK(orb_apps_enter("pop") == ESP_OK);
    CHECK(s_garden_writes == writes && s_garden_reads == reads);
    orb_apps_exit();
}

static void rewind_release_and_interruption(void)
{
    initialize(ORB_APP_NEEDS_STORAGE);
    CHECK(orb_apps_enter("marble") == ESP_OK);
    poll_launch();
    ui_action(ORB_GAME_REWIND_START);
    for (unsigned i = 0; i < 1000; ++i) ui_action(ORB_GAME_PRIMARY);
    poll(0, true); render_now();
    CHECK(s_last_model.game.marble.rewind_active);
    for (unsigned i = 0; i < 1000; ++i) ui_action(ORB_GAME_PRIMARY);
    ui_action(ORB_GAME_REWIND_STOP);
    poll(1, true); render_now();
    CHECK(!s_last_model.game.marble.rewind_active); /* Full mailbox cannot lose release. */
    ui_action(ORB_GAME_REWIND_START); ui_action(ORB_GAME_REWIND_STOP);
    poll(2, true); render_now();
    CHECK(!s_last_model.game.marble.rewind_active);
    ui_action(ORB_GAME_REWIND_START); poll(3, true); render_now();
    CHECK(s_last_model.game.marble.rewind_active);
    poll(4, false); render_now();
    CHECK(s_last_paused && !s_last_model.game.marble.rewind_active);
    ui_action(ORB_GAME_REWIND_START); /* Off-screen holds are discarded. */
    poll(10000000, true); render_now();
    CHECK(!s_last_paused && !s_last_model.game.marble.rewind_active);
    ui_action(ORB_GAME_REWIND_START); poll(10000001, true);
    ui_action(ORB_GAME_PAUSE); poll(10000002, true); render_now();
    CHECK(s_last_model.user_paused && !s_last_model.game.marble.rewind_active);
    ui_action(ORB_GAME_PAUSE); poll(10000003, true); render_now();
    CHECK(!s_last_model.user_paused && !s_last_model.game.marble.rewind_active);
    CHECK(s_garden_writes == 0 && s_allocations == 1);
    orb_apps_exit();
}

static void full_garden_commit(void)
{
    initialize(ORB_APP_NEEDS_STORAGE);
    orb_keepsake_record_t record = {.magic = {'O', 'G', 'K', '1'}, .version = 1,
        .count = ORB_MARBLE_KEEPSAKES_MAX, .stages_cleared = ORB_MARBLE_STAGES};
    for (unsigned i = 0; i < record.count; ++i) {
        record.items[i].kind = (uint8_t)(i % 3);
        record.items[i].stage = (uint8_t)(i % 3 + 1);
        record.items[i].x = (int16_t)(i * 3);
    }
    s_garden.size = orb_keepsake_encode(&record, s_garden.bytes, sizeof(s_garden.bytes));
    CHECK(s_garden.size == ORB_KEEPSAKE_RECORD_SIZE);
    s_garden.exists = true;
    CHECK(orb_apps_enter("marble") == ESP_OK);
    CHECK(s_last_model.game.marble.keepsake_count == ORB_MARBLE_KEEPSAKES_MAX);
    poll_launch();
    s_complete_stage = true; poll(ORB_GAME_STEP_US, true); render_now();
    CHECK(s_garden_writes == 1); /* The bounded count did not change; contents did. */
    CHECK(orb_keepsake_decode(s_garden.bytes, s_garden.size, &record));
    CHECK(record.count == ORB_MARBLE_KEEPSAKES_MAX && record.stages_cleared == 3);
    CHECK(record.items[0].kind == 1 && record.items[ORB_MARBLE_KEEPSAKES_MAX - 1].kind == 1);
    orb_apps_exit();
    CHECK(orb_apps_enter("marble") == ESP_OK);
    CHECK(s_last_model.game.marble.keepsakes[ORB_MARBLE_KEEPSAKES_MAX - 1].kind == 1);
    CHECK(s_last_model.game.marble.stages_cleared == 3 && s_garden_writes == 1);
    orb_apps_exit();
}

static void tilt_calibration_fallback_and_system_requests(void)
{
    initialize(ORB_APP_NEEDS_STORAGE | ORB_APP_NEEDS_IMU);
    CHECK(!orb_games_request_pause() && !orb_games_request_back());
    CHECK(orb_apps_enter("marble") == ESP_OK);
    CHECK(!orb_games_request_back());
    /* Synthetic accelerometer readings are specific force. A comfortable
     * 35-degree viewing angle has +y toward the top edge and +z out of glass. */
    const float ay = .57357644f, az = .81915204f;
    const int64_t origin = 900000000000LL;
    CHECK(s_last_model.started && !s_last_model.user_paused && !s_last_model.ticks);
    orb_apps_poll(origin, false, true, 0, ay, az, true);
    render_now();
    CHECK(s_last_model.started && !s_last_model.controls && !s_last_model.ticks);
    for (unsigned i = 0; i < 100; ++i) {
        orb_apps_poll(origin, false, true, 0, ay, az, true);
        render_now();
        CHECK(!s_last_model.ticks); /* Duplicate polls never count as samples. */
    }
    for (unsigned i = 1; i < 15; ++i) {
        orb_apps_poll(origin + (int64_t)i * 20000, false, true, 0, ay, az, true);
        render_now();
        CHECK(!s_last_model.ticks);
    }
    orb_apps_poll(origin + 300000, false, true, 0, ay, az, true);
    render_now();
    CHECK(s_last_model.ticks == 1 && s_last_gx == 0 && s_last_gy == 0);
    /* Right side down reads ax < 0; calibrated force retains that sign for
     * physics to accelerate downhill to the right. */
    orb_apps_poll(origin + 320000, false, true, -.17364818f,
        ay * .98480775f, az * .98480775f, true);
    render_now();
    CHECK(s_last_gx < -.02f && fabsf(s_last_gy) < .001f);
    CHECK(orb_games_request_pause() && orb_games_request_pause());
    orb_apps_poll(origin + 340000, false, true, 0, ay, az, true);
    render_now();
    uint32_t paused_ticks = s_last_model.ticks;
    CHECK(s_last_model.user_paused && !orb_apps_paused());
    CHECK(orb_games_request_pause());
    orb_apps_poll(origin + 360000, false, true, 0, ay, az, true);
    render_now();
    CHECK(s_last_model.user_paused && s_last_model.ticks == paused_ticks);
    ui_action(ORB_GAME_GARDEN);
    orb_apps_poll(origin + 380000, false, true, 0, ay, az, true);
    render_now();
    CHECK(s_last_model.game.marble.garden_open && s_last_model.user_paused);
    ui_action(ORB_GAME_GARDEN);
    ui_action(ORB_GAME_PAUSE);
    orb_apps_poll(origin + 1000000, false, true, 0, ay, az, true);
    render_now();
    CHECK(!s_last_model.user_paused && !s_last_model.game.marble.garden_open);
    CHECK(s_last_model.ticks == paused_ticks);
    for (unsigned i = 1; i < 15; ++i) {
        orb_apps_poll(origin + 1000000 + (int64_t)i * 20000,
            false, true, 0, ay, az, true);
        render_now();
        CHECK(s_last_model.ticks > paused_ticks); /* Resume preserves the neutral; no 300 ms recapture. */
    }
    orb_apps_poll(origin + 1300000, false, true, 0, ay, az, true);
    render_now();
    CHECK(s_last_model.ticks > paused_ticks + 1 && fabsf(s_last_gx) < .001f && fabsf(s_last_gy) < .001f);
    /* A failed sensor never strands the player at calibration or stops the
     * fixed-step clock: touch takes over, with a single short-lived hint. */
    orb_apps_poll(origin + 1320000, false, true, 0, 0, 0, false);
    render_now();
    CHECK(s_last_model.controls && s_last_model.touch_fallback_hint);
    CHECK(s_last_model.ticks > paused_ticks + 1);
    orb_apps_poll(origin + 4320000, false, true, 0, ay, az, true);
    render_now();
    CHECK(s_last_model.controls && !s_last_model.touch_fallback_hint);
    ui_action(ORB_GAME_CONTROLS);
    orb_apps_poll(origin + 4340000, false, true, 0, 0, 0, false);
    render_now();
    CHECK(s_last_model.controls && !s_last_model.touch_fallback_hint);
    CHECK(s_allocations == 1 && !s_writes && !s_garden_writes);
    CHECK(orb_games_request_pause());
    orb_apps_poll(origin + 4360000, false, true, 0, ay, az, true);
    render_now();
    CHECK(s_last_model.user_paused && orb_games_request_back());
    orb_apps_poll(origin + 4380000, false, true, 0, ay, az, true);
    CHECK(!orb_apps_active() && !orb_games_request_pause() && !orb_games_request_back());

    CHECK(orb_apps_enter("marble") == ESP_OK);
    CHECK(s_last_model.started && !s_last_model.user_paused && !s_last_model.ticks);
    orb_apps_poll(origin + 5000000, false, true, 0, ay, az, true);
    orb_apps_poll(origin + 5100000, false, true, 0, ay, az, true);
    /* A long gap is neither a 300 ms average nor six valid catch-up samples. */
    orb_apps_poll(origin + 5700000, false, true, 0, ay, az, true);
    for (unsigned i = 1; i < 15; ++i) {
        orb_apps_poll(origin + 5700000 + (int64_t)i * 20000,
            false, true, 0, ay, az, true);
        render_now();
        CHECK(!s_last_model.ticks);
    }
    orb_apps_poll(origin + 6000000, false, true, 0, ay, az, true);
    render_now();
    CHECK(s_last_model.ticks == 1);
    orb_apps_poll(origin + 6020000, false, false, 0, ay, az, true);
    orb_apps_poll(origin + 7000000, false, true, 0, ay, az, true);
    for (unsigned i = 1; i < 15; ++i) {
        orb_apps_poll(origin + 7000000 + (int64_t)i * 20000,
            false, true, 0, ay, az, true);
        render_now();
        CHECK(s_last_model.ticks > 1 && s_last_model.ticks <= 20); /* Wake preserves neutral, without sleep catch-up. */
    }
    orb_apps_poll(origin + 7300000, false, true, 0, ay, az, true);
    render_now();
    CHECK(s_last_model.ticks > 2 && fabsf(s_last_gx) < .001f && fabsf(s_last_gy) < .001f);
    orb_apps_exit();

    CHECK(orb_apps_enter("marble") == ESP_OK);
    poll_launch(); render_now(); /* No IMU at entry immediately offers touch. */
    CHECK(s_last_model.controls && s_last_model.touch_fallback_hint);
    CHECK(s_last_model.started && !s_last_model.user_paused);
    orb_apps_exit();
}

static void shared_tilt_lifecycle(void)
{
    const char *ids[] = {"marble", "beads", "ring-snake"};
    for (unsigned game = 0; game < 3; ++game) {
        initialize(ORB_APP_NEEDS_STORAGE | ORB_APP_NEEDS_IMU);
        CHECK(!orb_games_motion_active());
        CHECK(orb_apps_enter(ids[game]) == ESP_OK);
        CHECK(orb_games_motion_active());
        /* Marble/Snake capture one round-start pose. Beads is physical gravity
         * and starts immediately, without capturing a play neutral. */
        for (unsigned i = 0; i <= 32; ++i) {
            orb_apps_poll((int64_t)i * 10000, false, true, 0, 0, 1, true);
            render_now();
            if (game != 1 && i < 30) CHECK(!s_last_model.ticks);
        }
        CHECK(s_last_model.ticks && !s_last_model.tilt_calibrating);
        CHECK(fabsf(s_last_gx) < .001f && fabsf(s_last_gy) < .001f);
        orb_apps_poll(325000, false, true, .01f, .02f, .08f, true);
        render_now();
        CHECK(!s_last_model.controls && !s_last_model.motion_unavailable &&
              !s_last_model.tilt_calibrating);
        for (unsigned i = 33; i <= 1040; ++i)
            orb_apps_poll((int64_t)i * 10000, false, true, -.2f, 0, .9797959f, true);
        render_now();
        CHECK(s_last_gx < -.02f && fabsf(s_last_gy) < .002f);
        CHECK(orb_games_request_pause());
        orb_apps_poll(10410000, false, true, -.2f, 0, .9797959f, true);
        render_now();
        CHECK(!orb_games_motion_active() && s_last_model.user_paused);
        uint32_t ticks = s_last_model.ticks;
        ui_action(ORB_GAME_PAUSE);
        for (unsigned i = 0; i <= 30; ++i)
            orb_apps_poll(10500000 + (int64_t)i * 10000, false, true, -.2f, 0, .9797959f, true);
        render_now();
        CHECK(orb_games_motion_active() && s_last_model.ticks > ticks && s_last_gx < -.02f);
        ticks = s_last_model.ticks;
        orb_apps_poll(10840000, false, true, 0, 0, 0, false);
        render_now();
        CHECK(s_last_model.motion_unavailable);
        CHECK(s_last_model.ticks >= ticks && s_last_model.controls && !orb_games_motion_active());
        orb_apps_exit();
        CHECK(!orb_games_motion_active() && !s_live_allocations);
    }
}

static void bead_impacts_are_not_stillness(void)
{
    initialize(ORB_APP_NEEDS_IMU);
    CHECK(orb_apps_enter("beads") == ESP_OK);
    for (unsigned i = 0; i <= 32; ++i)
        orb_apps_poll((int64_t)i * 10000, false, true, .025f, .036f, .924f, true);
    render_now();
    uint32_t ticks = s_last_model.ticks;
    CHECK(ticks > 0);
    for (unsigned i = 33; i <= 333; ++i)
        orb_apps_poll((int64_t)i * 10000, false, true, .01f, .02f, .08f, true);
    render_now();
    CHECK(s_last_model.ticks == ticks && !s_last_model.controls &&
          !s_last_model.motion_unavailable && !s_last_model.tilt_calibrating);
    orb_apps_poll(3340000, false, true, .025f, .036f, .924f, true);
    orb_apps_poll(3350000, false, true, .025f, .036f, .924f, true);
    render_now();
    CHECK(s_last_model.ticks > ticks && s_last_gx > .02f && s_last_gy > .03f);
    orb_apps_exit();
}

static void bead_is_silent(void)
{
    initialize(ORB_APP_NEEDS_IMU);
    CHECK(orb_apps_enter("beads") == ESP_OK);
    unsigned allocations = s_allocations;
    for (unsigned i = 0; i <= 900; ++i) {
        s_bead_flow = i < 300 ? 300 : i < 600 ? 80 : 0;
        orb_apps_poll((int64_t)i * 10000, false, true, i < 450 ? -.35f : .35f, 0, .93675f, true);
    }
    CHECK(orb_games_request_pause());
    orb_apps_poll(9010000, false, true, 0, 0, 1, true);
    ui_action(ORB_GAME_PAUSE);
    orb_apps_poll(9020000, false, true, 0, 0, 1, true);
    CHECK(s_allocations == allocations && !s_writes);
    orb_apps_exit();
    for (unsigned cue = 0; cue < ORB_SOUND_CUE_COUNT; ++cue) CHECK(s_sound_counts[cue] == 0);
}

static void marble_end_round_result(void)
{
    initialize(ORB_APP_NEEDS_STORAGE);
    CHECK(orb_apps_enter("marble") == ESP_OK);
    poll_launch();
    ui_action(ORB_GAME_FINISH); poll(1, true); render_now();
    CHECK(orb_apps_active() && !s_last_model.game_over && !s_last_model.user_paused);
    ui_action(ORB_GAME_PRIMARY); poll(2, true);
    ui_action(ORB_GAME_REWIND_START); poll(3, true);
    ui_action(ORB_GAME_PAUSE); poll(4, true); render_now();
    CHECK(s_last_model.user_paused && !s_last_model.game.marble.rewind_active);
    ui_action(ORB_GAME_FINISH); poll(5, true); render_now();
    CHECK(orb_apps_active() && s_last_model.game_over && !s_last_model.user_paused);
    CHECK(!s_last_model.game.marble.rewind_active && !s_writes);
    poll(6, true); render_now();
    CHECK(orb_apps_active() && s_timer.alive && s_view.alive && !s_writes);
    /* End round shares the scored result template. It retains the model for
     * Garden/replay; only explicit Done leaves and performs the atomic save. */
    ui_action(ORB_GAME_GARDEN); poll(7, true); render_now();
    CHECK(s_last_model.game.marble.garden_open && s_last_model.game_over);
    ui_action(ORB_GAME_GARDEN); poll(8, true); render_now();
    CHECK(!s_last_model.game.marble.garden_open && s_last_model.game_over && !s_writes);
    ui_action(ORB_GAME_CLOSE); poll(9, true);
    CHECK(!orb_apps_active() && !s_timer.alive && !s_view.alive);
    CHECK(s_writes == 1 && record_score(ORB_GAME_MARBLE) == 10);
    CHECK(s_live_allocations == 0 && orb_games_last_save_error() == ESP_OK);
}

static void picker_progress(void)
{
    initialize(ORB_APP_NEEDS_STORAGE | ORB_APP_NEEDS_IMU);
    char text[48];
    for (unsigned i = 0; i < ORB_GAME_COUNT; i++) {
        orb_games_progress((orb_game_kind_t)i, text, sizeof(text));
        CHECK(!text[0]);
    }
    CHECK(s_reads == 0 && s_garden_reads == 0); /* UI reads are memory-only. */
    CHECK(orb_apps_enter("orbit") == ESP_OK);
    poll_launch();
    ui_action(ORB_GAME_PRIMARY); poll(1, true);
    orb_apps_exit();
    orb_games_progress(ORB_GAME_ORBIT, text, sizeof(text));
    CHECK(!strcmp(text, "Best 10"));
    unsigned reads = s_reads;
    orb_games_progress(ORB_GAME_ORBIT, text, sizeof(text));
    CHECK(s_reads == reads);
    s_records[ORB_GAME_ORBIT].bytes[8] ^= 1;
    orb_apps_init(ORB_APP_NEEDS_STORAGE | ORB_APP_NEEDS_IMU);
    orb_games_progress(ORB_GAME_ORBIT, text, sizeof(text));
    CHECK(!text[0]); /* Bad checksum never becomes an invented best. */
    orb_keepsake_record_t record = {.magic = {'O', 'G', 'K', '1'}, .version = 1,
        .count = 1, .stages_cleared = 2, .items = {{.kind = 1, .stage = 2}}};
    s_garden.size = orb_keepsake_encode(&record, s_garden.bytes, sizeof(s_garden.bytes));
    s_garden.exists = true;
    orb_apps_init(ORB_APP_NEEDS_STORAGE | ORB_APP_NEEDS_IMU);
    orb_games_progress(ORB_GAME_MARBLE, text, sizeof(text));
    CHECK(!strcmp(text, "2/3 stages · 1 keepsake"));
    s_garden.bytes[0] ^= 1;
    orb_apps_init(ORB_APP_NEEDS_STORAGE | ORB_APP_NEEDS_IMU);
    orb_games_progress(ORB_GAME_MARBLE, text, sizeof(text));
    CHECK(!text[0]);
}

static void snake_hold_and_record_reset(void)
{
    initialize(ORB_APP_NEEDS_STORAGE);
    /* A genuine legacy OGS1 record must not enter the new rules' best score. */
    const uint8_t legacy[] = {'O', 'G', 'S', '1', 99, 0, 0, 0, 0x52, 0x42, 0x52, 0x4f};
    memcpy(s_legacy_snake.bytes, legacy, sizeof(legacy));
    s_legacy_snake.size = sizeof(legacy);
    s_legacy_snake.exists = true;
    CHECK(orb_apps_enter("ring-snake") == ESP_OK);
    poll_launch();
    CHECK(s_last_high == 0 && !s_last_failed);
    ui_action(ORB_GAME_STEER_START); poll(0, true); render_now();
    CHECK(s_last_model.game.snake.touch_held);
    for (unsigned i = 0; i < 1000; ++i) ui_action(ORB_GAME_PRIMARY);
    ui_action(ORB_GAME_STEER_STOP); poll(1, true); render_now();
    CHECK(!s_last_model.game.snake.touch_held);
    ui_action(ORB_GAME_STEER_START); ui_action(ORB_GAME_STEER_STOP);
    poll(2, true); render_now();
    CHECK(!s_last_model.game.snake.touch_held);
    ui_action(ORB_GAME_STEER_START); poll(3, true); render_now();
    CHECK(s_last_model.game.snake.touch_held);
    poll(4, false); render_now();
    CHECK(s_last_paused && !s_last_model.game.snake.touch_held);
    ui_action(ORB_GAME_STEER_START);
    poll(10000000, true); render_now();
    CHECK(!s_last_model.game.snake.touch_held);
    ui_action(ORB_GAME_STEER_START); poll(10000001, true);
    ui_action(ORB_GAME_PAUSE); poll(10000002, true); render_now();
    CHECK(s_last_model.user_paused && !s_last_model.game.snake.touch_held);
    ui_action(ORB_GAME_PAUSE); poll(10000003, true); render_now();
    CHECK(!s_last_model.game.snake.touch_held);
    ui_action(ORB_GAME_STEER_START); poll(10000004, true);
    ui_action(ORB_GAME_CONTROLS); poll(10000005, true); render_now();
    CHECK(!s_last_model.game.snake.touch_held);
    uint32_t best = s_last_high;
    CHECK(best > 0);
    orb_apps_exit();
    CHECK(record_score(ORB_GAME_SNAKE) == best);
    CHECK(!memcmp(s_legacy_snake.bytes, legacy, sizeof(legacy)));
    CHECK(orb_apps_enter("ring-snake") == ESP_OK);
    render_now();
    CHECK(s_last_high == best && !s_last_failed);
    orb_apps_exit();
}

static void coherent_pose_publication(void)
{
    initialize(ORB_APP_NEEDS_STORAGE);
    CHECK(orb_apps_enter("orbit") == ESP_OK);
    poll_launch();
    s_step_interleave = true;
    s_interleaved_angle = 0;
    poll(ORB_GAME_STEP_US, true);
    render_now();
    CHECK(s_last_model.ticks == 1 && s_last_model.game.orbit.angle == 0);
    /* Interrupt main twice in each of two catch-up steps. Every UI sample
     * must retain the old complete model/pose/time publication, even after
     * main has changed its next pose or partially advanced the live model. */
    poll(ORB_GAME_STEP_US * 3, true);
    s_step_interleave = false;
    render_now();
    CHECK(s_last_model.ticks == 3 && fabsf(s_last_model.game.orbit.angle - .24f) < .00001f);
    orb_apps_exit();
}

static void gated_presentation(void)
{
    initialize(ORB_APP_NEEDS_STORAGE);
    s_event_fail = true;
    CHECK(orb_apps_enter("orbit") == ESP_ERR_NO_MEM);
    CHECK(!s_refresh_event.alive && !s_timer.alive && !s_view.alive && !s_live_allocations);
    s_event_fail = false;
    s_ready_event_fail = true;
    CHECK(orb_apps_enter("orbit") == ESP_ERR_NO_MEM);
    CHECK(!s_refresh_event.alive && !s_refresh_ready_event.alive && !s_view.alive && !s_live_allocations);
    s_ready_event_fail = false;
    CHECK(orb_apps_enter("orbit") == ESP_OK);
    CHECK(s_refresh_event.alive);
    poll_launch();
    unsigned clean_updates = s_view_updates;
    lv_lock();
    struct lv_event_t clean = {s_refresh_event.context, LV_EVENT_REFR_START};
    s_refresh_event.callback(&clean);
    lv_unlock();
    CHECK(s_view_updates == clean_updates); /* Clean refresh attempts do not arm motion. */
    poll(ORB_GAME_STEP_US, true);
    render_now();
    CHECK(s_last_model.ticks == 1);
    s_display.pending = true;
    unsigned before = s_view_updates;
    for (unsigned i = 2; i <= 5; ++i) {
        poll(ORB_GAME_STEP_US * i, true);
        render_now();
    }
    CHECK(s_view_updates == before && s_last_model.ticks == 1);
    lv_lock();
    struct lv_event_t event = {s_refresh_event.context, LV_EVENT_REFR_START};
    s_refresh_event.callback(&event);
    lv_unlock();
    CHECK(s_view_updates == before + 1 && s_last_model.ticks == 5);
    unsigned armed = s_timer_ready;
    struct lv_event_t ready = {s_refresh_ready_event.context, LV_EVENT_REFR_READY};
    lv_lock();
    s_refresh_ready_event.callback(&ready);
    lv_unlock();
    CHECK(s_timer_ready == armed + 1);
    lv_lock();
    s_refresh_ready_event.callback(&ready);
    s_display.pending = false;
    s_refresh_event.callback(&clean);
    s_refresh_ready_event.callback(&ready);
    lv_unlock();
    CHECK(s_timer_ready == armed + 1); /* No clean READY self-rearming loop. */
    /* A hidden game's timer and refresh callback must not damage the shell. */
    s_display.pending = false;
    lv_lock();
    lv_screen_load(&s_original_screen);
    lv_unlock();
    before = s_view_updates;
    render_now();
    lv_lock();
    s_refresh_event.callback(&event);
    lv_screen_load(&s_game_screen);
    lv_unlock();
    CHECK(s_view_updates == before);
    orb_apps_exit();
    CHECK(!s_refresh_event.alive);
}

static void rewind_pose_publication(void)
{
    /* Games 4 interpolates the preceding fixed step. Preserve its held first
     * rewind half and ordinary following blend; do not forecast history. */
    initialize(ORB_APP_NEEDS_STORAGE);
    s_real_rewind_fixture = true;
    CHECK(orb_apps_enter("marble") == ESP_OK);
    poll_launch();
    ui_action(ORB_GAME_REWIND_START);
    poll(0, true);
    poll(ORB_GAME_STEP_US, true);
    render_now();
    CHECK(s_last_model.game.marble.x == 20 && s_last_model.game.marble.history_count == 3);
    poll(ORB_GAME_STEP_US + ORB_GAME_STEP_US / 2, true);
    render_now();
    CHECK(s_last_model.game.marble.x == 20);
    poll(ORB_GAME_STEP_US * 2, true);
    render_now();
    CHECK(s_last_model.game.marble.x == 20 && s_last_model.game.marble.history_count == 2);
    CHECK(s_last_model.game.marble.history[1].x == 0); /* Private history was not published. */
    poll(ORB_GAME_STEP_US * 2 + ORB_GAME_STEP_US / 2, true);
    render_now();
    CHECK(fabsf(s_last_model.game.marble.x - 15) < .001f);
    orb_apps_exit();
    s_real_rewind_fixture = false;
}

static void panel_rate_without_te(void)
{
    initialize(ORB_APP_NEEDS_STORAGE);
    s_te_pacing = false;
    CHECK(orb_apps_enter("orbit") == ESP_OK);
    poll_launch();
    unsigned initial = s_view_updates;
    /* Hundreds of early callbacks and READYs cannot manufacture extra frames. */
    for (unsigned tick = 0; tick < 17; ++tick) {
        s_ui_now_us = (int64_t)tick * 1000;
        render_now();
    }
    CHECK(s_view_updates == initial);
    s_ui_now_us = 17000;
    render_now();
    CHECK(s_view_updates == initial + 1 && s_timer.period == 17 && s_timer.reset_us == 17000);
    s_display.pending = true;
    struct lv_event_t start = {s_refresh_event.context, LV_EVENT_REFR_START};
    struct lv_event_t ready = {s_refresh_ready_event.context, LV_EVENT_REFR_READY};
    lv_lock();
    s_refresh_event.callback(&start);
    s_refresh_ready_event.callback(&ready);
    lv_unlock();
    CHECK(s_timer_ready == 0);
    s_display.pending = false;
    unsigned sampled = s_view_updates;
    /* Absolute 16.920ms phase gives 16/17ms intervals after quantization,
     * rather than accumulating to repeated 16/33ms physics/render beats. */
    unsigned frames = 1;
    for (unsigned tick = 18; tick <= 1692; ++tick) {
        s_ui_now_us = (int64_t)tick * 1000;
        unsigned before = s_view_updates;
        render_now();
        if (s_view_updates != before) ++frames;
    }
    CHECK(frames == 100 && s_view_updates == sampled + 99);
    /* A slow preparation resets the timer at its end, without changing the
     * absolute phase. A missed period then produces only one newest pose. */
    s_ui_now_us = 1710000;
    s_view_work_us = 4000;
    render_now();
    CHECK(s_timer.reset_us == 1714000 && s_timer.period == 12);
    s_view_work_us = 0;
    s_ui_now_us = 1900000;
    unsigned before = s_view_updates;
    render_now(); render_now(); render_now();
    CHECK(s_view_updates == before + 1);
    orb_apps_exit();
    CHECK(!s_refresh_event.alive && !s_refresh_ready_event.alive && !s_timer.alive && !s_live_allocations);
}

int main(void)
{
    panel_rate_without_te();
    coherent_pose_publication();
    gated_presentation();
    rewind_pose_publication();
    picker_progress();
    catalogue_and_requests();
    fixed_step_and_pause();
    power_exit();
    bounded_actions();
    per_game_storage();
    partial_entry_cleanup();
    benchmark_ownership();
    user_pause_and_sound();
    game_over_restart_and_done();
    garden_lifecycle();
    full_garden_commit();
    rewind_release_and_interruption();
    snake_hold_and_record_reset();
    tilt_calibration_fallback_and_system_requests();
    shared_tilt_lifecycle();
    bead_impacts_are_not_stillness();
    bead_is_silent();
    marble_end_round_result();
    CHECK(s_active_screen == &s_original_screen && !s_game_screen.alive);
    CHECK(!orb_apps_active() && s_live_allocations == 0 && s_lock_depth == 0);
    printf("game runtime: %u checks; allocation-free frames; bounded queues; stage keepsakes; lossless rewind release\n", s_checks);
    return 0;
}
