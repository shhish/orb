/* Faces' production descriptors and compatibility host: value contexts, copied
 * projections and retained editors are exercised without widget child indexes. */
#include "orb_app_faces.h"
#include "orb_app_faces_registry.h"
#include "orb_app_host.h"
#include "orb_memory.h"
#include "orb_screen_lifecycle.h"
#include "orb_settings.h"
#include "orb_time.h"
#include "orb_ui.h"
#include "sim_display.h"
#ifdef ORB_SETTINGS_TEST_STORAGE
#include "orb_fs.h"
#endif

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CONTEXT_BYTES (sizeof(orb_screen_context_t) + sizeof(orb_app_faces_context_t))

static uint32_t s_now;
static uint64_t s_revision;
static unsigned s_timers, s_intents, s_commands;
static size_t s_workspace;
static bool s_reject_request, s_public;
static orb_intent_t s_last;
static orb_app_faces_model_t s_model = {.snapshot = {.now = 1793208527,
    .clock = "17:28", .weekday = "Wednesday", .day = "28", .usage_percent = {62, 38},
    .battery_percent = 80, .online = true, .weather_override = true}};
static orb_screen_model_t s_model_view = {&s_model, sizeof(s_model)};
static orb_app_faces_context_t s_editor;

static uint32_t tick(void) { return s_now; }

static unsigned timer_count(void)
{
    unsigned count = 0;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer)) count++;
    return count;
}

static unsigned running_content_timers(void)
{
    unsigned count = 0;
    lv_timer_t *refresh = lv_display_get_refr_timer(lv_display_get_default());
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer))
        count += timer != refresh && !lv_timer_get_paused(timer);
    return count;
}

static size_t workspace_used(void)
{
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    return stats.used;
}

static void settle(void)
{
    for (unsigned i = 0; i < 40; i++) { s_now += 16; lv_timer_handler(); }
}

static void quiescent(void)
{
    settle();
    assert(workspace_used() == s_workspace);
    assert(timer_count() == s_timers);
}

static orb_app_faces_context_t context_of(orb_screen_instance_t *instance)
{
    unsigned char bytes[CONTEXT_BYTES] = {0};
    assert(orb_screen_descriptor(instance)->context_bytes == sizeof(bytes));
    orb_screen_context_t *context = (orb_screen_context_t *)bytes;
    assert(orb_screen_save(instance, context, sizeof(bytes)));
    orb_app_faces_context_t value;
    memcpy(&value, context->payload, sizeof(value));
    return value;
}

static bool restore_value(orb_screen_instance_t *instance, const orb_app_faces_context_t *value)
{
    unsigned char bytes[CONTEXT_BYTES] = {0};
    orb_screen_context_t *context = (orb_screen_context_t *)bytes;
    context->version = orb_screen_descriptor(instance)->context_version;
    context->size = sizeof(bytes);
    memcpy(context->payload, value, sizeof(*value));
    return orb_screen_restore(instance, context, sizeof(bytes));
}

static orb_faces_prefs_t prefs_of(const orb_app_faces_context_t *value)
{
    orb_faces_prefs_t prefs;
    unsigned char *bytes = (unsigned char *)&prefs;
    for (size_t i = 0; i < sizeof(prefs); i++) {
        assert(value->prefs[i] <= UINT8_MAX);
        bytes[i] = (uint8_t)value->prefs[i];
    }
    return prefs;
}

static void set_prefs(orb_app_faces_context_t *value, const orb_faces_prefs_t *prefs)
{
    const unsigned char *bytes = (const unsigned char *)prefs;
    for (size_t i = 0; i < sizeof(*prefs); i++) value->prefs[i] = bytes[i];
}

static bool intent(void *context, const orb_intent_t *value)
{
    (void)context;
    s_intents++;
    s_last = *value;
    return true;
}

