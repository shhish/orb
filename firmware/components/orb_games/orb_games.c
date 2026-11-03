#include "orb_color_tokens.h"
#include "orb_games.h"
#include "orb_games_presentation.h"
#include "orb_apps.h"
#include "orb_fs.h"
#include "orb_game_logic.h"
#include "orb_game_interpolate.h"
#include "orb_game_view.h"
#include "orb_sound.h"
#include "orb_ui.h"
#include "orb_display.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "orb_memory.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#endif

#define ACTIONS 16u
#define CATCHUP_STEPS 6u
#define MARBLE_SNAPSHOT_BYTES offsetof(orb_game_state_t, game.marble.history)
#define BEADS_SNAPSHOT_BYTES (offsetof(orb_game_state_t, game) + sizeof(((orb_game_state_t *)0)->game.beads))
#define SNAPSHOT_BYTES (MARBLE_SNAPSHOT_BYTES > BEADS_SNAPSHOT_BYTES ? MARBLE_SNAPSHOT_BYTES : BEADS_SNAPSHOT_BYTES)
_Static_assert(SNAPSHOT_BYTES >= offsetof(orb_game_state_t, game) + sizeof(((orb_game_state_t *)0)->game.orbit),
               "Orbit presentation fits the compact snapshot");
_Static_assert(SNAPSHOT_BYTES >= offsetof(orb_game_state_t, game) + sizeof(((orb_game_state_t *)0)->game.snake),
               "Snake presentation fits the compact snapshot");
_Static_assert(SNAPSHOT_BYTES >= offsetof(orb_game_state_t, game) + sizeof(((orb_game_state_t *)0)->game.pop),
               "Pop presentation fits the compact snapshot");

typedef struct { int action; float x, y; } game_action_t;
typedef struct {
    orb_game_state_t model, previous_model, published, published_previous, rendering;
    float published_alpha;
    orb_game_view_t *view;
    lv_obj_t *screen, *saved_screen;
    lv_timer_t *timer;
    void *presentation;
    lv_display_t *display;
    bool refresh_registered, refresh_had_damage;
    game_action_t actions[ACTIONS];
    unsigned read, write;
    int64_t previous, accumulator;
    int64_t next_present_us;
    uint32_t present_period_us;
    uint32_t high_score, saved_score;
    orb_game_tilt_t tilt;
    int64_t tilt_sample_previous;
    float tilt_x, tilt_y;
    int64_t fallback_hint_until;
    int64_t last_time_sec;
    bool fallback_hint_shown;
    bool paused, score_error, garden_error, garden_dirty, rewind_held, steer_held;
    float steer_x, steer_y;
    atomic_bool reduce_motion;
} game_runtime_t;

static game_runtime_t *s_game;
static const orb_games_presentation_t *s_presentation;
static atomic_int s_save_error;
static atomic_int s_owner; /* 0 idle, 1 game lifecycle, 2 isolated benchmark. */
static atomic_bool s_game_active, s_pause_request, s_back_available, s_motion_active;
static char s_progress[ORB_GAME_COUNT][48];
static void refresh_progress(orb_game_kind_t kind);

static bool uses_tilt(orb_game_kind_t kind)
{
    return kind == ORB_GAME_MARBLE || kind == ORB_GAME_BEADS || kind == ORB_GAME_SNAKE;
}

static void feedback(const game_runtime_t *g, orb_sound_cue_t cue)
{
    /* Beads is a silent physical toy, including its lifecycle (owner 10-28).
     * Keep the signed palette assets untouched. */
    if (g->model.kind != ORB_GAME_BEADS) (void)orb_sound_play(cue);
}

