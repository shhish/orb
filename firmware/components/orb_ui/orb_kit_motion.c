#include "orb_ui_private.h"
#include "orb_kit_motion.h"

#include <string.h>

/* A bounded pool follows LVGL control lifetimes, outside the backdrop
 * allocator. The firmware heap prefers PSRAM. Registered owners reuse slots
 * allocation-free; deleting the last owner releases both pool and clock. */
#define MOTION_SLOTS 128
#define ARRIVAL_TIMEOUT_MS 1200

typedef struct {
    lv_obj_t *obj;
    const void *key;
    orb_kit_motion_cb_t callback;
    orb_kit_arrival_cb_t arrival;
    void *context;
    uint32_t started, previous, generation, period, deadline;
    bool staged, fade, input_feedback, waiting, bounded;
} motion_t;

static motion_t *s_slots;
static lv_timer_t *s_timer;
static uint32_t s_generation;
static uint32_t s_interrupt_generation;
static bool s_reduce_motion;
static unsigned s_owners, s_dispatching;

static bool within(lv_obj_t *obj, lv_obj_t *parent)
{
    if (!parent) return true;
    for (; obj; obj = lv_obj_get_parent(obj)) if (obj == parent) return true;
    return false;
}

static void clock_pause(void)
{
    if (!s_timer) return;
    if (!s_owners && !s_dispatching) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
        lv_free(s_slots);
        s_slots = NULL;
        return;
    }
    uint32_t remaining = UINT32_MAX;
    for (unsigned i = 0; i < MOTION_SLOTS; i++) {
        motion_t *motion = &s_slots[i];
        if (!motion->obj) continue;
        uint32_t elapsed = lv_tick_elaps(motion->previous);
        uint32_t due = elapsed >= motion->period ? 0 : motion->period - elapsed;
        if (motion->bounded) {
            int32_t budget = (int32_t)(motion->deadline - lv_tick_get());
            due = LV_MIN(due, budget <= 0 ? 0 : (uint32_t)budget);
        }
        if (motion->arrival) {
            uint32_t age = lv_tick_elaps(motion->started);
            due = LV_MIN(due, age >= ARRIVAL_TIMEOUT_MS ? 0 : ARRIVAL_TIMEOUT_MS - age);
        }
        remaining = LV_MIN(remaining, due);
    }
    if (remaining == UINT32_MAX) lv_timer_pause(s_timer);
    else {
        lv_timer_set_period(s_timer, LV_MAX(1, remaining));
        lv_timer_reset(s_timer);
        lv_timer_resume(s_timer);
        /* Covered siblings can finish their jobs immediately before every
         * handler. Preserve an overdue deadline instead of postponing it. */
        if (!remaining) lv_timer_ready(s_timer);
    }
}

static void retire(motion_t *motion, bool finish, bool animate)
{
    motion_t value = *motion;
    memset(motion, 0, sizeof(*motion));
    if (value.arrival) value.arrival(value.context, animate);
    else if (finish && value.callback) value.callback(value.context, 0, 0, true);
}

static void owner_event(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        if (s_slots) for (unsigned i = 0; i < MOTION_SLOTS; i++)
            if (s_slots[i].obj == obj) memset(&s_slots[i], 0, sizeof(s_slots[i]));
        if (s_owners) s_owners--;
        clock_pause();
    }
}

static void gate_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_DELETE) lv_free(lv_event_get_user_data(event));
}

static bool admitted(lv_obj_t *obj, bool check_hidden)
{
    if (!obj) return false;
    for (; obj; obj = lv_obj_get_parent(obj)) {
        if (check_hidden && lv_obj_is_hidden(obj)) return false;
        for (unsigned i = 0; i < lv_obj_get_event_count(obj); i++) {
            lv_event_dsc_t *event = lv_obj_get_event_dsc(obj, i);
            if (lv_event_dsc_get_cb(event) == gate_event &&
                !*(bool *)lv_event_dsc_get_user_data(event)) return false;
        }
    }
    return true;
}

bool orb_kit_motion_allowed(lv_obj_t *obj)
{
    return !orb_ui_reduce_motion() && admitted(obj, true);
}