static bool project(void *context, orb_screen_id_t id, orb_screen_update_t *update)
{
    (void)context;
    assert(id == (s_public ? ORB_APP_FACES_PUBLIC_SCREEN_ID : ORB_APP_FACES_SCREEN_ID));
    *update = (orb_screen_update_t){.revisions.time = s_revision, .model = &s_model_view};
    return true;
}

static bool allow_synthetic(void *context, const orb_intent_t *value)
{
    (void)context;
    orb_faces_prefs_t prefs;
    return orb_app_faces_preferences_from_intent(value, &prefs);
}

static bool command(void *context, orb_screen_instance_t *instance, const orb_intent_t *value)
{
    (void)context;
    orb_faces_prefs_t prefs;
    assert(orb_app_faces_preferences_from_intent(value, &prefs));
    assert(value->origin == ORB_INPUT_TOUCH);
    s_commands++;
    s_last = *value;
    uint64_t ticket = s_reject_request ? 0 : orb_settings_faces_request(&prefs);
    orb_app_faces_request_result(instance, ticket);
    return true;
}

static bool host_allow_synthetic(const orb_intent_t *value)
{
    return allow_synthetic(NULL, value);
}

static orb_app_host_t *new_host(bool preview, bool public)
{
    return orb_app_host_create(preview, &(orb_app_host_config_t){
        .descriptor = public ? orb_app_faces_public_descriptor : orb_app_faces_descriptor,
        .name = "Faces", .model_bytes = sizeof(s_model), .command = command,
        .allow_synthetic = host_allow_synthetic, .rim_finish = orb_app_faces_rotary_finish});
}

static void exercise(orb_screen_instance_t *instance)
{
    orb_app_faces_context_t value = context_of(instance);
    if (!s_public) {
        orb_faces_prefs_t prefs = prefs_of(&value);
        prefs.face = ORB_FACE_MERIDIAN;
        prefs.meridian_slot[3] = ORB_FACE_NEXT;
        set_prefs(&value, &prefs);
        value.mode = ORB_FACES_SLOTS;
        value.slot = 3;
        value.face_before_gallery = ORB_FACE_ORB;
        value.browsing = value.dirty = value.save_error = value.failed_select = true;
        assert(restore_value(instance, &value));
    }
    s_editor = context_of(instance);
    assert(!s_intents);
}

static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    assert(orb_screen_alive(instance));
    assert(!lv_obj_is_clickable(orb_screen_root(instance)));
    assert(lv_obj_is_event_bubble(orb_screen_root(instance)));
    orb_screen_chrome_t chrome;
    orb_screen_chrome(instance, &chrome);
    assert(!strcmp(chrome.title, "Face"));
    orb_app_faces_context_t value = context_of(instance);
    if (phase == ORB_LIFECYCLE_RESTORED || phase == ORB_LIFECYCLE_REMOVED_ID || phase == ORB_LIFECYCLE_EMPTY) {
        assert(!memcmp(&value, &s_editor, sizeof(value)));
        if (!s_public) {
            assert(orb_app_faces_mode(instance) == ORB_FACES_SLOTS);
            int current, maximum;
            assert(orb_app_faces_rotary(instance, &current, &maximum));
            assert(current == ORB_FACE_NEXT && maximum == ORB_FACE_PROVIDER_COUNT - 1);
        }
    } else {
        assert(value.mode == ORB_FACES_FACE && !value.browsing && !value.dirty);
    }
    if (s_public) assert(!orb_app_faces_rotary(instance, NULL, NULL));
}

static void lifecycle(orb_screen_host_t *host, bool public)
{
    s_public = public;
    orb_screen_update_t initial = {.revisions.time = ++s_revision, .model = &s_model_view};
    orb_screen_update_t changed = {.revisions.time = ++s_revision, .model = &s_model_view};
    orb_screen_update_t removed = {.revisions.time = ++s_revision, .model = &s_model_view};
    orb_screen_update_t empty = {.revisions.time = ++s_revision};
    orb_lifecycle_case_t test = {.descriptor = public ? orb_app_faces_public_descriptor() : orb_app_faces_descriptor(),
        .initial = &initial, .changed = &changed, .removed = &removed, .empty = &empty,
        .exercise = exercise, .verify = verify, .quiescent = quiescent};
    orb_test_screen_lifecycle(host, &test);
}