static void resume_motion(game_runtime_t *g)
{
    /* Resume keeps the round's neutral. Only the elapsed sample window is
     * discarded so a pause cannot manufacture stillness or catch-up input. */
    g->tilt_sample_previous = -1;
    g->tilt.still_us = 0;
    g->tilt.observed = g->tilt.gyro_observed = false;
    g->tilt_x = g->tilt_y = 0;
}
static void reset_tilt(game_runtime_t *g)
{
    orb_game_tilt_reset(&g->tilt);
    g->tilt_sample_previous = -1;
    g->tilt_x = g->tilt_y = 0;
}
#ifdef ESP_PLATFORM
static portMUX_TYPE s_mailbox = portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL(&s_mailbox)
#define UNLOCK() portEXIT_CRITICAL(&s_mailbox)
#else
/* Host runtime tests drive both tasks sequentially. Production has one tiny
 * mailbox critical section, not an LVGL lock around simulation. */
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#endif
static void action(int action_id, float x, float y, void *context)
{
    game_runtime_t *g = context;
    if (action_id == ORB_GAME_CLOSE) {
        orb_apps_request_exit(); /* Main consumes this even while paused. */
        return;
    }
    LOCK();
    /* Release cannot be lost behind a full steering mailbox. Hold state is
     * level-triggered, so a release before the next tick cancels the hold too. */
    if (action_id == ORB_GAME_REWIND_START || action_id == ORB_GAME_REWIND_STOP) {
        g->rewind_held = action_id == ORB_GAME_REWIND_START && !g->paused;
        UNLOCK();
        return;
    }
    if (action_id == ORB_GAME_STEER_START || action_id == ORB_GAME_STEER_STOP) {
        g->steer_held = action_id == ORB_GAME_STEER_START && !g->paused;
        g->steer_x = x;
        g->steer_y = y;
        UNLOCK();
        return;
    }
    unsigned next = (g->write + 1) % ACTIONS;
    if (!g->paused && next != g->read) {
        g->actions[g->write] = (game_action_t){action_id, x, y};
        g->write = next;
    }
    UNLOCK();
}
static int64_t presentation_now_us(void)
{
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#else
    /* The simulator and runtime tests advance LVGL's virtual clock. */
    return (int64_t)lv_tick_get() * 1000;
#endif
}
static void presentation_wait(game_runtime_t *g)
{
    int64_t remaining = g->next_present_us - presentation_now_us();
    lv_timer_set_period(g->timer, remaining > 1000 ? (uint32_t)((remaining + 999) / 1000) : 1);
    /* LVGL anchors last_run at callback entry. Reset after view preparation
     * so its elapsed work is not subtracted from this remaining delay twice. */
    lv_timer_reset(g->timer);
}
static void render_latest(game_runtime_t *g)
{
    LOCK();
    memcpy(&g->rendering, &g->published, orb_game_snapshot_bytes(g->published.kind));
    orb_game_interpolate(&g->published_previous, &g->rendering, g->published_alpha);
    bool paused = g->paused, failed = g->score_error || g->garden_error;
    uint32_t high_score = g->high_score;
    UNLOCK();
    atomic_store(&g->reduce_motion, orb_ui_reduce_motion());
    orb_game_view_update(g->view, &g->rendering, high_score, paused, failed);
}
static void refresh_event(lv_event_t *event)
{
    game_runtime_t *g = lv_event_get_user_data(event);
    LOCK(); bool paused = g->paused; UNLOCK();
    if (paused) { g->refresh_had_damage = false; return; }
    if (lv_event_get_code(event) == LV_EVENT_REFR_READY) {
        bool completed = g->refresh_had_damage;
        g->refresh_had_damage = false;
        /* Only a live TE gate can consume early producer credit safely.
         * Without that gate, READY must not turn fast games into a free-running
         * redraw loop. Clean/held-clock refreshes never rearm themselves. */
        if (completed && lv_screen_active() == g->screen && g->timer &&
            orb_display_te_pacing_active()) lv_timer_ready(g->timer);
        return;
    }
    /* The display has admitted this frame at its TE gate. Sample once more
     * before layout so a delayed slot presents the newest complete pose. */
    g->refresh_had_damage = lv_screen_active() == g->screen && orb_display_refresh_pending();
    if (g->refresh_had_damage) render_latest(g);
}
static void render(lv_timer_t *timer)
{
    game_runtime_t *g = lv_timer_get_user_data(timer);
    /* Preparing several intermediate views while the TE gate waits piles up
     * stale damage and can overflow LVGL's region queue. Actual unchanged
     * views still submit no damage, preserving the held clock's zero redraw. */
    if (lv_screen_active() != g->screen) return;
    int64_t now = presentation_now_us();
    bool due = now >= g->next_present_us;
    if (due) {
        /* Absolute fractional-period deadlines avoid accumulating millisecond
         * rounding and never catch up old poses in a burst after a slow frame. */
        uint64_t slots = (uint64_t)(now - g->next_present_us) / g->present_period_us + 1;
        g->next_present_us += (int64_t)(slots * g->present_period_us);
    }
    if ((due || orb_display_te_pacing_active()) && !orb_display_refresh_pending()) render_latest(g);
    presentation_wait(g);
}
static uint32_t decode(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void encode(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (i*8));
}

static const char *score_key(orb_game_kind_t kind)
{
    /* Continuous Snake has different rules. Preserve the legacy record but
     * start its new leaderboard cleanly; all new scores remain persistent. */
    return kind == ORB_GAME_SNAKE ? "high-score-v2" : "high-score";
}