void orb_kit_motion_set_active(lv_obj_t *parent, bool active)
{
    if (!parent) return;
    bool *gate = kit_event_state(parent, gate_event);
    bool was_active = gate && *gate;
    if (!gate) {
        gate = lv_malloc(sizeof(*gate));
        if (!gate) { if (!active) orb_kit_motion_finish(parent); return; }
        if (!lv_obj_add_event_cb(parent, gate_event, LV_EVENT_DELETE, gate)) {
            lv_free(gate);
            if (!active) orb_kit_motion_finish(parent);
            return;
        }
    }
    *gate = active;
    if (!active) orb_kit_motion_finish(parent);
    else if (!was_active) orb_kit_breathe_resume(parent);
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    uint32_t now = lv_tick_get(), generation = s_generation;
    s_dispatching++;
    for (unsigned i = 0; i < MOTION_SLOTS; i++) {
        motion_t *motion = &s_slots[i];
        if (!motion->obj || motion->generation > generation) continue;
        if (motion->bounded && (int32_t)(now - motion->deadline) >= 0) {
            retire(motion, true, false);
            continue;
        }
        uint32_t elapsed = now - motion->started;
        if (motion->arrival && elapsed >= ARRIVAL_TIMEOUT_MS) { retire(motion, true, false); continue; }
        if (now - motion->previous < motion->period) continue;
        if (motion->arrival) {
            motion->previous = now;
            if (admitted(motion->obj, true)) motion->staged = false;
            if (orb_ui_reduce_motion() || !admitted(motion->obj, !motion->staged) ||
                elapsed >= ARRIVAL_TIMEOUT_MS) retire(motion, true, false);
            continue;
        }
        if (!(motion->fade ? admitted(motion->obj, true) : orb_kit_motion_allowed(motion->obj))) {
            retire(motion, true, false); continue;
        }
        uint32_t serial = motion->generation, dt = now - motion->previous;
        motion->previous = now;
        bool moving = motion->callback(motion->context, elapsed, dt, false);
        /* A callback may replace its own job or delete its object. */
        if (!moving && motion->obj && motion->generation == serial) memset(motion, 0, sizeof(*motion));
    }
    s_dispatching--;
    clock_pause();
}

static motion_t *slot(lv_obj_t *obj, const void *key)
{
    if (!obj) return NULL;
    if (!s_slots) {
        s_slots = lv_malloc_zeroed(MOTION_SLOTS * sizeof(*s_slots));
        if (!s_slots) return NULL;
    }
    if (!s_timer) {
        s_timer = lv_timer_create(tick, 16, NULL);
        if (!s_timer) { lv_free(s_slots); s_slots = NULL; return NULL; }
        lv_timer_pause(s_timer);
    }
    bool registered = false;
    for (unsigned i = 0; i < lv_obj_get_event_count(obj); i++)
        registered |= lv_event_dsc_get_cb(lv_obj_get_event_dsc(obj, i)) == owner_event;
    if (!registered) {
        if (!lv_obj_add_event_cb(obj, owner_event, LV_EVENT_DELETE, NULL)) { clock_pause(); return NULL; }
        s_owners++;
    }
    motion_t *empty = NULL;
    for (unsigned i = 0; i < MOTION_SLOTS; i++) {
        if (s_slots[i].obj == obj && s_slots[i].key == key) return &s_slots[i];
        if (!s_slots[i].obj && !empty) empty = &s_slots[i];
    }
    return empty;
}

static void start(motion_t *motion, lv_obj_t *obj, const void *key, void *context)
{
    *motion = (motion_t){.obj = obj, .key = key, .context = context,
        .started = lv_tick_get(), .previous = lv_tick_get(), .generation = ++s_generation, .period = 16};
    if (lv_timer_get_paused(s_timer)) lv_timer_reset(s_timer);
    clock_pause();
}

bool orb_kit_motion_start(lv_obj_t *obj, const void *key, orb_kit_motion_cb_t callback, void *context)
{
    if (!callback || !obj) return false;
    if (!orb_kit_motion_allowed(obj)) {
        orb_kit_motion_cancel(obj, key, false);
        callback(context, 0, 0, true);
        return false;
    }
    motion_t *motion = slot(obj, key);
    if (!motion) { callback(context, 0, 0, true); return false; }
    start(motion, obj, key, context);
    motion->callback = callback;
    return true;
}

void orb_kit_motion_cancel(lv_obj_t *obj, const void *key, bool finish)
{
    s_dispatching++;
    if (s_slots) for (unsigned i = 0; i < MOTION_SLOTS; i++) {
        motion_t *motion = &s_slots[i];
        if (motion->obj != obj || motion->key != key) continue;
        if (finish) retire(motion, true, false);
        else memset(motion, 0, sizeof(*motion));
    }
    s_dispatching--;
    clock_pause();
}

void orb_kit_motion_finish(lv_obj_t *parent)
{
    s_dispatching++;
    if (s_slots) for (unsigned i = 0; i < MOTION_SLOTS; i++)
        if (s_slots[i].obj && within(s_slots[i].obj, parent)) retire(&s_slots[i], true, false);
    s_dispatching--;
    clock_pause();
}

