#include "orb_nav.h"

#include <math.h>
#include <string.h>

#define ORB_NAV_PI 3.14159265358979323846f
#define ORB_NAV_ARBITRATION 12.0f
#define ORB_NAV_HOLD_SLOP 10.0f
#define ORB_NAV_AXIS_HYSTERESIS 1.25f
#define ORB_NAV_VELOCITY_MS 100u
#define ORB_NAV_LONG_PRESS_MS 400u
#define ORB_NAV_FLING_MAX_TURNS 2.0f

static float clampf(float value, float minimum, float maximum)
{
    return fmaxf(minimum, fminf(maximum, value));
}

static const orb_nav_place_t *place_for(const orb_nav_t *nav, orb_nav_id_t id)
{
    for (size_t i = 0; i < nav->place_count; i++) {
        if (nav->places[i].id == id) return &nav->places[i];
    }
    return NULL;
}

static orb_nav_id_t role_id(const orb_nav_t *nav, orb_nav_role_t role)
{
    for (size_t i = 0; i < nav->place_count; i++) {
        if (nav->places[i].role == role) return nav->places[i].id;
    }
    return ORB_NAV_INVALID_ID;
}

static bool is_layer(const orb_nav_t *nav, orb_nav_id_t id)
{
    const orb_nav_place_t *place = place_for(nav, id);
    return place && (place->role == ORB_NAV_ROLE_STACK ||
                     place->role == ORB_NAV_ROLE_NOTIFICATIONS ||
                     place->role == ORB_NAV_ROLE_CONTROL);
}

static void restore_layers(orb_nav_t *nav)
{
    nav->layer_count = 0;
    if (!is_layer(nav, nav->current)) return;
    /* Layers overlay the nearest plain place. Moving to another plain place
     * dismisses that stack; Back may later restore it from bounded history.
     * Counting every layer ever pushed leaked slots across repeated opens. */
    size_t first = nav->back_count;
    while (first && is_layer(nav, nav->back[first - 1].id)) first--;
    if (nav->back_count - first >= ORB_NAV_LAYER_CAPACITY)
        first = nav->back_count - (ORB_NAV_LAYER_CAPACITY - 1);
    for (size_t i = first; i < nav->back_count; i++)
        nav->layers[nav->layer_count++] = nav->back[i].id;
    nav->layers[nav->layer_count++] = nav->current;
}

void orb_nav_init(orb_nav_t *nav, orb_nav_id_t face)
{
    if (!nav) return;
    memset(nav, 0, sizeof(*nav));
    nav->current = nav->face = face;
}

bool orb_nav_register(orb_nav_t *nav, orb_nav_place_t place)
{
    if (!nav || place.id == ORB_NAV_INVALID_ID) return false;
    for (size_t i = 0; i < nav->place_count; i++) {
        if (nav->places[i].id == place.id) {
            nav->places[i] = place;
            return true;
        }
    }
    if (nav->place_count == ORB_NAV_MAX_PLACES) return false;
    nav->places[nav->place_count++] = place;
    return true;
}

void orb_nav_set_current(orb_nav_t *nav, orb_nav_id_t id)
{
    if (!nav) return;
    if (nav->current != id) orb_nav_rotary_stop(nav);
    nav->current = id;
    restore_layers(nav);
}