static lv_obj_t *page(void)
{
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(parent);
    lv_obj_set_size(parent, 466, 466);
    return parent;
}

static void copied_projection(void)
{
    assert(sizeof(s_model) > ORB_APP_HOST_MODEL_MAX);
    orb_app_host_t *host = new_host(false, false);
    assert(host);
    orb_app_faces_model_t model = s_model;
    snprintf(model.snapshot.weather.place, sizeof(model.snapshot.weather.place), "%s", "Copied projection tail");
    orb_app_faces_model_t expected = model;
    assert(orb_app_host_set_model(host, &model, sizeof(model)));
    memset(&model, 0x5a, sizeof(model));
    size_t size = 0;
    const void *copy = orb_app_host_model(host, &size);
    assert(size == sizeof(expected) && !memcmp(copy, &expected, size));
    assert(!orb_app_host_set_model(host, &expected, sizeof(expected) + 1));
    assert(!orb_app_host_set_model(host, NULL, sizeof(expected)));
    copy = orb_app_host_model(host, &size);
    assert(size == sizeof(expected) && !memcmp(copy, &expected, size));
    orb_app_host_t *clone = NULL;
    assert(orb_app_host_clone(&clone, host, true));
    assert(orb_app_host_set_model(clone, &expected, sizeof(expected)));
    orb_app_host_destroy(clone);
    orb_app_host_destroy(host);

    /* The extension is per host: the unchanged default remains a 64-byte
     * admission boundary for consumers that did not declare a larger model. */
    host = orb_app_host_create(false, &(orb_app_host_config_t){.descriptor = orb_app_faces_descriptor,
        .name = "Faces", .rim_finish = orb_app_faces_rotary_finish});
    assert(host && !orb_app_host_set_model(host, &expected, sizeof(expected)));
    assert(orb_app_host_set_model(host, &expected, ORB_APP_HOST_MODEL_MAX));
    orb_app_host_destroy(host);
    quiescent();
}

static void public_projection(void)
{
    orb_faces_snapshot_t value = s_model.snapshot;
    value.home_on = 7;
    value.home_total = 9;
    value.sessions = 4;
    value.working = 2;
    value.next_seconds = 100;
    value.weather.configured = value.weather.available = true;
    value.weather.temperature = 72;
    snprintf(value.weather.place, sizeof(value.weather.place), "%s", "Private household location");
    orb_app_faces_public_projection(&value);
    assert(!value.home_on && !value.home_total && !value.weather.place[0]);
    assert(value.weather_override && value.weather.temperature == 72);
    assert(value.usage_percent[0] == 62 && value.usage_percent[1] == 38);
    assert(value.sessions == 4 && value.working == 2 && value.next_seconds == 100);
    assert(value.battery_percent == 80 && !strcmp(value.clock, "17:28"));
}

static void malformed_context(orb_screen_host_t *host)
{
    s_public = false;
    orb_screen_instance_t instance = {0};
    assert(orb_screen_create(&instance, host, orb_app_faces_descriptor(), lv_screen_active(), NULL, false));
    orb_app_faces_context_t original = context_of(&instance), value = original;
    value.mode = ORB_FACES_SLOTS + 1;
    assert(!restore_value(&instance, &value));
    value = original; value.slot = 4;
    assert(!restore_value(&instance, &value));
    value = original; value.face_before_gallery = ORB_FACE_COUNT;
    assert(!restore_value(&instance, &value));
    value = original; value.prefs[0] = UINT8_MAX + 1u;
    assert(!restore_value(&instance, &value));
    value = original; value.prefs[1] = ORB_FACE_COUNT;
    assert(!restore_value(&instance, &value));
    value = context_of(&instance);
    assert(!memcmp(&value, &original, sizeof(value)) && !s_intents);
    orb_screen_destroy(&instance);
    quiescent();
}