static void refresh_progress(orb_game_kind_t kind)
{
    char text[48] = "";
    if (orb_fs_ready() && kind == ORB_GAME_MARBLE) {
        uint8_t bytes[ORB_KEEPSAKE_RECORD_SIZE];
        orb_keepsake_record_t record;
        size_t size = 0;
        if (orb_fs_read("marble", "keepsakes", bytes, sizeof(bytes), &size) == ESP_OK &&
            orb_keepsake_decode(bytes, size, &record)) {
            /* The save records completed stages, not a resumable current stage. */
            snprintf(text, sizeof(text), "%u/3 stages · %u keepsake%s",
                     record.stages_cleared, record.count, record.count == 1 ? "" : "s");
        }
    } else if (orb_fs_ready() && kind != ORB_GAME_BEADS) {
        uint8_t bytes[12];
        size_t size = 0;
        if (orb_fs_read(orb_game_id(kind), score_key(kind), bytes, sizeof(bytes), &size) == ESP_OK &&
            size == sizeof(bytes) && !memcmp(bytes, "OGS1", 4) &&
            decode(bytes + 8) == (decode(bytes + 4) ^ 0x4f524231u))
            snprintf(text, sizeof(text), "Best %lu", (unsigned long)decode(bytes + 4));
    }
    LOCK();
    memcpy(s_progress[kind], text, sizeof(text));
    UNLOCK();
}

void orb_games_progress(orb_game_kind_t kind, char *text, size_t capacity)
{
    if (!text || !capacity) return;
    text[0] = '\0';
    if ((unsigned)kind >= ORB_GAME_COUNT) return;
    LOCK();
    snprintf(text, capacity, "%s", s_progress[kind]);
    UNLOCK();
}

static void load_score(game_runtime_t *g)
{
    uint8_t record[12];
    size_t size = 0;
    esp_err_t err = orb_fs_read(orb_game_id(g->model.kind), score_key(g->model.kind), record, sizeof(record), &size);
    if (err == ESP_ERR_NOT_FOUND) return;
    if (err == ESP_OK && size == sizeof(record) && !memcmp(record, "OGS1", 4) &&
        decode(record+8) == (decode(record+4) ^ 0x4f524231u)) {
        g->high_score = g->saved_score = decode(record+4);
    } else g->score_error = true;
}

static void load_garden(game_runtime_t *g)
{
    if (g->model.kind != ORB_GAME_MARBLE) return;
    uint8_t bytes[ORB_KEEPSAKE_RECORD_SIZE];
    orb_keepsake_record_t record;
    size_t size = 0;
    esp_err_t err = orb_fs_read("marble", "keepsakes", bytes, sizeof(bytes), &size);
    if (err == ESP_ERR_NOT_FOUND) return;
    if (err != ESP_OK || !orb_keepsake_decode(bytes, size, &record)) {
        g->garden_error = true;
        return;
    }
    g->model.game.marble.keepsake_count = record.count;
    g->model.game.marble.stages_cleared = record.stages_cleared;
    for (unsigned i = 0; i < record.count; ++i) {
        g->model.game.marble.keepsakes[i] = (orb_marble_keepsake_t){
            record.items[i].kind, record.items[i].stage, record.items[i].x, record.items[i].y};
    }
}

static esp_err_t save_garden(game_runtime_t *g)
{
    if (g->model.kind != ORB_GAME_MARBLE || !g->garden_dirty) return ESP_OK;
    orb_keepsake_record_t record = {.magic = {'O', 'G', 'K', '1'}, .version = 1,
        .count = g->model.game.marble.keepsake_count,
        .stages_cleared = g->model.game.marble.stages_cleared};
    for (unsigned i = 0; i < record.count; ++i) {
        const orb_marble_keepsake_t *item = &g->model.game.marble.keepsakes[i];
        record.items[i].kind = item->kind;
        record.items[i].stage = item->stage;
        record.items[i].x = item->x;
        record.items[i].y = item->y;
    }
    uint8_t bytes[ORB_KEEPSAKE_RECORD_SIZE];
    size_t size = orb_keepsake_encode(&record, bytes, sizeof(bytes));
    esp_err_t err = size ? orb_fs_write("marble", "keepsakes", bytes, size) : ESP_ERR_INVALID_ARG;
    if (err == ESP_OK) g->garden_dirty = false;
    return err;
}

static void publish(game_runtime_t *g)
{
    /* The 2,400-byte history belongs only to fixed-step logic. The renderer
     * consumes four extracted trail points, keeping critical sections small.
     * Every other game union is smaller than this stable prefix. */
    memcpy(&g->published, &g->model, orb_game_snapshot_bytes(g->model.kind));
    memcpy(&g->published_previous, &g->previous_model, orb_game_snapshot_bytes(g->model.kind));
    g->published_alpha = (float)g->accumulator / (float)ORB_GAME_STEP_US;
    atomic_store(&s_back_available, g->paused || g->model.user_paused);
    atomic_store(&s_motion_active, uses_tilt(g->model.kind) && !g->paused &&
        !g->model.user_paused && !g->model.game_over && !g->model.controls &&
        (g->model.kind != ORB_GAME_MARBLE || !g->model.game.marble.stage_cleared));
}