orb_nav_id_t orb_nav_neighbor(const orb_nav_t *nav, orb_nav_gesture_t gesture)
{
    if (!nav) return ORB_NAV_INVALID_ID;
    const orb_nav_place_t *place = place_for(nav, nav->current);
    const orb_nav_role_t role = place ? place->role : ORB_NAV_ROLE_SCREEN;
    switch (gesture) {
    case ORB_NAV_GESTURE_LEFT:
        if (role == ORB_NAV_ROLE_FACE) return role_id(nav, ORB_NAV_ROLE_HOME);
        break;
    case ORB_NAV_GESTURE_RIGHT:
        if (role == ORB_NAV_ROLE_FACE) return role_id(nav, ORB_NAV_ROLE_AGENTS);
        break;
    case ORB_NAV_GESTURE_UP:
        if (role == ORB_NAV_ROLE_NOTIFICATIONS) {
            return nav->back_count ? nav->back[nav->back_count - 1].id : nav->face;
        }
        if (role != ORB_NAV_ROLE_STACK) return role_id(nav, ORB_NAV_ROLE_STACK);
        break;
    case ORB_NAV_GESTURE_DOWN:
        if (role == ORB_NAV_ROLE_STACK) {
            return nav->back_count ? nav->back[nav->back_count - 1].id : nav->face;
        }
        if (role != ORB_NAV_ROLE_NOTIFICATIONS) return role_id(nav, ORB_NAV_ROLE_NOTIFICATIONS);
        break;
    case ORB_NAV_GESTURE_CONTROL:
        if (role != ORB_NAV_ROLE_CONTROL) return role_id(nav, ORB_NAV_ROLE_CONTROL);
        break;
    case ORB_NAV_GESTURE_BACK:
        if (nav->current != nav->face) {
            return nav->back_count ? nav->back[nav->back_count - 1].id : nav->face;
        }
        break;
    default:
        break;
    }
    return ORB_NAV_INVALID_ID;
}

bool orb_nav_push(orb_nav_t *nav, orb_nav_id_t id, const orb_nav_token_t *token)
{
    if (!nav || id == ORB_NAV_INVALID_ID) return false;
    if (nav->current == id) return true;
    if (is_layer(nav, id) && is_layer(nav, nav->current)) {
        /* A sheet has one identity in the active stack. Revealing a sheet
         * already beneath this one unwinds to it; repeated edge/button
         * routes must not exhaust slots or bury the original plain place. */
        for (size_t i = nav->back_count; i && is_layer(nav, nav->back[i - 1].id); i--) {
            if (nav->back[i - 1].id != id) continue;
            nav->back_count = i - 1;
            orb_nav_set_current(nav, id);
            return true;
        }
    }
    if (is_layer(nav, id) && nav->layer_count == ORB_NAV_LAYER_CAPACITY) return false;
    if (nav->back_count == ORB_NAV_BACK_CAPACITY) {
        /* Keep recent context without growing a heap allocation under input. */
        memmove(nav->back, nav->back + 1,
                (ORB_NAV_BACK_CAPACITY - 1) * sizeof(nav->back[0]));
        nav->back_count--;
    }
    orb_nav_back_entry_t *entry = &nav->back[nav->back_count++];
    *entry = (orb_nav_back_entry_t){.id = nav->current};
    if (token) {
        entry->token = *token;
        entry->iris = token->width > 0 && token->height > 0;
    }
    orb_nav_set_current(nav, id);
    return true;
}

bool orb_nav_back_peek(const orb_nav_t *nav, orb_nav_back_entry_t *entry)
{
    if (!nav || !nav->back_count) return false;
    if (entry) *entry = nav->back[nav->back_count - 1];
    return true;
}

bool orb_nav_back(orb_nav_t *nav, orb_nav_back_entry_t *entry)
{
    if (!orb_nav_back_peek(nav, entry)) return false;
    orb_nav_id_t previous = nav->back[--nav->back_count].id;
    orb_nav_set_current(nav, previous);
    return true;
}

void orb_nav_reset(orb_nav_t *nav, orb_nav_id_t id)
{
    if (!nav) return;
    nav->back_count = nav->layer_count = 0;
    memset(&nav->touch, 0, sizeof(nav->touch));
    orb_nav_set_current(nav, id);
    orb_nav_rotary_stop(nav);
}

float orb_nav_rubber_band(float distance, float extent)
{
    if (extent <= 0.0f) return 0.0f;
    float magnitude = fabsf(distance);
    return copysignf(extent * magnitude * 0.55f / (extent + magnitude * 0.55f), distance);
}