static void retained_editor(void)
{
    orb_app_host_t *host = new_host(false, false);
    lv_obj_t *workspace = page(), *first = page();
    assert(host && orb_app_host_set_model(host, &s_model, sizeof(s_model)) && orb_app_host_attach(host, first));
    orb_screen_instance_t *instance = orb_app_host_current(host);
    orb_app_faces_context_t value = context_of(instance);
    orb_faces_prefs_t prefs = prefs_of(&value);
    prefs.face = ORB_FACE_MERIDIAN;
    prefs.meridian_slot[3] = ORB_FACE_NEXT;
    set_prefs(&value, &prefs);
    value.mode = ORB_FACES_SLOTS;
    value.slot = 3;
    value.face_before_gallery = ORB_FACE_ORB;
    value.browsing = true;
    assert(restore_value(instance, &value));
    lv_obj_t *root = orb_screen_root(instance), *view = orb_app_faces_root(instance);
    unsigned commands = s_commands;
    assert(orb_app_faces_retain(instance, workspace) && orb_app_faces_retained(instance));
    assert(lv_obj_get_parent(root) == workspace && lv_obj_is_hidden(root));
    orb_app_host_detach_page(host, first);
    lv_obj_delete(first);
    assert(orb_app_host_current(host) == instance && orb_screen_alive(instance));
    lv_obj_t *second = page();
    assert(orb_app_faces_resume(instance, second) && !orb_app_faces_retained(instance));
    assert(orb_app_faces_root(instance) == view && orb_screen_root(instance) == root);
    assert(lv_obj_get_parent(root) == second && !lv_obj_is_hidden(root));
    orb_app_faces_context_t restored = context_of(instance);
    assert(!memcmp(&value, &restored, sizeof(value)) && s_commands == commands);
    lv_obj_delete(workspace);
    assert(orb_screen_alive(instance));
    lv_obj_delete(second);
    assert(!orb_app_host_current(host));
    orb_app_host_destroy(host);
    quiescent();
}

#ifndef ORB_SETTINGS_TEST_STORAGE
static void preference_intents(void)
{
    orb_settings_init();
    orb_app_host_t *host = new_host(false, false);
    lv_obj_t *parent = page();
    assert(host && orb_app_host_set_model(host, &s_model, sizeof(s_model)) && orb_app_host_attach(host, parent));
    orb_screen_instance_t *instance = orb_app_host_current(host);
    orb_app_faces_context_t value = context_of(instance);
    orb_faces_prefs_t prefs = prefs_of(&value), current;
    prefs.colour = 3;
    set_prefs(&value, &prefs);
    value.dirty = true;
    unsigned commands = s_commands;
    assert(restore_value(instance, &value) && s_commands == commands);
    s_reject_request = true;
    orb_app_faces_active(instance, false, true);
    value = context_of(instance);
    assert(value.dirty && value.save_error && s_commands == commands + 1);
    orb_settings_faces_current(&current);
    assert(current.colour != prefs.colour);
    s_reject_request = false;
    orb_app_faces_active(instance, false, true);
    value = context_of(instance);
    assert(!value.dirty && !value.save_error && s_commands == commands + 2);
    orb_faces_prefs_t decoded;
    assert(orb_app_faces_preferences_from_intent(&s_last, &decoded) && !memcmp(&decoded, &prefs, sizeof(prefs)));
    orb_settings_faces_current(&current);
    assert(!memcmp(&current, &prefs, sizeof(prefs)));
    orb_app_faces_active(instance, false, true);
    assert(s_commands == commands + 2); /* No duplicate save after success. */
    orb_intent_t invalid = s_last;
    invalid.kind = ORB_INTENT_SETTING_SET;
    assert(!orb_app_faces_preferences_from_intent(&invalid, &decoded));
    invalid = s_last; invalid.value.command.action = ORB_APP_FACES_OPEN;
    assert(!orb_app_faces_preferences_from_intent(&invalid, &decoded));
    invalid = s_last; invalid.value.command.target = 0;
    assert(!orb_app_faces_preferences_from_intent(&invalid, &decoded));
    lv_obj_delete(parent);
    orb_app_host_destroy(host);
    assert(s_commands == commands + 2);
    quiescent();

    /* A preview may hold the same draft but cannot issue a preference write. */
    host = new_host(true, false);
    parent = page();
    assert(host && orb_app_host_set_model(host, &s_model, sizeof(s_model)) && orb_app_host_attach(host, parent));
    instance = orb_app_host_current(host);
    value = context_of(instance);
    prefs = prefs_of(&value); prefs.colour = 4; set_prefs(&value, &prefs); value.dirty = true;
    assert(restore_value(instance, &value));
    orb_app_faces_active(instance, false, true);
    assert(s_commands == commands + 2 && !orb_app_faces_retain(instance, lv_screen_active()));
    lv_obj_delete(parent);
    orb_app_host_destroy(host);
    quiescent();
}