static esp_err_t save_score(game_runtime_t *g)
{
    if (g->high_score <= g->saved_score) return ESP_OK;
    uint8_t record[12] = {'O', 'G', 'S', '1'};
    encode(record+4, g->high_score);
    encode(record+8, g->high_score ^ 0x4f524231u);
    /* Only lifecycle exit writes. Flash/fsync never runs in a frame/step or
     * while the LVGL lock is held; orb_fs supplies atomic LittleFS replacement. */
    return orb_fs_write(orb_game_id(g->model.kind), score_key(g->model.kind), record, sizeof(record));
}

/* UI lifecycle only; the render and simulation paths above remain unchanged. */
static bool presentation_create(lv_obj_t *parent, void *context)
{
    game_runtime_t *g = context;
    g->view = orb_game_view_create(parent, g->model.kind, action, g);
    if (g->view) {
        orb_game_view_update(g->view, &g->model, g->high_score, false,
                              g->score_error || g->garden_error);
        /* The producer retains an absolute panel-rate clock even with TE
         * admission disabled; fixed 60 Hz physics remains independent. */
        g->present_period_us = orb_display_panel_period_us();
        if (!g->present_period_us) g->present_period_us = ORB_GAME_STEP_US;
        g->next_present_us = presentation_now_us() + g->present_period_us;
        g->display = lv_display_get_default();
        g->refresh_registered = g->display && lv_display_add_event_cb(
            g->display, refresh_event, LV_EVENT_REFR_START, g) != NULL;
        if (g->refresh_registered && lv_display_add_event_cb(
            g->display, refresh_event, LV_EVENT_REFR_READY, g))
            g->timer = lv_timer_create(render, (g->present_period_us + 999) / 1000, g);
    }
    return g->view && g->timer;
}

static void presentation_stop(game_runtime_t *g)
{
    if (g->refresh_registered)
        lv_display_remove_event_cb_with_user_data(g->display, refresh_event, g);
    if (g->timer) lv_timer_delete(g->timer);
    g->timer = NULL;
    g->refresh_registered = false;
}

static void presentation_destroy(void *context)
{
    game_runtime_t *g = context;
    presentation_stop(g);
    orb_game_view_destroy(g->view);
    g->view = NULL;
}

bool orb_games_set_presentation(const orb_games_presentation_t *presentation)
{
    /* Main initializes the composition before admission. A live runtime's
     * cleanup port must never be replaced beneath its UI callbacks. */
    if (!presentation || !presentation->open || !presentation->close) return false;
    if (atomic_load(&s_owner)) return s_presentation == presentation;
    s_presentation = presentation;
    return true;
}

static esp_err_t enter_game(orb_game_kind_t kind)
{
    if (!s_presentation) return ESP_ERR_INVALID_STATE;
    int idle = 0;
    if (!atomic_compare_exchange_strong(&s_owner, &idle, 1)) return ESP_ERR_INVALID_STATE;
#ifdef ESP_PLATFORM
    game_runtime_t *g = orb_memory_calloc(ORB_MEMORY_GAMES, 1, sizeof(*g));
#else
    game_runtime_t *g = calloc(1, sizeof(*g));
#endif
    if (!g) { atomic_store(&s_owner, 0); return ESP_ERR_NO_MEM; }
    orb_game_init(&g->model, kind, 0x6f726231u);
    memcpy(&g->previous_model, &g->model, orb_game_snapshot_bytes(g->model.kind));
    /* The shell's Play picker is the attract screen; its launch action starts
     * the round without another Play confirmation (HIG §§1/7). */
    g->model.started = true;
    reset_tilt(g);
    g->previous = -1;
    g->last_time_sec = -1;
    load_score(g);
    load_garden(g);
    publish(g);
    lv_lock();
    atomic_init(&g->reduce_motion, orb_ui_reduce_motion());
    g->saved_screen = lv_screen_active();
    /* Keep the shell alive on its inactive screen. A top-layer overlay would
     * repaint the whole game whenever the shell clock/status invalidates; an
     * independent screen keeps moving-frame damage confined to game sprites. */
    g->screen = lv_obj_create(NULL);
    if (g->screen) g->presentation = s_presentation->open(g->screen,
        &(orb_games_view_port_t){.context = g, .create = presentation_create, .destroy = presentation_destroy});
    if (g->presentation) lv_screen_load(g->screen);
    lv_unlock();
    if (!g->presentation) {
        atomic_store(&s_motion_active, false);
        lv_lock();
        if (g->screen) lv_obj_delete(g->screen);
        lv_unlock();
#ifdef ESP_PLATFORM
        orb_memory_free(g);
#else
        free(g);
#endif
        atomic_store(&s_owner, 0);
        return ESP_ERR_NO_MEM;
    }
    s_game = g;
    atomic_store(&s_pause_request, false);
    atomic_store(&s_game_active, true);
    feedback(g, ORB_SOUND_OPEN);
    return ESP_OK;
}