static orb_nav_compass_selection_t compass_selection(float x, float y)
{
    x -= ORB_NAV_DIAMETER * 0.5f;
    y -= ORB_NAV_DIAMETER * 0.5f;
    if (x * x + y * y < 52.0f * 52.0f) return ORB_NAV_COMPASS_CANCEL;
    if (fabsf(y) >= fabsf(x)) return y < 0 ? ORB_NAV_COMPASS_SETTINGS : ORB_NAV_COMPASS_PLAY;
    return x > 0 ? ORB_NAV_COMPASS_AGENTS : ORB_NAV_COMPASS_HOME;
}

static orb_nav_gesture_t edge_gesture(const orb_nav_t *nav)
{
    const orb_nav_recognizer_t *touch = &nav->touch;
    /* DOWN fixes the nearest angular edge for the entire contact. A wide
     * rectangular edge test misclassifies the round panel's diagonal bands. */
    if (fabsf(touch->radial_y) >= fabsf(touch->radial_x))
        return touch->radial_y < 0 ? ORB_NAV_GESTURE_DOWN : ORB_NAV_GESTURE_UP;
    if (touch->radial_x > 0) return ORB_NAV_GESTURE_CONTROL;
    /* A Face-local gallery/editor still needs Back despite sharing the
     * Face place ID. The adapter decides whether any parent exists. */
    return ORB_NAV_GESTURE_BACK;
}

static orb_nav_gesture_t choose_owner(const orb_nav_t *nav, float dx, float dy)
{
    const orb_nav_recognizer_t *touch = &nav->touch;
    const orb_nav_place_t *place = place_for(nav, nav->current);
    if (touch->rim) {
        float radial = dx * touch->radial_x + dy * touch->radial_y;
        float tangent = -dx * touch->radial_y + dy * touch->radial_x;
        if (fmaxf(fabsf(radial), fabsf(tangent)) <
            fminf(fabsf(radial), fabsf(tangent)) * ORB_NAV_AXIS_HYSTERESIS &&
            hypotf(dx, dy) < 2 * ORB_NAV_ARBITRATION) return ORB_NAV_GESTURE_NONE;
        /* Wait through initial finger jitter and ambiguous diagonals. Once
         * locked, crossing an axis cannot hand crown input to navigation. */
        if (fabsf(tangent) > fabsf(radial)) {
            return nav->rotary.bound && nav->rotary.owner.owner == nav->current ?
                   ORB_NAV_GESTURE_ROTARY : ORB_NAV_GESTURE_CONTENT;
        }
        if (radial < 0.0f) return edge_gesture(nav);
        return ORB_NAV_GESTURE_CONTENT;
    }
    /* Centre contacts always belong to content. Face has no scrollable
     * content, and is the only place that retains centre navigation. */
    if (!place || place->role != ORB_NAV_ROLE_FACE) return ORB_NAV_GESTURE_CONTENT;
    if (fmaxf(fabsf(dx), fabsf(dy)) <
        fminf(fabsf(dx), fabsf(dy)) * ORB_NAV_AXIS_HYSTERESIS &&
        hypotf(dx, dy) < 2 * ORB_NAV_ARBITRATION) return ORB_NAV_GESTURE_NONE;
    if (fabsf(dx) > fabsf(dy)) {
        return dx < 0 ? ORB_NAV_GESTURE_LEFT : ORB_NAV_GESTURE_RIGHT;
    }
    return dy < 0 ? ORB_NAV_GESTURE_UP : ORB_NAV_GESTURE_DOWN;
}

