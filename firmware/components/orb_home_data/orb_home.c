#include "orb_home.h"
#include "orb_memory.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#else
#include <pthread.h>
#endif

typedef struct {
    bool active, claimed, room_off;
    orb_home_request_t request;
    orb_home_state_t before;
    uint32_t authoritative; /* Characteristics changed by events after enqueue. */
} pending_t;
typedef struct {
    orb_home_snapshot_t view, stage;
    pending_t pending[ORB_HOME_MAX_PENDING];
    bool staging;
    uint32_t sequence;
    uint64_t room_off_remaining;
} home_store_t;
static home_store_t *s_store;
static void drain_room_off(void);
#ifdef ESP_PLATFORM
static StaticSemaphore_t s_mutex_buffer;
static SemaphoreHandle_t s_mutex;
static void lock(void) { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mutex); }
#else
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static void lock(void) { pthread_mutex_lock(&s_mutex); }
static void unlock(void) { pthread_mutex_unlock(&s_mutex); }
#endif

/* Byte bounds also validate UTF-8: rejecting a split sequence is safer than
 * silently assigning a different accessory's shortened alias. */
static bool text_ok(const char *text, size_t capacity, bool required)
{
    if (!text) return false;
    const unsigned char *p = (const unsigned char *)text;
    size_t n = strnlen(text, capacity);
    if (n == capacity || (required && !n)) return false;
    for (size_t i = 0; i < n;) {
        unsigned c = p[i++], need; uint32_t v, min;
        if (c < 0x80) continue;
        if (c >= 0xc2 && c <= 0xdf) { need = 1; v = c & 31; min = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { need = 2; v = c & 15; min = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { need = 3; v = c & 7; min = 0x10000; }
        else return false;
        if (n - i < need) return false;
        while (need--) { c = p[i++]; if ((c & 0xc0) != 0x80) return false; v = (v << 6) | (c & 63); }
        if (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) return false;
    }
    return true;
}
static bool alias_ok(const char *id)
{
    if (!text_ok(id, ORB_HOME_ID_MAX + 1, true)) return false;
    for (const unsigned char *p = (const unsigned char *)id; *p; p++) if (*p < 0x20 || *p >= 0x7f) return false;
    return true;
}
static bool state_ok(const orb_home_state_t *s)
{
    if (!s || s->present & ~0xffffu) return false;
#define RANGE(bit, field, lo, hi) \
    if ((s->present & (bit)) && (!isfinite(s->field) || s->field < (lo) || s->field > (hi))) return false
    RANGE(ORB_HOME_BRIGHTNESS, brightness, 0, 100);
    RANGE(ORB_HOME_HUE, hue, 0, 360); RANGE(ORB_HOME_SAT, sat, 0, 100);
    RANGE(ORB_HOME_CT, ct, 50, 1000); RANGE(ORB_HOME_TEMP, temp, -100, 100);
    RANGE(ORB_HOME_TARGET, target, -100, 100); RANGE(ORB_HOME_HUMIDITY_VALUE, humidity, 0, 100);
    RANGE(ORB_HOME_SPEED, speed, 0, 100); RANGE(ORB_HOME_POSITION, position, 0, 100);
    RANGE(ORB_HOME_BATTERY, battery, 0, 100);
#undef RANGE
    return !(s->present & ORB_HOME_MODE) || (s->mode >= ORB_HOME_MODE_OFF && s->mode <= ORB_HOME_MODE_AUTO);
}
static void merge(orb_home_state_t *to, const orb_home_state_t *from)
{
#define FIELD(bit, field) if (from->present & (bit)) to->field = from->field
    FIELD(ORB_HOME_ON, on); FIELD(ORB_HOME_BRIGHTNESS, brightness); FIELD(ORB_HOME_HUE, hue);
    FIELD(ORB_HOME_SAT, sat); FIELD(ORB_HOME_CT, ct); FIELD(ORB_HOME_TEMP, temp);
    FIELD(ORB_HOME_TARGET, target); FIELD(ORB_HOME_MODE, mode); FIELD(ORB_HOME_HUMIDITY_VALUE, humidity);
    FIELD(ORB_HOME_MOTION_VALUE, motion); FIELD(ORB_HOME_CONTACT_VALUE, contact);
    FIELD(ORB_HOME_LOCKED, locked); FIELD(ORB_HOME_SPEED, speed); FIELD(ORB_HOME_POSITION, position);
    FIELD(ORB_HOME_BATTERY, battery); FIELD(ORB_HOME_LOW_BATTERY, low_battery);
#undef FIELD
    to->present |= from->present;
}
static orb_home_accessory_t *find_mut(orb_home_snapshot_t *view, const char *id)
{
    for (size_t i = 0; i < view->accessory_count; i++) if (!strcmp(view->accessories[i].id, id)) return &view->accessories[i];
    return NULL;
}
const orb_home_accessory_t *orb_home_find(const orb_home_snapshot_t *view, const char *id)
{
    return view && id ? find_mut((orb_home_snapshot_t *)view, id) : NULL;
}
static void advance(uint64_t *counter) { if (++*counter == 0) ++*counter; }
static void change(void) { advance(&s_store->view.revision); }
static void failure(const char *id)
{
    const orb_home_accessory_t *a = find_mut(&s_store->view, id);
    const orb_home_scene_t *scene = NULL;
    for (size_t i = 0; !a && i < s_store->view.scene_count; i++)
        if (!strcmp(s_store->view.scenes[i].id, id)) scene = &s_store->view.scenes[i];
    snprintf(s_store->view.error, sizeof(s_store->view.error), a ? "Couldn't reach %s" : "Couldn't run %s",
        a ? a->name : scene ? scene->name : "scene");
    snprintf(s_store->view.error_id, sizeof(s_store->view.error_id), "%s", id);
}
static void finish_room_off(bool failed)
{
    s_store->view.room_off_completed++;
    if (failed) s_store->view.room_off_failed++;
    if (s_store->view.room_off_completed == s_store->view.room_off_total)
        s_store->view.room_off_pending = false;
}
static void cancel_pending(void)
{
    /* Preserve loss of authority even when reconnect/replacement finishes
     * between UI polls and the accessory's final bytes are unchanged. */
    advance(&s_store->view.generation);
    for (size_t i = 0; i < ORB_HOME_MAX_PENDING; i++) {
        pending_t *p = &s_store->pending[i];
        if (!p->active) continue;
        orb_home_accessory_t *a = find_mut(&s_store->view, p->request.id);
        if (a) { a->state = p->before; a->pending = false; }
        if (p->room_off) finish_room_off(true);
        failure(p->request.id); p->active = false;
    }
    for (size_t i = 0; i < s_store->view.accessory_count; i++) {
        if (!(s_store->room_off_remaining & (UINT64_C(1) << i))) continue;
        s_store->view.accessories[i].pending = false;
        finish_room_off(true);
    }
    s_store->room_off_remaining = 0;
    s_store->view.scene_pending = false;
}
bool orb_home_init(void)
{
    if (s_store) return true;
#ifdef ESP_PLATFORM
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buffer);
    if (!orb_memory_reserve(ORB_MEMORY_MODEL, sizeof(*s_store))) return false;
    s_store = heap_caps_calloc(1, sizeof(*s_store), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    s_store = calloc(1, sizeof(*s_store));
#endif
    if (!s_store) {
#ifdef ESP_PLATFORM
        orb_memory_release(ORB_MEMORY_MODEL, sizeof(*s_store));
#endif
        return false;
    }
#ifndef ESP_PLATFORM
    orb_memory_track(ORB_MEMORY_MODEL, sizeof(*s_store));
#endif
    s_store->view.revision = s_store->view.generation = 1;
    return true;
}
void orb_home_reset(void)
{
    if (!s_store) return;
    lock(); uint64_t rev = s_store->view.revision, generation = s_store->view.generation; uint32_t seq = s_store->sequence;
    memset(s_store, 0, sizeof(*s_store)); s_store->view.revision = rev;
    s_store->view.generation = generation; advance(&s_store->view.generation);
    s_store->sequence = seq; change(); unlock();
}
uint64_t orb_home_revision(void)
{
    if (!s_store) return 0;
    lock(); uint64_t rev = s_store->view.revision; unlock(); return rev;
}
bool orb_home_glance_count(size_t *on_count)
{
    if (!s_store || !on_count) return false;
    lock();
    bool ready = s_store->view.online && s_store->view.synced && s_store->view.available;
    if (ready) *on_count = orb_home_on_count(&s_store->view);
    unlock();
    return ready;
}

size_t orb_home_memory_bytes(void)
{
    return s_store ? sizeof(*s_store) : 0;
}
bool orb_home_snapshot(orb_home_snapshot_t *out)
{
    if (!s_store || !out) return false;
    lock(); *out = s_store->view; unlock(); return true;
}
bool orb_home_snapshot_if_changed(uint64_t revision, orb_home_snapshot_t *out)
{
    if (!s_store || !out) return false;
    lock(); bool changed = revision != s_store->view.revision;
    if (changed) *out = s_store->view;
    unlock(); return changed;
}
void orb_home_set_connection(bool online, bool synced)
{
    if (!s_store) return;
    lock(); synced = online && synced;
    if (online != s_store->view.online || synced != s_store->view.synced) {
        if (!synced) cancel_pending();
        s_store->view.online = online; s_store->view.synced = synced; change();
    }
    if (!online) s_store->staging = false;
    unlock();
}
void orb_home_snapshot_discard(void)
{
    if (!s_store) return;
    lock(); s_store->staging = false;
    if (s_store->view.synced) {
        cancel_pending(); s_store->view.synced = false; change();
    }
    unlock();
}
static bool accessory_ok(const orb_home_accessory_t *a)
{
    return alias_ok(a->id) && text_ok(a->name, sizeof(a->name), false) &&
        alias_ok(a->room) && a->kind >= ORB_HOME_LIGHT && a->kind <= ORB_HOME_UNKNOWN &&
        !(a->writable & ~ORB_HOME_WRITABLE) && state_ok(&a->state);
}
bool orb_home_snapshot_chunk(const orb_home_room_t *rooms, size_t nr,
    const orb_home_accessory_t *accessories, size_t na, const orb_home_scene_t *scenes, size_t ns,
    bool reset, bool more)
{
    return orb_home_snapshot_chunk_available(rooms, nr, accessories, na, scenes, ns, true, reset, more);
}
bool orb_home_snapshot_chunk_available(const orb_home_room_t *rooms, size_t nr,
    const orb_home_accessory_t *accessories, size_t na, const orb_home_scene_t *scenes, size_t ns,
    bool available, bool reset, bool more)
{
    if (!s_store || nr > ORB_HOME_MAX_ROOMS || na > ORB_HOME_MAX_ACCESSORIES || ns > ORB_HOME_MAX_SCENES ||
        (nr && !rooms) || (na && !accessories) || (ns && !scenes)) {
        /* Rejecting a cheap envelope check must invalidate the same stage as
         * an invalid object: a later continuation cannot repair lost data. */
        orb_home_snapshot_discard(); return false;
    }
    lock();
    if (reset) {
        /* A membership replacement can revoke a pending object's alias. Do
         * not dispatch intents while the replacement is incomplete. */
        cancel_pending();
        if (s_store->view.synced) { s_store->view.synced = false; change(); }
        memset(&s_store->stage, 0, sizeof(s_store->stage));
        s_store->stage.available = available; s_store->staging = true;
    }
    orb_home_snapshot_t *st = &s_store->stage;
    bool ok = s_store->staging && st->available == available && st->room_count + nr <= ORB_HOME_MAX_ROOMS &&
        st->accessory_count + na <= ORB_HOME_MAX_ACCESSORIES && st->scene_count + ns <= ORB_HOME_MAX_SCENES;
    for (size_t i = 0; ok && i < nr; i++) {
        ok = alias_ok(rooms[i].id) && text_ok(rooms[i].name, sizeof(rooms[i].name), false);
        for (size_t j = 0; ok && j < st->room_count; j++) ok = strcmp(st->rooms[j].id, rooms[i].id) != 0;
        if (ok) st->rooms[st->room_count++] = rooms[i];
    }
    for (size_t i = 0; ok && i < na; i++) {
        ok = accessory_ok(&accessories[i]) && !find_mut(st, accessories[i].id);
        bool has_room = false;
        for (size_t j = 0; j < st->room_count; j++) if (!strcmp(st->rooms[j].id, accessories[i].room)) has_room = true;
        ok = ok && has_room && (available || !accessories[i].reachable);
        if (ok) { st->accessories[st->accessory_count] = accessories[i]; st->accessories[st->accessory_count++].pending = false; }
    }
    for (size_t i = 0; ok && i < ns; i++) {
        ok = alias_ok(scenes[i].id) && text_ok(scenes[i].name, sizeof(scenes[i].name), false);
        for (size_t j = 0; ok && j < st->scene_count; j++) ok = strcmp(st->scenes[j].id, scenes[i].id) != 0;
        if (ok) st->scenes[st->scene_count++] = scenes[i];
    }
    if (!ok) {
        s_store->staging = false;
        if (s_store->view.synced) { cancel_pending(); s_store->view.synced = false; change(); }
    }
    else if (!more) {
        /* A new authoritative snapshot supersedes all pre-reconnect optimism;
         * late replies carry sequence IDs and cannot restore the old view. */
        uint64_t revision = s_store->view.revision, generation = s_store->view.generation;
        bool online = s_store->view.online;
        s_store->view = *st; s_store->view.revision = revision; s_store->view.generation = generation;
        s_store->view.online = online; s_store->view.synced = true;
        memset(s_store->pending, 0, sizeof(s_store->pending)); s_store->staging = false; change();
    }
    unlock(); return ok;
}
bool orb_home_update(const char *id, const orb_home_state_t *patch, bool has_reachable, bool reachable, uint64_t updated)
{
    if (!s_store || !alias_ok(id) || !state_ok(patch)) return false;
    lock(); orb_home_accessory_t *a = find_mut(&s_store->view, id);
    if (!a || !s_store->view.online || !s_store->view.synced || !s_store->view.available || s_store->staging) { unlock(); return false; }
    orb_home_accessory_t old = *a;
    pending_t *pending = NULL;
    for (size_t i = 0; i < ORB_HOME_MAX_PENDING; i++) if (s_store->pending[i].active && !strcmp(s_store->pending[i].request.id, id)) pending = &s_store->pending[i];
    if (pending) {
        /* Rollback must retain intervening authoritative SSE changes. */
        merge(&pending->before, patch); pending->authoritative |= patch->present;
        a->state = pending->before;
        orb_home_state_t optimistic = pending->request.values;
        optimistic.present &= ~pending->authoritative;
        merge(&a->state, &optimistic);
    } else merge(&a->state, patch);
    if (has_reachable) a->reachable = reachable;
    if (pending && has_reachable && !reachable) {
        /* Do not dispatch a queued actuator write after the hub has explicitly
         * marked it No Response. Already sent replies are now stale by token. */
        a->state = pending->before;
        a->pending = false;
        pending->active = false;
        if (pending->room_off) finish_room_off(true);
        failure(id);
    }
    if (updated) a->updated = updated;
    if (memcmp(&old, a, sizeof(old))) change();
    drain_room_off();
    unlock(); return true;
}
static pending_t *available(void)
{
    for (size_t i = 0; i < ORB_HOME_MAX_PENDING; i++) if (!s_store->pending[i].active) return &s_store->pending[i];
    return NULL;
}
static bool busy(const char *id)
{
    snprintf(s_store->view.error, sizeof(s_store->view.error), "Home is busy");
    snprintf(s_store->view.error_id, sizeof(s_store->view.error_id), "%s", id);
    change(); return false;
}
static bool queue_accessory(const char *id, const orb_home_state_t *values, orb_home_request_kind_t kind, bool deliberate, bool room_off)
{
    orb_home_accessory_t *a = find_mut(&s_store->view, id);
    /* HIG §7: a deliberate gesture cannot make an unknown lock state safe.
     * The model enforces the same current-state precondition as the UI. */
    if (!a || !s_store->view.online || !s_store->view.synced || !s_store->view.available || s_store->staging || !a->reachable || (a->pending && !room_off) ||
        !values->present || (values->present & ~ORB_HOME_WRITABLE) || (values->present & ~a->writable) ||
        ((values->present & ORB_HOME_LOCKED) && !(a->state.present & ORB_HOME_LOCKED)) ||
        ((values->present & ORB_HOME_LOCKED) && !values->locked && (!deliberate || a->kind != ORB_HOME_LOCK))) return false;
    pending_t *p = available();
    if (!p) return room_off ? false : busy(id);
    memset(p, 0, sizeof(*p)); p->active = true; p->room_off = room_off; p->before = a->state;
    if (++s_store->sequence == 0) ++s_store->sequence;
    p->request.sequence = s_store->sequence; p->request.kind = kind;
    memcpy(p->request.id, id, strlen(id) + 1); p->request.values = *values;
    merge(&a->state, values); a->pending = true;
    if (!room_off) { s_store->view.error[0] = '\0'; s_store->view.error_id[0] = '\0'; }
    if (s_store->view.demo) { a->pending = false; p->active = false; }
    change(); return true;
}
bool orb_home_set(const char *id, const orb_home_state_t *values, bool deliberate_unlock)
{
    if (!s_store || !alias_ok(id) || !state_ok(values)) return false;
    lock(); bool ok = queue_accessory(id, values, ORB_HOME_REQUEST_SET, deliberate_unlock, false); unlock(); return ok;
}
bool orb_home_toggle(const char *id)
{
    if (!s_store || !alias_ok(id)) return false;
    lock(); orb_home_accessory_t *a = find_mut(&s_store->view, id);
    /* Security controls can only be reached through their explicit detail UI. */
    bool ok = a && a->kind != ORB_HOME_LOCK && (a->state.present & ORB_HOME_ON);
    orb_home_state_t values = {.present = ORB_HOME_ON, .on = a ? !a->state.on : false};
    if (ok) ok = queue_accessory(id, &values, ORB_HOME_REQUEST_TOGGLE, false, false);
    unlock(); return ok;
}
bool orb_home_scene(const char *id)
{
    if (!s_store || !alias_ok(id)) return false;
    lock(); bool found = false;
    for (size_t i = 0; i < s_store->view.scene_count; i++) if (!strcmp(s_store->view.scenes[i].id, id)) found = !s_store->view.scenes[i].contains_unlock;
    bool ok = found && s_store->view.online && s_store->view.synced && s_store->view.available && !s_store->staging && !s_store->view.scene_pending && !s_store->view.room_off_pending;
    pending_t *p = available();
    if (ok && !p) { busy(id); unlock(); return false; }
    if (ok) {
        memset(p, 0, sizeof(*p)); p->active = true;
        if (++s_store->sequence == 0) ++s_store->sequence;
        p->request.sequence = s_store->sequence; p->request.kind = ORB_HOME_REQUEST_SCENE;
        memcpy(p->request.id, id, strlen(id) + 1); memcpy(s_store->view.last_scene, id, strlen(id) + 1);
        s_store->view.scene_pending = true; s_store->view.error[0] = '\0'; s_store->view.error_id[0] = '\0';
        if (s_store->view.demo) {
            bool off = !strcmp(id, "good-night") || !strcmp(id, "leaving");
            for (size_t i = 0; i < s_store->view.accessory_count; i++) {
                orb_home_accessory_t *a = &s_store->view.accessories[i];
                if (a->reachable && (a->kind == ORB_HOME_LIGHT || a->kind == ORB_HOME_SWITCH)) a->state.on = !off;
            }
            s_store->view.scene_pending = false; p->active = false;
        }
        change();
    }
    unlock(); return ok;
}
static void drain_room_off(void)
{
    if (!s_store->view.room_off_pending) return;
    for (size_t i = 0; i < s_store->view.accessory_count && available(); i++) {
        uint64_t bit = UINT64_C(1) << i;
        if (!(s_store->room_off_remaining & bit)) continue;
        s_store->room_off_remaining &= ~bit;
        orb_home_accessory_t *a = &s_store->view.accessories[i];
        if ((a->state.present & ORB_HOME_ON) && !a->state.on) {
            a->pending = false; finish_room_off(false); continue;
        }
        if (!queue_accessory(a->id, &(orb_home_state_t){.present = ORB_HOME_ON}, ORB_HOME_REQUEST_SET, false, true)) {
            a->pending = false; failure(a->id); finish_room_off(true);
        } else if (s_store->view.demo) finish_room_off(false);
    }
}
bool orb_home_room_off(const char *room_id)
{
    if (!s_store || !alias_ok(room_id)) return false;
    lock();
    bool found = false;
    for (size_t i = 0; i < s_store->view.room_count; i++)
        if (!strcmp(s_store->view.rooms[i].id, room_id)) found = true;
    bool ok = found && s_store->view.online && s_store->view.synced && s_store->view.available &&
        !s_store->staging && !s_store->view.scene_pending && !s_store->view.room_off_pending;
    if (ok) {
        for (size_t i = 0; i < s_store->view.accessory_count; i++) {
            orb_home_accessory_t *a = &s_store->view.accessories[i];
            if (!strcmp(a->room, room_id) && orb_home_summary_accessory(a) && a->pending) ok = false;
        }
    }
    if (ok) {
        s_store->room_off_remaining = 0;
        s_store->view.room_off_total = s_store->view.room_off_completed = s_store->view.room_off_failed = 0;
        snprintf(s_store->view.room_off_id, sizeof(s_store->view.room_off_id), "%s", room_id);
        s_store->view.error[0] = '\0'; s_store->view.error_id[0] = '\0';
        for (size_t i = 0; i < s_store->view.accessory_count; i++) {
            orb_home_accessory_t *a = &s_store->view.accessories[i];
            if (strcmp(a->room, room_id) || !orb_home_summary_accessory(a) ||
                !(a->state.present & ORB_HOME_ON) || !a->state.on) continue;
            s_store->view.room_off_total++;
            if (!a->reachable || !(a->writable & ORB_HOME_ON)) {
                failure(a->id); finish_room_off(true); continue;
            }
            s_store->room_off_remaining |= UINT64_C(1) << i;
            a->pending = true;
        }
        s_store->view.room_off_pending = s_store->view.room_off_completed < s_store->view.room_off_total;
        drain_room_off(); change();
    }
    unlock(); return ok;
}
bool orb_home_take_request(orb_home_request_t *out)
{
    if (!s_store || !out) return false;
    lock(); pending_t *oldest = NULL;
    for (size_t i = 0; i < ORB_HOME_MAX_PENDING; i++) {
        pending_t *p = &s_store->pending[i];
        if (p->active && !p->claimed && (!oldest ||
            (uint32_t)(s_store->sequence - p->request.sequence) >
            (uint32_t)(s_store->sequence - oldest->request.sequence))) oldest = p;
    }
    /* Reusing the first free slot must not let a long room batch continually
     * overtake intents already waiting in the other three slots. */
    if (oldest) { *out = oldest->request; oldest->claimed = true; }
    unlock(); return oldest != NULL;
}
void orb_home_complete(const orb_home_request_t *request, const orb_home_state_t *accepted, const char *error)
{
    if (!s_store || !request || (accepted && !state_ok(accepted))) return;
    lock();
    for (size_t i = 0; i < ORB_HOME_MAX_PENDING; i++) {
        pending_t *p = &s_store->pending[i];
        if (!p->active || p->request.sequence != request->sequence || strcmp(p->request.id, request->id)) continue;
        orb_home_accessory_t *a = find_mut(&s_store->view, request->id);
        if (a) {
            a->state = p->before;
            if (!error) {
                /* The write result is a complete compact provider state, not
                 * a patch: missing lock/on fields must become unknown. */
                a->state = accepted ? *accepted : (orb_home_state_t){0};
                /* Reply computation can precede a later SSE change even when
                 * its delivery follows it. Without wire revisions, preserve
                 * every authoritative field received since this intent. */
                orb_home_state_t authoritative = p->before;
                authoritative.present &= p->authoritative;
                merge(&a->state, &authoritative);
            }
            a->pending = false;
        }
        if (request->kind == ORB_HOME_REQUEST_SCENE) s_store->view.scene_pending = false;
        if (p->room_off) finish_room_off(error != NULL);
        if (error) failure(request->id);
        else if (!p->room_off) { s_store->view.error[0] = '\0'; s_store->view.error_id[0] = '\0'; }
        p->active = false; drain_room_off(); change(); break;
    }
    unlock();
}
bool orb_home_summary_accessory(const orb_home_accessory_t *a)
{
    return a && (a->kind == ORB_HOME_LIGHT || a->kind == ORB_HOME_SWITCH || a->kind == ORB_HOME_OUTLET);
}
size_t orb_home_on_count(const orb_home_snapshot_t *view)
{
    size_t count = 0;
    if (view) for (size_t i = 0; i < view->accessory_count; i++) {
        const orb_home_accessory_t *a = &view->accessories[i];
        count += orb_home_summary_accessory(a) && (a->state.present & ORB_HOME_ON) && a->state.on;
    }
    return count;
}
void orb_home_load_demo(void)
{
    if (!orb_home_init()) return;
    orb_home_reset(); lock(); orb_home_snapshot_t *v = &s_store->view;
    v->online = v->synced = v->available = v->demo = true;
    static const char *const rooms[] = {"Living Room", "Bedroom", "Kitchen", "Office", "Dining"};
    for (size_t i = 0; i < 5; i++) { snprintf(v->rooms[i].id, 33, "%s", rooms[i]); snprintf(v->rooms[i].name, 33, "%s", rooms[i]); }
    v->room_count = 5;
    const struct { const char *id, *name; unsigned room; orb_home_kind_t kind; bool on; float hue, sat; } fixtures[] = {
        {"floor-lamp", "Floor Lamp", 0, ORB_HOME_LIGHT, true, 35, 50},
        {"sofa-lamp", "Ceiling Lamp", 0, ORB_HOME_LIGHT, true, 280, 65},
        {"bedside", "Bedside Lamp", 1, ORB_HOME_LIGHT, false, 30, 25},
        {"kitchen", "Kitchen Lights", 2, ORB_HOME_SWITCH, true, 0, 0},
        {"office", "Desk Light", 3, ORB_HOME_LIGHT, true, 205, 65},
        {"climate", "Thermostat", 0, ORB_HOME_THERMOSTAT, true, 0, 0},
        {"front-door", "Front Door", 0, ORB_HOME_LOCK, false, 0, 0},
        {"fan", "Bedroom Fan", 1, ORB_HOME_FAN, true, 0, 0},
        {"dining", "Dining Pendant", 4, ORB_HOME_LIGHT, true, 0, 0},
        {"coffee", "Coffee Machine", 2, ORB_HOME_OUTLET, true, 0, 0},
        {"tv", "TV", 0, ORB_HOME_SWITCH, false, 0, 0},
        {"bookshelf", "Bookshelf Light", 3, ORB_HOME_LIGHT, false, 0, 0},
        {"bed-temp", "Bedroom Sensor", 1, ORB_HOME_TEMPERATURE, false, 0, 0},
        {"living-temp", "Living Room Sensor", 0, ORB_HOME_TEMPERATURE, false, 0, 0},
        {"kitchen-temp", "Kitchen Sensor", 2, ORB_HOME_HUMIDITY, false, 0, 0},
        {"office-temp", "Office Sensor", 3, ORB_HOME_TEMPERATURE, false, 0, 0},
        {"hall-motion", "Hall Motion", 4, ORB_HOME_MOTION, false, 0, 0},
        {"window", "Office Window", 3, ORB_HOME_CONTACT, false, 0, 0},
    };
    v->accessory_count = sizeof(fixtures) / sizeof(fixtures[0]);
    for (size_t i = 0; i < v->accessory_count; i++) {
        orb_home_accessory_t *a = &v->accessories[i];
        snprintf(a->id, 33, "%s", fixtures[i].id); snprintf(a->name, 33, "%s", fixtures[i].name);
        snprintf(a->room, 33, "%s", rooms[fixtures[i].room]); a->kind = fixtures[i].kind;
        a->reachable = i != 11; a->favourite = i < 8; a->updated = 1793138160;
        if (a->kind <= ORB_HOME_THERMOSTAT || a->kind == ORB_HOME_FAN) {
            a->state.present = a->writable = ORB_HOME_ON; a->state.on = fixtures[i].on;
        }
        if (a->kind == ORB_HOME_LIGHT) {
            a->state.present |= ORB_HOME_BRIGHTNESS | ORB_HOME_CT; a->writable |= ORB_HOME_BRIGHTNESS | ORB_HOME_CT;
            a->state.brightness = i == 0 ? 60 : 70; a->state.ct = 300;
            if (i < 5) { a->state.present |= ORB_HOME_HUE | ORB_HOME_SAT; a->writable |= ORB_HOME_HUE | ORB_HOME_SAT; a->state.hue = fixtures[i].hue; a->state.sat = fixtures[i].sat; }
        }
        if (a->kind == ORB_HOME_THERMOSTAT) { a->state.present |= ORB_HOME_TEMP | ORB_HOME_TARGET | ORB_HOME_MODE; a->writable |= ORB_HOME_TARGET | ORB_HOME_MODE; a->state.temp = 21.5f; a->state.target = 22.2f; a->state.mode = ORB_HOME_MODE_HEAT; }
        if (a->kind == ORB_HOME_FAN) { a->state.present |= ORB_HOME_SPEED; a->writable |= ORB_HOME_SPEED; a->state.speed = 40; }
        if (a->kind == ORB_HOME_LOCK) { a->state.present = a->writable = ORB_HOME_LOCKED; a->state.locked = true; }
        if (i >= 12 && i <= 17) {
            a->state.present = ORB_HOME_BATTERY | ORB_HOME_LOW_BATTERY; a->state.battery = i == 16 ? 15 : 84; a->state.low_battery = i == 16;
            if (i <= 15) { a->state.present |= ORB_HOME_TEMP | ORB_HOME_HUMIDITY_VALUE; a->state.temp = 20 + (float)(i % 3); a->state.humidity = 45 + (float)(i % 5); }
            if (a->kind == ORB_HOME_MOTION) { a->state.present |= ORB_HOME_MOTION_VALUE; a->state.motion = true; }
            if (a->kind == ORB_HOME_CONTACT) a->state.present |= ORB_HOME_CONTACT_VALUE;
        }
    }
    static const char *const scene_ids[] = {"morning", "evening", "good-night", "leaving", "arriving"};
    static const char *const scene_names[] = {"Good morning", "Evening", "Good Night", "I'm leaving", "I'm home"};
    v->scene_count = 5;
    for (size_t i = 0; i < 5; i++) { snprintf(v->scenes[i].id, 33, "%s", scene_ids[i]); snprintf(v->scenes[i].name, 33, "%s", scene_names[i]); v->scenes[i].favourite = true; }
    change(); unlock();
}