static void leave_game(void)
{
    game_runtime_t *g = s_game;
    if (!g) return;
    s_game = NULL;
    atomic_store(&s_game_active, false);
    atomic_store(&s_motion_active, false);
    atomic_store(&s_pause_request, false);
    atomic_store(&s_back_available, false);
    lv_lock();
    presentation_stop(g);
    lv_screen_load(g->saved_screen);
    s_presentation->close(g->presentation);
    lv_obj_delete(g->screen);
    lv_unlock();
    esp_err_t garden_err = save_garden(g);
    esp_err_t err = save_score(g);
    if (garden_err != ESP_OK) err = garden_err;
    atomic_store(&s_save_error, err);
    refresh_progress(g->model.kind);
    feedback(g, ORB_SOUND_CLOSE);
#ifdef ESP_PLATFORM
    if (err != ESP_OK) ESP_LOGW("orb_games", "game save failed: %s", esp_err_to_name(err));
#endif
#ifdef ESP_PLATFORM
    orb_memory_free(g);
#else
    free(g);
#endif
    atomic_store(&s_owner, 0);
}
static void pause_game(bool paused)
{
    game_runtime_t *g = s_game;
    if (!g) return;
    LOCK();
    bool changed = g->paused != paused;
    g->paused = paused;
    g->read = g->write = 0;
    g->rewind_held = false;
    g->steer_held = false;
    UNLOCK();
    if (g->model.kind == ORB_GAME_MARBLE && g->model.game.marble.rewind_active)
        orb_game_action(&g->model, ORB_GAME_REWIND_STOP, 0, 0);
    if (g->model.kind == ORB_GAME_SNAKE || g->model.kind == ORB_GAME_BEADS)
        orb_game_action(&g->model, ORB_GAME_STEER_STOP, 0, 0);
    LOCK();
    publish(g);
    UNLOCK();
    g->previous = -1;
    g->accumulator = 0; /* Never catch up screen-off time. */
    if (!paused) resume_motion(g);
    if (changed) {
        lv_lock();
        /* The retained game must not prepare frames beneath the calm root. */
        if (paused) lv_timer_pause(g->timer);
        else { g->next_present_us = presentation_now_us(); lv_timer_resume(g->timer); lv_timer_ready(g->timer); }
        render_latest(g);
        lv_unlock();
    }
    if (changed && g->model.started && !g->model.game_over && !g->model.user_paused)
        feedback(g, paused ? ORB_SOUND_OPEN : ORB_SOUND_CLOSE);
}