static void touch_sample(orb_nav_recognizer_t *touch, float x, float y, uint32_t now_ms)
{
    /* The navigation timer observes LVGL's last input read, including repeats
     * between controller updates. A time window includes those held samples:
     * a final quantized pixel or release delay cannot erase the entire fling. */
    if (touch->sample_count &&
        now_ms == touch->samples[touch->sample_count - 1].ms) {
        touch->sample_count--;
    }
    while (touch->sample_count > 1 &&
           now_ms - touch->samples[1].ms >= ORB_NAV_VELOCITY_MS) {
        memmove(touch->samples, touch->samples + 1,
                --touch->sample_count * sizeof(touch->samples[0]));
    }
    if (touch->sample_count == ORB_NAV_VELOCITY_SAMPLES) {
        memmove(touch->samples, touch->samples + 1,
                --touch->sample_count * sizeof(touch->samples[0]));
    }
    touch->samples[touch->sample_count++] = (orb_nav_touch_sample_t){x, y, now_ms};
    const orb_nav_touch_sample_t *first = touch->samples;
    uint32_t elapsed = now_ms - first->ms;
    touch->vx = touch->vy = 0;
    if (!elapsed || touch->sample_count < 2) return;
    float first_x = first->x, first_y = first->y;
    if (elapsed > ORB_NAV_VELOCITY_MS) {
        const orb_nav_touch_sample_t *next = first + 1;
        uint32_t span = next->ms - first->ms;
        float amount = span ? (float)(elapsed - ORB_NAV_VELOCITY_MS) / span : 0;
        first_x += (next->x - first_x) * amount;
        first_y += (next->y - first_y) * amount;
        elapsed = ORB_NAV_VELOCITY_MS;
    }
    touch->vx = (x - first_x) * 1000.0f / elapsed;
    touch->vy = (y - first_y) * 1000.0f / elapsed;
}

static float reverse_speed(const orb_nav_recognizer_t *touch, bool horizontal,
                           float direction, uint32_t now_ms)
{
    float current = horizontal ? touch->last_x : touch->last_y;
    float speed = 0;
    for (unsigned i = 0; i < touch->sample_count; i++) {
        const orb_nav_touch_sample_t *sample = &touch->samples[i];
        uint32_t elapsed = now_ms - sample->ms;
        if (!elapsed || elapsed > ORB_NAV_VELOCITY_MS) continue;
        float previous = horizontal ? sample->x : sample->y;
        float reversed = (previous - current) * direction;
        /* A deliberate reverse stroke cancels even a long drag. Ignore the
         * few pixels of lateral noise a finger makes as it leaves the glass. */
        if (reversed >= ORB_NAV_ARBITRATION)
            speed = fmaxf(speed, reversed * 1000.0f / elapsed);
    }
    return speed >= 180 ? speed : 0;
}

static bool rotary_apply(orb_nav_t *nav, float delta)
{
    orb_nav_rotary_t *rotary = &nav->rotary;
    if (!rotary->bound || rotary->owner.owner != nav->current) return false;
    float old = rotary->owner.value;
    float value = clampf(old + delta, rotary->owner.minimum, rotary->owner.maximum);
    if (value == old) return false;
    rotary->owner.value = value;
    if (rotary->owner.change) rotary->owner.change(rotary->owner.ctx, value, value - old);
    if (rotary->owner.detent > 0.0f) {
        float position = floorf((value - rotary->owner.minimum) / rotary->owner.detent);
        if (position != rotary->detent_position) {
            rotary->detent_position = position;
            if (rotary->owner.tick) rotary->owner.tick(rotary->owner.ctx);
        }
    }
    return true;
}

bool orb_nav_rotary_step(orb_nav_t *nav, int detents)
{
    if (!nav || !detents || nav->touch.down || !nav->rotary.bound ||
        nav->rotary.owner.owner != nav->current || nav->rotary.owner.detent <= 0) return false;
    /* Bound a delayed producer, and do not turn desk input into a fling. */
    detents = detents > 8 ? 8 : detents < -8 ? -8 : detents;
    orb_nav_rotary_stop(nav);
    bool changed = false;
    for (int i = 0; i < (detents < 0 ? -detents : detents); i++)
        changed = rotary_apply(nav, copysignf(nav->rotary.owner.detent, (float)detents)) || changed;
    return changed;
}