#endif

#ifdef ORB_SETTINGS_TEST_STORAGE
static bool s_storage_fail;
static unsigned s_prepares, s_writes;
static orb_faces_prefs_t s_record;

/* This target compiles the production settings service with a bounded storage
 * port. Only these in-memory stubs observe writes; no real filesystem is used. */
bool orb_fs_ready(void) { return true; }
esp_err_t orb_fs_read(const char *id, const char *key, void *data, size_t capacity, size_t *size)
{
    (void)data; (void)capacity; (void)size;
    assert(!strcmp(id, "faces") && !strcmp(key, "preferences"));
    return ESP_ERR_NOT_FOUND;
}
esp_err_t orb_fs_app_prepare(const char *id)
{
    assert(!strcmp(id, "faces"));
    s_prepares++;
    return ESP_OK;
}
esp_err_t orb_fs_write(const char *id, const char *key, const void *data, size_t size)
{
    assert(!strcmp(id, "faces") && !strcmp(key, "preferences") && size == sizeof(s_record));
    s_writes++;
    if (s_storage_fail) return ESP_FAIL;
    memcpy(&s_record, data, size);
    return ESP_OK;
}

static bool saving(void)
{
    orb_settings_snapshot_t result;
    orb_settings_snapshot(&result);
    return result.saving;
}