static bool tick_motion(game_runtime_t *g, const orb_app_input_t *input,
    bool old_started, bool old_user_paused, bool old_controls, bool restarted,
    float *out_x, float *out_y)
{
    float accel_x = input->accel_x, accel_y = input->accel_y;
    bool calibrating = false;
    if (uses_tilt(g->model.kind)) {
        if (g->model.kind == ORB_GAME_SNAKE && old_user_paused &&
            !g->model.user_paused && g->model.motion_unavailable)
            orb_game_action(&g->model, ORB_GAME_CONTROLS, 0, 0);
        if ((!old_started && g->model.started) || restarted)
            reset_tilt(g);
        else if ((old_user_paused && !g->model.user_paused) ||
                 (old_controls && !g->model.controls)) resume_motion(g);
        bool playing = g->model.started && !g->model.user_paused && !g->model.game_over &&
                       (g->model.kind != ORB_GAME_MARBLE || !g->model.game.marble.stage_cleared);
        g->model.tilt_calibrating = false;
        if (playing && !g->model.controls) {
            float magnitude = sqrtf(input->accel_x * input->accel_x +
                input->accel_y * input->accel_y + input->accel_z * input->accel_z);
            /* A finite free-fall/impact sample is rejected by the shared
             * filter, not mistaken for a missing sensor that changes modes. */
            bool unavailable = !input->motion_valid || !isfinite(magnitude);
            if (unavailable) {
                /* Motion failure changes the input mode through the ordinary
                 * action so its geometry/history rules stay consistent. */
                orb_game_action(&g->model, ORB_GAME_CONTROLS, 0, 0);
                g->model.motion_unavailable = true;
                if (g->model.kind != ORB_GAME_BEADS && !g->fallback_hint_shown) {
                    g->fallback_hint_shown = true;
                    g->fallback_hint_until = input->now_us + 3000000;
                }
                accel_x = accel_y = 0;
                resume_motion(g);
                /* The same visible touch alternative remains usable when the
                 * sensor is unavailable; no fabricated gravity is supplied. */
                calibrating = false;
            } else {
                g->model.motion_unavailable = false;
                int64_t sampled = input->motion_sample_us;
                if (sampled < g->tilt_sample_previous) resume_motion(g);
                bool sample_ok = g->tilt.ready;
                if (g->tilt_sample_previous >= 0 && sampled > g->tilt_sample_previous) {
                    int64_t elapsed = sampled - g->tilt_sample_previous;
                    uint32_t sample_us = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
                    const float gyro[] = {input->gyro_x, input->gyro_y, input->gyro_z};
                    sample_ok = g->model.kind == ORB_GAME_BEADS ?
                        orb_game_gravity_update_motion(&g->tilt,
                            input->accel_x, input->accel_y, input->accel_z,
                            input->gyro_valid ? gyro : NULL, sample_us, &g->tilt_x, &g->tilt_y) :
                        orb_game_tilt_update_motion(&g->tilt,
                            input->accel_x, input->accel_y, input->accel_z,
                            input->gyro_valid ? gyro : NULL, sample_us, &g->tilt_x, &g->tilt_y);
                }
                g->tilt_sample_previous = sampled;
                accel_x = g->tilt_x;
                accel_y = g->tilt_y;
                /* An impact is not stillness: it must not advance Beads'
                 * clock latch. Marble/Snake can coast with zero force. */
                calibrating = !g->tilt.ready || (g->model.kind == ORB_GAME_BEADS && !sample_ok);
                /* Beads uses the board frame and never captures a neutral: its
                 * first sample only waits for a second timestamp. Reporting
                 * that frame as calibration would flash the shared frozen
                 * "Hold still" overlay over the pool at every round start. */
                g->model.tilt_calibrating = !g->tilt.ready && g->model.kind != ORB_GAME_BEADS;
                orb_game_set_motion(&g->model, &g->tilt);
            }
        }
        g->model.touch_fallback_hint = input->now_us < g->fallback_hint_until;
        if (calibrating) {
            /* Capture the held viewing angle without consuming physics time,
             * turning gates, advancing Snake, or moving Beads during the
             * 300 ms neutral average. Catchup never duplicates IMU samples. */
            g->previous = input->now_us;
            g->accumulator = 0;
        }
    }
    *out_x = accel_x;
    *out_y = accel_y;
    return calibrating;
}

static void tick_clock(game_runtime_t *g, const orb_app_input_t *input)
{
    if (g->model.kind == ORB_GAME_BEADS) {
        /* Beads settles into the wall clock; inject once per second so a minute
         * boundary is noticed without polling libc every fixed step. */
        int64_t sec = input->now_us / 1000000;
        if (sec != g->last_time_sec) {
            g->last_time_sec = sec;
            time_t wall = time(NULL);
            struct tm local;
            /* Before SNTP/retained time is available, stay a quiet fluid toy;
             * never assemble 1970 or the design fixture's clock as real data. */
            if (wall >= 1577836800 && localtime_r(&wall, &local))
                orb_game_beads_set_time(&g->model, (uint8_t)local.tm_hour, (uint8_t)local.tm_min);
        }
    }
}