static void rotary_move(orb_nav_t *nav, float x, float y, uint32_t now_ms)
{
    orb_nav_rotary_t *rotary = &nav->rotary;
    float angle = atan2f(y - ORB_NAV_DIAMETER * 0.5f, x - ORB_NAV_DIAMETER * 0.5f);
    float radians = angle - rotary->last_angle;
    while (radians > ORB_NAV_PI) radians -= 2.0f * ORB_NAV_PI;
    while (radians < -ORB_NAV_PI) radians += 2.0f * ORB_NAV_PI;
    uint32_t elapsed = now_ms - rotary->last_ms;
    float delta = radians * rotary->owner.units_per_turn / (2.0f * ORB_NAV_PI);
    rotary_apply(nav, delta);
    if (elapsed && elapsed < 160u) {
        float limit = rotary->owner.units_per_turn * ORB_NAV_FLING_MAX_TURNS;
        rotary->velocity = clampf(delta * 1000.0f / elapsed, -limit, limit);
    } else rotary->velocity = 0.0f;
    rotary->last_angle = angle;
    rotary->last_ms = now_ms;
}

bool orb_nav_touch_resume(orb_nav_t *nav, orb_nav_gesture_t gesture,
                          float progress, bool commit)
{
    if (!nav || !nav->touch.down || !isfinite(progress) ||
        gesture < ORB_NAV_GESTURE_LEFT || gesture > ORB_NAV_GESTURE_CONTROL) return false;
    const orb_nav_place_t *place = place_for(nav, nav->current);
    if (nav->touch.rim) {
        if (gesture != edge_gesture(nav)) return false;
    } else if (!place || place->role != ORB_NAV_ROLE_FACE ||
               gesture > ORB_NAV_GESTURE_DOWN) return false;
    nav->touch.owner = gesture;
    nav->touch.progress_origin = progress;
    nav->touch.resumed = true;
    nav->touch.resume_commit = commit;
    nav->touch.moved = false;
    return true;
}

static void touch_begin(orb_nav_t *nav, float x, float y, uint32_t now_ms, bool blank_start)
{
    orb_nav_recognizer_t *touch = &nav->touch;
    orb_nav_rotary_stop(nav);
    float rx = x - ORB_NAV_DIAMETER * 0.5f;
    float ry = y - ORB_NAV_DIAMETER * 0.5f;
    float radius = hypotf(rx, ry);
    *touch = (orb_nav_recognizer_t){
        .down = true, .rim = radius >= ORB_NAV_DIAMETER * 0.5f - ORB_NAV_EDGE_BAND,
        .blank = blank_start,
        .start_x = x, .start_y = y, .last_x = x, .last_y = y,
        .start_ms = now_ms, .last_ms = now_ms,
        .radial_x = radius > 0 ? rx / radius : 0,
        .radial_y = radius > 0 ? ry / radius : 0,
    };
    nav->rotary.last_angle = atan2f(ry, rx);
    nav->rotary.last_ms = now_ms;
    touch_sample(touch, x, y, now_ms);
}