static void asynchronous_preferences(void)
{
    orb_settings_init();
    orb_app_host_t *host = new_host(false, false);
    lv_obj_t *parent = page(), *workspace = page();
    assert(host && orb_app_host_set_model(host, &s_model, sizeof(s_model)) && orb_app_host_attach(host, parent));
    orb_screen_instance_t *instance = orb_app_host_current(host);
    orb_app_faces_context_t value = context_of(instance);
    orb_faces_prefs_t prefs = prefs_of(&value);
    prefs.colour = 3;
    set_prefs(&value, &prefs);
    value.mode = ORB_FACES_COLOUR;
    value.dirty = true;
    assert(restore_value(instance, &value));
    unsigned commands = s_commands;
    assert(orb_screen_back(instance) == ORB_BACK_CONSUMED && saving());
    assert(s_commands == commands + 1 && !s_prepares && !s_writes);
    value = context_of(instance);
    assert(value.mode == ORB_FACES_COLOUR && value.dirty && !value.save_error);
    assert(!restore_value(instance, &value)); /* Pending resources cannot be overwritten by values. */
    assert(orb_screen_back(instance) == ORB_BACK_CONSUMED && s_commands == commands + 1);

    /* A system layer retains the same pending action and observer while its
     * old page is destroyed. Failure must return to that editor, not a clone. */
    lv_obj_t *view = orb_app_faces_root(instance);
    assert(orb_app_faces_retain(instance, workspace));
    orb_app_host_detach_page(host, parent);
    lv_obj_delete(parent);
    assert(orb_app_host_current(host) == instance && saving());
    s_storage_fail = true;
    orb_settings_service();
    assert(s_prepares == 1 && s_writes == 1 && !saving());
    parent = page();
    assert(orb_app_faces_resume(instance, parent) && orb_app_faces_root(instance) == view);
    orb_app_host_poll(host);
    value = context_of(instance);
    assert(value.mode == ORB_FACES_COLOUR && value.dirty && value.save_error);
    orb_settings_service(); orb_app_host_poll(host);
    assert(s_writes == 1 && s_commands == commands + 1); /* Failure never retries itself. */
    s_storage_fail = false;
    assert(orb_screen_back(instance) == ORB_BACK_CONSUMED && saving());
    assert(s_commands == commands + 2);
    orb_settings_service(); orb_app_host_poll(host);
    value = context_of(instance);
    assert(value.mode == ORB_FACES_GALLERY && !value.dirty && !value.save_error);
    assert(s_record.colour == 3 && s_writes == 2);

    /* An old successful write cannot clear a newer draft or finish its Back.
     * The original save timestamp/action stays bound to its copied candidate. */
    value.mode = ORB_FACES_COLOUR;
    assert(restore_value(instance, &value));
    orb_app_faces_rotate(instance, 4);
    assert(orb_screen_back(instance) == ORB_BACK_CONSUMED && saving());
    assert(s_commands == commands + 3);
    orb_app_faces_rotate(instance, 5);
    orb_settings_service(); orb_app_host_poll(host);
    value = context_of(instance);
    assert(s_record.colour == 4 && value.mode == ORB_FACES_COLOUR && value.dirty && !value.save_error);
    prefs = prefs_of(&value);
    assert(prefs.colour == 5 && s_commands == commands + 3 && s_writes == 3);
    assert(orb_screen_back(instance) == ORB_BACK_CONSUMED && saving());
    orb_settings_service(); orb_app_host_poll(host);
    value = context_of(instance);
    assert(value.mode == ORB_FACES_GALLERY && !value.dirty && !value.save_error);
    assert(s_record.colour == 5 && s_commands == commands + 4 && s_writes == 4);

    /* Display deactivation keeps the existing autosave point. Destruction
     * releases only its observer: the admitted copied write still completes. */
    value.mode = ORB_FACES_COLOUR;
    assert(restore_value(instance, &value));
    orb_app_faces_rotate(instance, 6);
    orb_app_faces_active(instance, false, true);
    assert(saving() && s_commands == commands + 5);
    lv_obj_delete(parent);
    orb_app_host_destroy(host);
    lv_obj_delete(workspace);
    assert(saving() && s_commands == commands + 5);
    orb_settings_service();
    assert(!saving() && s_record.colour == 6 && s_writes == 5);
    orb_settings_faces_current(&prefs);
    assert(prefs.colour == 6);
    quiescent();

    /* The same editor draft in a preview never requests persistence. */
    host = new_host(true, false);
    parent = page();
    assert(host && orb_app_host_set_model(host, &s_model, sizeof(s_model)) && orb_app_host_attach(host, parent));
    instance = orb_app_host_current(host);
    value = context_of(instance);
    prefs = prefs_of(&value); prefs.colour = 7; set_prefs(&value, &prefs); value.dirty = true;
    assert(restore_value(instance, &value));
    orb_app_faces_active(instance, false, true);
    assert(!saving() && s_commands == commands + 5 && s_writes == 5);
    lv_obj_delete(parent);
    orb_app_host_destroy(host);
    quiescent();
}
#endif