static void tick_game(const orb_app_input_t *input)
{
    game_runtime_t *g = s_game;
    if (!g) return;
    /* The model is exclusively main-owned. UI owns only a published copy and
     * a bounded action mailbox. Neither frame path allocates or does I/O. */
    game_action_t pending[ACTIONS];
    unsigned count = 0;
    uint32_t old_score = g->model.score;
    uint8_t old_lives = g->model.lives;
    bool old_started = g->model.started, old_over = g->model.game_over;
    bool old_user_paused = g->model.user_paused;
    bool old_controls = g->model.controls;
    uint8_t old_ring = g->model.kind == ORB_GAME_ORBIT ? g->model.game.orbit.ring : 0;
    bool old_stage_cleared = g->model.kind == ORB_GAME_MARBLE && g->model.game.marble.stage_cleared;
    uint8_t old_stage = g->model.kind == ORB_GAME_MARBLE ? g->model.game.marble.stage : 0;
    bool restarted = false, ended = false;
    LOCK();
    while (g->read != g->write) {
        pending[count++] = g->actions[g->read];
        g->read = (g->read+1) % ACTIONS;
    }
    bool rewind_held = g->rewind_held;
    bool steer_held = g->steer_held;
    float steer_x = g->steer_x, steer_y = g->steer_y;
    UNLOCK();
    for (unsigned i = 0; i < count; ++i) {
        if (pending[i].action == ORB_GAME_RESTART) restarted = true;
        bool finish = pending[i].action == ORB_GAME_FINISH &&
            g->model.started && g->model.user_paused && !g->model.game_over;
        ended |= finish;
        orb_game_action(&g->model, (orb_game_action_t)pending[i].action, pending[i].x, pending[i].y);
    }
    if (atomic_exchange(&s_pause_request, false) && g->model.started &&
        !g->model.game_over && !g->model.user_paused)
        orb_game_action(&g->model, ORB_GAME_PAUSE, 0, 0);
    if (g->model.kind == ORB_GAME_MARBLE) {
        if (g->model.user_paused || g->model.game_over || g->model.game.marble.stage_cleared)
            rewind_held = false;
        if (rewind_held != g->model.game.marble.rewind_active)
            orb_game_action(&g->model, rewind_held ? ORB_GAME_REWIND_START : ORB_GAME_REWIND_STOP, 0, 0);
    }
    if (g->model.kind == ORB_GAME_SNAKE) {
        if (g->model.user_paused || g->model.game_over || restarted || old_controls != g->model.controls)
            steer_held = false;
        orb_game_action(&g->model, steer_held ? ORB_GAME_STEER_START : ORB_GAME_STEER_STOP,
                        steer_x, steer_y);
    }
    if (g->model.kind == ORB_GAME_BEADS) {
        if (g->model.user_paused || restarted || old_controls != g->model.controls)
            steer_held = false;
        orb_game_action(&g->model, steer_held ? ORB_GAME_STEER_START : ORB_GAME_STEER_STOP,
                        steer_x, steer_y);
    }
    if (old_started != g->model.started || old_user_paused != g->model.user_paused ||
        restarted) {
        g->previous = -1;
        g->accumulator = 0;
    }
    float accel_x, accel_y;
    bool calibrating = tick_motion(g, input, old_started, old_user_paused, old_controls,
                                   restarted, &accel_x, &accel_y);
    tick_clock(g, input);
    if (g->previous >= 0 && input->now_us > g->previous) {
        int64_t elapsed = input->now_us - g->previous;
        if (elapsed > ORB_GAME_STEP_US * CATCHUP_STEPS) elapsed = ORB_GAME_STEP_US * CATCHUP_STEPS;
        g->accumulator += elapsed;
    }
    g->previous = input->now_us;
    unsigned steps = 0;
    while (!calibrating && g->accumulator >= ORB_GAME_STEP_US && steps++ < CATCHUP_STEPS) {
        memcpy(&g->previous_model, &g->model, orb_game_snapshot_bytes(g->model.kind));
        orb_game_step(&g->model, accel_x, accel_y, input->motion_valid,
                      atomic_load(&g->reduce_motion));
        g->accumulator -= ORB_GAME_STEP_US;
    }
    /* A completed stage is a stationary receipt. Its one atomic write is a
     * lifecycle event outside LVGL/publication locks, never a moving frame.
     * Failure remains dirty for the next stage or exit, avoiding frame retries. */
    if (g->model.kind == ORB_GAME_MARBLE && g->model.game.marble.stage_cleared &&
        (!old_stage_cleared || old_stage != g->model.game.marble.stage)) {
        g->garden_dirty = true;
        esp_err_t err = save_garden(g);
        LOCK();
        g->garden_error = err != ESP_OK;
        UNLOCK();
        atomic_store(&s_save_error, err);
    }
    LOCK();
    if (g->model.kind == ORB_GAME_MARBLE && (g->model.user_paused ||
        g->model.game_over || g->model.game.marble.stage_cleared))
        g->rewind_held = false;
    if ((g->model.kind == ORB_GAME_SNAKE || g->model.kind == ORB_GAME_BEADS) &&
        (!steer_held || g->model.user_paused || g->model.game_over))
        g->steer_held = false;
    publish(g);
    if (g->model.score > g->high_score) g->high_score = g->model.score;
    UNLOCK();
    /* HIG §8: every cue accompanies a visible event. The shared sound service
     * owns Silent/face-down/capture gates; dropped cues are never retried.
     * Calls stay outside both LVGL and the short model publication lock. */
    if ((!old_started && g->model.started) || restarted) feedback(g, ORB_SOUND_OPEN);
    else if (!old_over && g->model.game_over) {
        /* An explicit End Round is neither a win nor a loss. Pop's timed
         * completion and Marble's final flower are successes; Snake/Orbit
         * collisions are failures. Never celebrate a lost round (§8). */
        bool completed = g->model.kind == ORB_GAME_MARBLE ||
            (g->model.kind == ORB_GAME_SNAKE && g->model.game.snake.won) ||
            (g->model.kind == ORB_GAME_POP && g->model.lives > 0);
        if (!ended || g->model.kind != ORB_GAME_MARBLE)
            feedback(g, ended ? ORB_SOUND_CLOSE : completed ? ORB_SOUND_DONE : ORB_SOUND_FAILED);
    } else if (old_user_paused != g->model.user_paused)
        feedback(g, g->model.user_paused ? ORB_SOUND_OPEN : ORB_SOUND_CLOSE);
    else if (g->model.lives < old_lives) feedback(g, ORB_SOUND_FAILED);
    else if (g->model.kind == ORB_GAME_MARBLE && !old_stage_cleared &&
             g->model.game.marble.stage_cleared) feedback(g, ORB_SOUND_DONE);
    else if (g->model.score > old_score)
        feedback(g, g->model.kind == ORB_GAME_POP ? ORB_SOUND_GAME_POP : ORB_SOUND_GAME_COLLECT);
    else if (g->model.kind == ORB_GAME_ORBIT && g->model.game.orbit.ring != old_ring)
        feedback(g, ORB_SOUND_GAME_SWITCH);
    /* Continuous movement is deliberately silent: no rolling/flow/collision
     * loop can compete with the meaningful collect, hit and outcome cues. */
}