orb_nav_event_t orb_nav_touch(orb_nav_t *nav, orb_nav_touch_phase_t phase,
                             float x, float y, uint32_t now_ms,
                             bool blank_start, bool scroll_at_top)
{
    (void)scroll_at_top;
    orb_nav_event_t event = {0};
    if (!nav) return event;
    orb_nav_recognizer_t *touch = &nav->touch;
    if (phase == ORB_NAV_TOUCH_DOWN) {
        touch_begin(nav, x, y, now_ms, blank_start);
        return event;
    }
    if (!touch->down) {
        if (phase == ORB_NAV_TOUCH_CANCEL) orb_nav_rotary_stop(nav);
        return event;
    }
    event.gesture = touch->owner;
    event.dx = x - touch->start_x;
    event.dy = y - touch->start_y;
    /* HIG §7: movement cancels a deliberate hold at the same threshold as
     * taps, even before axis arbitration or after returning to DOWN. */
    if (hypotf(event.dx, event.dy) >= ORB_NAV_HOLD_SLOP) touch->blank = false;
    if (phase == ORB_NAV_TOUCH_CANCEL) {
        event.type = ORB_NAV_EVENT_CANCEL;
        touch->down = false;
        touch->owner = ORB_NAV_GESTURE_NONE;
        orb_nav_rotary_stop(nav);
        return event;
    }
    if (phase == ORB_NAV_TOUCH_TICK) {
        if (touch->owner == ORB_NAV_GESTURE_NONE && touch->blank && !touch->rim &&
            now_ms - touch->start_ms >= ORB_NAV_LONG_PRESS_MS) {
            touch->owner = event.gesture = ORB_NAV_GESTURE_COMPASS;
            event.type = ORB_NAV_EVENT_COMPASS;
        }
        return event;
    }
    uint32_t elapsed = now_ms - touch->last_ms;
    bool same_position = x == touch->last_x && y == touch->last_y;
    if (!same_position) {
        touch->moved = true;
        touch->last_x = x;
        touch->last_y = y;
        touch->last_ms = now_ms;
    }
    touch_sample(touch, x, y, now_ms);
    event.vx = touch->vx;
    event.vy = touch->vy;
    if (touch->owner == ORB_NAV_GESTURE_NONE && phase == ORB_NAV_TOUCH_MOVE &&
        hypotf(event.dx, event.dy) >= ORB_NAV_ARBITRATION) {
        touch->owner = choose_owner(nav, event.dx, event.dy);
        if (touch->owner != ORB_NAV_GESTURE_NONE) event.type = ORB_NAV_EVENT_BEGIN;
    } else if (touch->owner != ORB_NAV_GESTURE_NONE) event.type = ORB_NAV_EVENT_UPDATE;
    event.gesture = touch->owner;
    if (touch->owner == ORB_NAV_GESTURE_ROTARY) {
        if (!same_position) rotary_move(nav, x, y, now_ms);
        else if (elapsed >= 80u) nav->rotary.velocity = 0;
        if (phase == ORB_NAV_TOUCH_UP) nav->rotary.last_ms = now_ms;
    }
    if (touch->owner == ORB_NAV_GESTURE_COMPASS) {
        event.selection = hypotf(event.dx, event.dy) >= ORB_NAV_ARBITRATION ?
                          compass_selection(x, y) : ORB_NAV_COMPASS_CANCEL;
        event.commit = event.selection != ORB_NAV_COMPASS_CANCEL;
    } else {
        float distance = 0, velocity = 0;
        switch (touch->owner) {
        case ORB_NAV_GESTURE_LEFT: case ORB_NAV_GESTURE_CONTROL:
            distance = -event.dx; velocity = -event.vx; break;
        case ORB_NAV_GESTURE_RIGHT: case ORB_NAV_GESTURE_BACK:
            distance = event.dx; velocity = event.vx; break;
        case ORB_NAV_GESTURE_UP:
            distance = -event.dy; velocity = -event.vy; break;
        case ORB_NAV_GESTURE_DOWN:
            distance = event.dy; velocity = event.vy; break;
        default: break;
        }
        distance += touch->progress_origin * ORB_NAV_DIAMETER;
        event.progress = distance / ORB_NAV_DIAMETER;
        bool horizontal = touch->owner == ORB_NAV_GESTURE_LEFT ||
            touch->owner == ORB_NAV_GESTURE_RIGHT || touch->owner == ORB_NAV_GESTURE_BACK ||
            touch->owner == ORB_NAV_GESTURE_CONTROL;
        float direction = touch->owner == ORB_NAV_GESTURE_LEFT ||
            touch->owner == ORB_NAV_GESTURE_CONTROL || touch->owner == ORB_NAV_GESTURE_UP ? -1 : 1;
        float reversed = reverse_speed(touch, horizontal, direction, now_ms);
        if (reversed) {
            /* Do not seed the cancellation spring with the earlier outward
             * part of the window after a deliberate change of direction. */
            if (horizontal) event.vx = -direction * reversed;
            else event.vy = -direction * reversed;
        }
        /* HIG §6: hand the windowed lift speed to the spring. A quarter-screen
         * drag remains intentional after deceleration or a pause before lift;
         * a recent reverse flick explicitly cancels, rather than fighting it. */
        event.commit = touch->resumed && !touch->moved ? touch->resume_commit :
                       distance >= 60 && !reversed &&
                       distance + clampf(velocity, 0, 2400) * 0.16f >=
                       ORB_NAV_DIAMETER * 0.25f;
    }
    if (phase == ORB_NAV_TOUCH_UP) {
        if (touch->owner != ORB_NAV_GESTURE_NONE) event.type = ORB_NAV_EVENT_END;
        touch->down = false;
        if (touch->owner == ORB_NAV_GESTURE_ROTARY) {
            nav->rotary.flinging = fabsf(nav->rotary.velocity) > 0.5f;
        }
        touch->owner = ORB_NAV_GESTURE_NONE;
    }
    return event;
}