static void seconds_visibility(orb_screen_host_t *host)
{
    s_public = false;
    orb_screen_instance_t instance = {0};
    assert(orb_screen_create(&instance, host, orb_app_faces_descriptor(), lv_screen_active(), NULL, false));
    orb_screen_update_t update = {.revisions.time = ++s_revision, .model = &s_model_view};
    assert(orb_screen_update(&instance, &update));
    orb_app_faces_context_t value = context_of(&instance);
    orb_faces_prefs_t prefs = prefs_of(&value); prefs.face = ORB_FACE_MERIDIAN;
    set_prefs(&value, &prefs);
    assert(restore_value(&instance, &value));
    settle();
    unsigned active = running_content_timers();
    orb_screen_visibility(&instance, ORB_VISIBILITY_COVERED);
    settle();
    unsigned covered = running_content_timers();
    assert(active == covered + 1); /* Meridian's one seconds timer. */
    orb_screen_visibility(&instance, ORB_VISIBILITY_ACTIVE);
    assert(running_content_timers() == active);
    orb_app_faces_hold(&instance);
    settle();
    assert(orb_app_faces_mode(&instance) == ORB_FACES_GALLERY && running_content_timers() == covered);
    orb_screen_destroy(&instance);
    quiescent();

    s_public = true;
    orb_settings_faces_current(&prefs);
    prefs.face = ORB_FACE_MERIDIAN;
    assert(orb_settings_faces_accept(&prefs));
    assert(orb_screen_create(&instance, host, orb_app_faces_public_descriptor(), lv_screen_active(), NULL, false));
    update.revisions.time = ++s_revision;
    assert(orb_screen_update(&instance, &update));
    settle();
    unsigned public_timers = running_content_timers();
    orb_screen_visibility(&instance, ORB_VISIBILITY_COVERED);
    assert(running_content_timers() == public_timers);
    orb_screen_visibility(&instance, ORB_VISIBILITY_ACTIVE);
    assert(running_content_timers() == public_timers);
    orb_app_faces_hold(&instance);
    assert(orb_app_faces_mode(&instance) == ORB_FACES_FACE && !orb_app_faces_rotary(&instance, NULL, NULL));
    orb_screen_destroy(&instance);
    quiescent();
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    assert(sim_display_create());
    orb_ui_init(NULL);
    assert(orb_model_init());
    orb_time_init();
    orb_settings_init();
    orb_screen_registry_t registry;
    orb_screen_registry_init(&registry);
    assert(orb_screen_registry_add(&registry, orb_app_faces_manifest(), NULL));
    assert(orb_screen_registry_find(&registry, ORB_APP_FACES_SCREEN_ID) == orb_app_faces_descriptor());
    assert(orb_screen_registry_find(&registry, ORB_APP_FACES_PUBLIC_SCREEN_ID) == orb_app_faces_public_descriptor());
    orb_nav_t nav;
    orb_nav_init(&nav, ORB_APP_FACES_SCREEN_ID);
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.nav = &nav, .intent = intent,
        .project = project, .allow_synthetic = allow_synthetic});
    orb_screen_instance_t warm = {0};
    assert(orb_screen_create(&warm, &host, orb_app_faces_descriptor(), lv_screen_active(), NULL, false));
    orb_screen_destroy(&warm);
    settle();
    s_timers = timer_count();
    s_workspace = workspace_used();

    lifecycle(&host, false);
    lifecycle(&host, true);
    malformed_context(&host);
    copied_projection();
    public_projection();
    retained_editor();
#ifdef ORB_SETTINGS_TEST_STORAGE
    asynchronous_preferences();
#else
    preference_intents();
#endif
    seconds_visibility(&host);
    puts("app_faces: lifecycle, contexts, projection capacity, privacy, retained editor, preferences and seconds passed");
    return 0;
}