bool orb_kit_arrival_prepare(lv_obj_t *obj, orb_kit_arrival_cb_t callback, void *context)
{
    if (!callback || !obj) return false;
    if (orb_ui_reduce_motion() || !admitted(obj, false)) { callback(context, false); return false; }
    /* Distinct properties of one glyph (Pop and Draw on) share the fence
     * without replacing each other's prepared presentation. */
    motion_t *motion = slot(obj, context);
    if (!motion) { callback(context, false); return false; }
    start(motion, obj, context, context);
    motion->arrival = callback;
    motion->staged = !admitted(obj, true);
    motion->period = 100;
    clock_pause();
    return true;
}

void orb_kit_arrival_complete(lv_obj_t *parent, bool fresh)
{
    s_dispatching++;
    if (s_slots) for (unsigned i = 0; i < MOTION_SLOTS; i++) {
        motion_t *motion = &s_slots[i];
        if (motion->obj && motion->arrival && within(motion->obj, parent))
            retire(motion, true, fresh && orb_kit_motion_allowed(motion->obj));
    }
    s_dispatching--;
    clock_pause();
    /* Visibility may follow framework activation when a staged page moves
     * out of its hidden snapshot parent. The fence resumes only named waits
     * and keeps their original absolute deadline on fresh and restored UI. */
    orb_kit_breathe_resume(parent);
}

unsigned orb_kit_motion_count(void)
{
    unsigned count = 0;
    if (s_slots) for (unsigned i = 0; i < MOTION_SLOTS; i++) count += s_slots[i].obj != NULL;
    return count;
}

bool orb_kit_motion_timer_running(void)
{
    return s_timer && !lv_timer_get_paused(s_timer);
}

bool orb_kit_motion_settling(lv_obj_t *parent)
{
    if (s_slots) for (unsigned i = 0; i < MOTION_SLOTS; i++)
        if (s_slots[i].obj && !s_slots[i].waiting && within(s_slots[i].obj, parent)) return true;
    return false;
}

void orb_ui_set_reduce_motion(bool on)
{
    bool was_reduced = s_reduce_motion;
    s_reduce_motion = on;
    if (on) orb_kit_motion_finish(NULL);
    else if (was_reduced && lv_display_get_default()) orb_kit_breathe_resume(lv_screen_active());
}

bool orb_ui_reduce_motion(void)
{
    return s_reduce_motion;
}

bool orb_kit_motion_start_interval(lv_obj_t *obj, const void *key,
    orb_kit_motion_cb_t callback, void *context, uint32_t period_ms)
{
    if (!orb_kit_motion_start(obj, key, callback, context)) return false;
    motion_t *motion = slot(obj, key);
    motion->period = LV_MAX(16, period_ms);
    clock_pause();
    return true;
}

bool orb_kit_motion_prepare(lv_obj_t *obj)
{
    return slot(obj, obj) != NULL;
}

bool orb_kit_motion_start_wait(lv_obj_t *obj, const void *key,
    orb_kit_motion_cb_t callback, void *context)
{
    if (!orb_kit_motion_start_interval(obj, key, callback, context, 100)) return false;
    slot(obj, key)->waiting = true;
    return true;
}

bool orb_kit_motion_start_wait_until(lv_obj_t *obj, const void *key,
    orb_kit_motion_cb_t callback, void *context, uint32_t deadline_ms)
{
    if (!orb_kit_motion_start_wait(obj, key, callback, context)) return false;
    motion_t *motion = slot(obj, key);
    motion->deadline = deadline_ms;
    motion->bounded = true;
    clock_pause();
    return true;
}

bool orb_kit_motion_start_fade(lv_obj_t *obj, const void *key,
    orb_kit_motion_cb_t callback, void *context)
{
    if (!callback || !obj) return false;
    if (!admitted(obj, true)) {
        orb_kit_motion_cancel(obj, key, false);
        callback(context, 0, 0, true);
        return false;
    }
    motion_t *motion = slot(obj, key);
    if (!motion) { callback(context, 0, 0, true); return false; }
    start(motion, obj, key, context);
    motion->callback = callback;
    motion->fade = true;
    return true;
}

bool orb_kit_motion_start_feedback(lv_obj_t *obj, const void *key,
    orb_kit_motion_cb_t callback, void *context)
{
    if (!orb_kit_motion_start(obj, key, callback, context)) return false;
    slot(obj, key)->input_feedback = true;
    return true;
}

void orb_kit_motion_interrupt(lv_obj_t *parent)
{
    s_interrupt_generation++;
    s_dispatching++;
    if (s_slots) for (unsigned i = 0; i < MOTION_SLOTS; i++) {
        motion_t *motion = &s_slots[i];
        /* A named wait is state, not presentation: it keeps breathing
         * through a touch until the wait ends or its 30 s budget runs out. */
        if (motion->obj && !motion->input_feedback && !motion->waiting &&
            within(motion->obj, parent))
            retire(motion, true, false);
    }
    s_dispatching--;
    clock_pause();
}

uint32_t orb_kit_motion_interrupt_generation(void)
{
    return s_interrupt_generation;
}