bool orb_nav_touch_owned(const orb_nav_t *nav)
{
    return nav && nav->touch.down && nav->touch.owner != ORB_NAV_GESTURE_NONE &&
           nav->touch.owner != ORB_NAV_GESTURE_CONTENT;
}

void orb_nav_rotary_bind(orb_nav_t *nav, const orb_nav_rotary_owner_t *owner)
{
    if (!nav) return;
    memset(&nav->rotary, 0, sizeof(nav->rotary));
    if (!owner || owner->maximum < owner->minimum || owner->units_per_turn <= 0) return;
    nav->rotary.owner = *owner;
    nav->rotary.bound = true;
    orb_nav_rotary_set(nav, owner->value);
}

void orb_nav_rotary_unbind(orb_nav_t *nav, orb_nav_id_t owner)
{
    if (nav && nav->rotary.bound && nav->rotary.owner.owner == owner) {
        memset(&nav->rotary, 0, sizeof(nav->rotary));
    }
}

void orb_nav_rotary_set(orb_nav_t *nav, float value)
{
    if (!nav || !nav->rotary.bound) return;
    orb_nav_rotary_t *rotary = &nav->rotary;
    rotary->owner.value = clampf(value, rotary->owner.minimum, rotary->owner.maximum);
    rotary->detent_position = rotary->owner.detent > 0 ?
                             floorf((rotary->owner.value - rotary->owner.minimum) /
                                    rotary->owner.detent) : 0;
}

void orb_nav_rotary_stop(orb_nav_t *nav)
{
    if (!nav) return;
    nav->rotary.flinging = false;
    nav->rotary.velocity = 0;
}

bool orb_nav_tick(orb_nav_t *nav, uint32_t now_ms)
{
    if (!nav || !nav->rotary.bound || !nav->rotary.flinging || nav->touch.down) return false;
    orb_nav_rotary_t *rotary = &nav->rotary;
    uint32_t elapsed = now_ms - rotary->last_ms;
    rotary->last_ms = now_ms;
    if (!elapsed) return false;
    /* A stalled UI resumes at rest, rather than jumping across a long list. */
    if (elapsed > 100u || rotary->owner.owner != nav->current) {
        orb_nav_rotary_stop(nav);
        return false;
    }
    float seconds = elapsed / 1000.0f;
    bool changed = rotary_apply(nav, rotary->velocity * seconds);
    rotary->velocity *= expf(-5.5f * seconds);
    if (!changed || fabsf(rotary->velocity) < fmaxf(0.5f, rotary->owner.detent * 0.08f)) {
        orb_nav_rotary_stop(nav);
    }
    return changed;
}