static esp_err_t enter_marble(void) { return enter_game(ORB_GAME_MARBLE); }
static esp_err_t enter_beads(void) { return enter_game(ORB_GAME_BEADS); }
static esp_err_t enter_orbit(void) { return enter_game(ORB_GAME_ORBIT); }
static esp_err_t enter_snake(void) { return enter_game(ORB_GAME_SNAKE); }
static esp_err_t enter_pop(void) { return enter_game(ORB_GAME_POP); }
static const orb_app_entry_t s_entries[ORB_GAME_COUNT] = {
    {enter_marble, leave_game, pause_game, tick_game},
    {enter_beads, leave_game, pause_game, tick_game},
    {enter_orbit, leave_game, pause_game, tick_game},
    {enter_snake, leave_game, pause_game, tick_game},
    {enter_pop, leave_game, pause_game, tick_game},
};
static const orb_app_manifest_t s_manifests[ORB_GAME_COUNT] = {
    {"marble", "Marble", ORB_ICON_CIRCLE_FILL, ORB_RGB_GREEN, ORB_APP_NEEDS_STORAGE, &s_entries[0]},
    {"beads", "Beads", ORB_ICON_DOTS_THREE, ORB_RGB_PINK, ORB_APP_NEEDS_IMU, &s_entries[1]},
    {"orbit", "Orbit", ORB_ICON_RECORD, ORB_RGB_LEGACY_INDIGO, ORB_APP_NEEDS_STORAGE, &s_entries[2]},
    {"ring-snake", "Snake", ORB_ICON_ARROWS_CLOCKWISE, ORB_RGB_LEGACY_ORANGE, ORB_APP_NEEDS_STORAGE, &s_entries[3]},
    {"pop", "Pop", ORB_ICON_SPARKLE, ORB_RGB_LEGACY_YELLOW, ORB_APP_NEEDS_STORAGE, &s_entries[4]},
};

bool orb_apps_register_builtins(void)
{
    for (unsigned i = 0; i < ORB_GAME_COUNT; ++i) {
        if (orb_apps_register(&s_manifests[i]) != ESP_OK) return false;
        refresh_progress((orb_game_kind_t)i);
    }
    return true;
}

esp_err_t orb_games_last_save_error(void) { return atomic_load(&s_save_error); }

bool orb_games_request_pause(void)
{
    if (!atomic_load(&s_game_active)) return false;
    atomic_store(&s_pause_request, true);
    return true;
}

bool orb_games_request_back(void)
{
    if (!atomic_load(&s_game_active) || !atomic_load(&s_back_available)) return false;
    orb_apps_request_exit();
    return true;
}

bool orb_games_motion_active(void) { return atomic_load(&s_motion_active); }

bool orb_games_benchmark_reserve(void)
{
    int idle = 0;
    return atomic_compare_exchange_strong(&s_owner, &idle, 2);
}

void orb_games_benchmark_release(void)
{
    int benchmark = 2;
    (void)atomic_compare_exchange_strong(&s_owner, &benchmark, 0);
}
