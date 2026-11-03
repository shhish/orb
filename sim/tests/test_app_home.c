/* Production Home descriptor exercised through the shared lifetime driver and
 * the shell's compatibility host. Assertions read the value context (stable
 * IDs, page and scroll) and semantic queries, never presenter child order. */
#include "orb_app_home.h"
#include "orb_app_home_registry.h"
#include "home_context.h"
#include "orb_home.h"
#include "orb_home_command.h"
#include "orb_home_host.h"
#include "orb_memory.h"
#include "orb_screen_lifecycle.h"
#include "orb_ui.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned s_timer_baseline, s_intents, s_transitions;
static size_t s_workspace_baseline, s_model_baseline;
static orb_intent_t s_last_intent;
static orb_app_home_model_t s_model = {.hour = 19, .system_navigation = true};
static orb_screen_model_t s_model_view = {&s_model, sizeof(s_model)};
static uint64_t s_time;
static int32_t s_scroll;
static lv_area_t s_transition_area;
static bool s_transition_reverse, s_transition_prepare;

static unsigned timer_count(void)
{
    unsigned count = 0;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer)) count++;
    return count;
}

static size_t used(orb_memory_owner_t owner)
{
    orb_memory_stats_t stats;
    orb_memory_get(owner, &stats);
    return stats.used;
}

static void settle(void)
{
    for (unsigned i = 0; i < 8; i++) lv_timer_handler();
}

/* A scrollable room list, each with one dimmable light, through the public chunk
 * path. Revisions advance exactly as a hub snapshot would. */
static void publish(unsigned rooms)
{
    static const char *const ids[] = {"living", "office", "studio", "dining", "kitchen", "bedroom", "bathroom", "garage"};
    static const char *const names[] = {"Living Room", "Office", "Studio", "Dining", "Kitchen", "Bedroom", "Bathroom", "Garage"};
    orb_home_room_t room[8] = {0};
    orb_home_accessory_t accessory[8] = {0};
    for (unsigned i = 0; i < rooms; i++) {
        snprintf(room[i].id, sizeof(room[i].id), "%s", ids[i]);
        snprintf(room[i].name, sizeof(room[i].name), "%s", names[i]);
        snprintf(accessory[i].id, sizeof(accessory[i].id), "%s-lamp", ids[i]);
        snprintf(accessory[i].name, sizeof(accessory[i].name), "%s Lamp", names[i]);
        snprintf(accessory[i].room, sizeof(accessory[i].room), "%s", ids[i]);
        accessory[i].kind = ORB_HOME_LIGHT;
        accessory[i].reachable = true;
        accessory[i].writable = ORB_HOME_ON | ORB_HOME_BRIGHTNESS;
        accessory[i].state = (orb_home_state_t){.present = ORB_HOME_ON | ORB_HOME_BRIGHTNESS,
            .on = true, .brightness = 40};
    }
    assert(orb_home_snapshot_chunk_available(room, rooms, accessory, rooms, NULL, 0, true, true, false));
    orb_home_set_connection(true, true);
}

static home_context_t context_of(orb_screen_instance_t *instance)
{
    static unsigned char bytes[HOME_CONTEXT_BYTES];
    orb_screen_context_t *context = (orb_screen_context_t *)bytes;
    assert(orb_screen_save(instance, context, sizeof(bytes)));
    home_context_t value;
    memcpy(&value, context->payload, sizeof(value));
    return value;
}

static bool intent(void *context, const orb_intent_t *value)
{
    (void)context;
    s_intents++;
    s_last_intent = *value;
    return true;
}

static bool project(void *context, orb_screen_id_t id, orb_screen_update_t *update)
{
    (void)context;
    assert(id == ORB_APP_HOME_SCREEN_ID);
    *update = (orb_screen_update_t){.revisions = {.home = orb_home_revision(), .time = s_time},
        .model = &s_model_view};
    return true;
}

static void quiescent(void)
{
    settle();
    assert(used(ORB_MEMORY_WORKSPACE) == s_workspace_baseline);
    assert(used(ORB_MEMORY_MODEL) == s_model_baseline);
    assert(timer_count() == s_timer_baseline);
}

static int32_t scroll_to(orb_screen_instance_t *instance, int32_t offset)
{
    lv_obj_t *list = orb_app_home_scroll(instance);
    assert(list);
    lv_obj_update_layout(list);
    lv_obj_stop_scroll_anim(list);
    lv_obj_scroll_to_y(list, offset, LV_ANIM_OFF);
    settle();
    return lv_obj_get_scroll_y(list);
}

static void exercise(orb_screen_instance_t *instance)
{
    /* The landing list scrolls without a value lease or a Home command. */
    unsigned before = s_intents;
    float value, step;
    assert(!orb_app_home_dial(instance, &value, &step));
    assert(!orb_app_home_dial_writes(instance));
    lv_obj_t *list = orb_app_home_scroll(instance);
    assert(list);
    s_scroll = scroll_to(instance, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list));
    assert(s_scroll > 0 && s_intents == before);
    assert(orb_screen_back(instance) == ORB_BACK_LEAVE_SCREEN);
}

static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    assert(orb_screen_alive(instance));
    assert(!lv_obj_is_clickable(orb_screen_root(instance)));
    assert(lv_obj_is_event_bubble(orb_screen_root(instance)));
    orb_screen_chrome_t chrome;
    orb_screen_chrome(instance, &chrome);
    assert(!strcmp(chrome.title, "Home"));
    settle();
    home_context_t context = context_of(instance);
    assert(context.surface == ORB_HOME_ROOMS && !context.selected[0]);
    switch (phase) {
    case ORB_LIFECYCLE_INITIAL:
    case ORB_LIFECYCLE_IDEMPOTENT:
        assert(context.rooms_scroll == 0);
        assert(lv_obj_get_scroll_y(orb_app_home_scroll(instance)) > 0);
        float value, step;
        assert(!orb_app_home_dial(instance, &value, &step));
        /* The view polls only on the host's next time revision. */
        if (phase == ORB_LIFECYCLE_IDEMPOTENT) publish(8);
        break;
    case ORB_LIFECYCLE_CHANGED:
        assert(orb_app_home_scroll(instance));
        break;
    case ORB_LIFECYCLE_RESTORED:
        assert(context.rooms_scroll == s_scroll);
        publish(6);
        break;
    case ORB_LIFECYCLE_REMOVED_ID:
        /* Removing rooms clamps the saved offset into the shorter list. */
        assert(context.rooms_scroll >= 0 && context.rooms_scroll < s_scroll);
        publish(0);
        break;
    case ORB_LIFECYCLE_EMPTY:
        assert(!orb_app_home_dial_writes(instance));
        break;
    }
}

static void lifecycle(orb_screen_host_t *host)
{
    publish(7);
    orb_screen_update_t initial = {.revisions = {.home = orb_home_revision(), .time = ++s_time}, .model = &s_model_view};
    orb_screen_update_t changed = {.revisions = {.home = orb_home_revision() + 1, .time = ++s_time}, .model = &s_model_view};
    orb_screen_update_t removed = {.revisions = {.time = ++s_time}, .model = &s_model_view};
    orb_screen_update_t empty = {.revisions = {.time = ++s_time}};
    orb_lifecycle_case_t test = {.descriptor = orb_app_home_descriptor(), .initial = &initial,
        .changed = &changed, .removed = &removed, .empty = &empty,
        .exercise = exercise, .verify = verify, .quiescent = quiescent};
    orb_test_screen_lifecycle(host, &test);
}

static void preview(orb_screen_host_t *host)
{
    publish(2);
    orb_screen_instance_t instance = {0};
    assert(orb_screen_create(&instance, host, orb_app_home_descriptor(), lv_screen_active(), NULL, true));
    /* A read-only preview cannot present a local iris or take the rim. */
    unsigned before = s_intents;
    orb_screen_update_t update = {.revisions.time = ++s_time, .model = &s_model_view};
    assert(orb_screen_update(&instance, &update));
    assert(orb_screen_back(&instance) == ORB_BACK_CONSUMED);
    assert(!orb_screen_rim_acquire(&instance));
    assert(s_intents == before);
    orb_screen_destroy(&instance);
    quiescent();
}


static void restore_selected(orb_screen_instance_t *instance, const char *room, const char *accessory)
{
    home_context_t value = context_of(instance);
    value.surface = ORB_HOME_ROOM;
    snprintf(value.room, sizeof(value.room), "%s", room);
    snprintf(value.selected, sizeof(value.selected), "%s", accessory);
    unsigned char bytes[HOME_CONTEXT_BYTES] = {0};
    orb_screen_context_t *context = (orb_screen_context_t *)bytes;
    context->version = orb_app_home_descriptor()->context_version;
    context->size = HOME_CONTEXT_BYTES;
    memcpy(context->payload, &value, sizeof(value));
    assert(orb_screen_restore(instance, context, sizeof(bytes)));
    settle();
}

/* The dial's write leaves the presenter as a typed, value-only level intent
 * carrying the stable alias, one characteristic and its committed value. */
static void typed_writes(orb_screen_host_t *host)
{
    publish(2);
    orb_screen_instance_t instance = {0};
    assert(orb_screen_create(&instance, host, orb_app_home_descriptor(), lv_screen_active(), NULL, false));
    orb_screen_update_t update = {.revisions.time = ++s_time, .model = &s_model_view};
    assert(orb_screen_update(&instance, &update));
    restore_selected(&instance, "living", "living-lamp");
    float value, step;
    assert(orb_app_home_dial(&instance, &value, &step) && orb_app_home_dial_writes(&instance));
    unsigned before = s_intents;
    orb_app_home_rotate(&instance, .8f);
    assert(s_intents == before);
    orb_app_home_rotary_finish(&instance, true);
    assert(s_intents == before + 1);
    assert(s_last_intent.kind == ORB_INTENT_HOME_SET_LEVEL && s_last_intent.origin == ORB_INPUT_TOUCH);
    assert(s_last_intent.physical && !strcmp(s_last_intent.value.home.id, "living-lamp"));
    assert(s_last_intent.value.home.action == ORB_HOME_COMMAND_LEVEL);
    assert(s_last_intent.value.home.field == ORB_HOME_BRIGHTNESS && s_last_intent.value.home.value == 80);
    orb_home_command_t command;
    assert(orb_app_home_command_from_intent(&s_last_intent, &command));
    assert(command.kind == ORB_HOME_COMMAND_LEVEL && !strcmp(command.id, "living-lamp"));
    assert(command.values.present == ORB_HOME_BRIGHTNESS && command.values.brightness == 80);
    /* Saving and restoring the same context never replays the write. */
    orb_screen_destroy(&instance);
    assert(s_intents == before + 1);
    settle();
    quiescent();
}

static void intent_codec(void)
{
    static const struct { uint32_t field; float value; } fields[] = {
        {ORB_HOME_ON, 1}, {ORB_HOME_BRIGHTNESS, 37}, {ORB_HOME_HUE, 210}, {ORB_HOME_SAT, 44},
        {ORB_HOME_CT, 320}, {ORB_HOME_TARGET, 21.5f}, {ORB_HOME_MODE, ORB_HOME_MODE_COOL},
        {ORB_HOME_LOCKED, 0}, {ORB_HOME_SPEED, 60}, {ORB_HOME_POSITION, 25},
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        orb_intent_t intent = {.kind = ORB_INTENT_HOME_COMMAND};
        snprintf(intent.value.home.id, sizeof(intent.value.home.id), "door");
        intent.value.home.action = ORB_HOME_COMMAND_SET;
        intent.value.home.field = fields[i].field;
        intent.value.home.value = fields[i].value;
        intent.value.home.deliberate = fields[i].field == ORB_HOME_LOCKED;
        orb_home_command_t command;
        assert(orb_app_home_command_from_intent(&intent, &command));
        assert(command.values.present == fields[i].field && command.deliberate_unlock == (fields[i].field == ORB_HOME_LOCKED));
        float round = -1;
        switch (fields[i].field) {
        case ORB_HOME_ON: round = command.values.on; break;
        case ORB_HOME_BRIGHTNESS: round = command.values.brightness; break;
        case ORB_HOME_HUE: round = command.values.hue; break;
        case ORB_HOME_SAT: round = command.values.sat; break;
        case ORB_HOME_CT: round = command.values.ct; break;
        case ORB_HOME_TARGET: round = command.values.target; break;
        case ORB_HOME_MODE: round = (float)command.values.mode; break;
        case ORB_HOME_LOCKED: round = command.values.locked; break;
        case ORB_HOME_SPEED: round = command.values.speed; break;
        case ORB_HOME_POSITION: round = command.values.position; break;
        }
        assert(round == fields[i].value);
    }
    orb_intent_t scene = {.kind = ORB_INTENT_HOME_COMMAND};
    snprintf(scene.value.home.id, sizeof(scene.value.home.id), "evening");
    scene.value.home.action = ORB_HOME_COMMAND_SCENE;
    orb_home_command_t command;
    assert(orb_app_home_command_from_intent(&scene, &command) && command.kind == ORB_HOME_COMMAND_SCENE);
    /* A level kind must carry the level action, two fields are not one value,
     * an unterminated alias is malformed and other intents are not Home's. */
    scene.kind = ORB_INTENT_HOME_SET_LEVEL;
    assert(!orb_app_home_command_from_intent(&scene, &command));
    orb_intent_t two = {.kind = ORB_INTENT_HOME_COMMAND};
    snprintf(two.value.home.id, sizeof(two.value.home.id), "lamp");
    two.value.home.action = ORB_HOME_COMMAND_SET;
    two.value.home.field = ORB_HOME_ON | ORB_HOME_BRIGHTNESS;
    assert(!orb_app_home_command_from_intent(&two, &command));
    memset(two.value.home.id, 'x', sizeof(two.value.home.id));
    two.value.home.field = ORB_HOME_ON;
    assert(!orb_app_home_command_from_intent(&two, &command));
    orb_intent_t other = {.kind = ORB_INTENT_SETTING_SET};
    assert(!orb_app_home_command_from_intent(&other, &command));
}

static void host_transition(void *context, const lv_area_t *token, bool reverse, bool before)
{
    (void)context;
    s_transitions++;
    s_transition_area = *token;
    s_transition_reverse = reverse;
    s_transition_prepare = before;
}

static lv_obj_t *page(void)
{
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(parent);
    lv_obj_set_size(parent, 466, 466);
    return parent;
}

static void compatibility_host(void)
{
    publish(8);
    orb_nav_t nav;
    orb_nav_init(&nav, ORB_APP_HOME_SCREEN_ID);
    orb_app_host_t *host = orb_home_host_create(false, &(orb_home_host_shell_t){
        .transition = host_transition, .nav = &nav});
    assert(host);
    lv_obj_t *first = page();
    orb_app_home_model_t model = {.hour = 19, .system_navigation = true};
    assert(orb_home_host_attach(host, first, &model));
    orb_screen_instance_t *home = orb_app_host_current(host);
    assert(home && lv_obj_get_parent(orb_screen_root(home)) == first);
    int32_t saved_scroll = scroll_to(home, 144);
    assert(saved_scroll == 144);

    /* A preview clone holds a deep value copy; it never aliases the source. */
    orb_app_host_t *clone = NULL;
    assert(orb_app_host_clone(&clone, host, true));
    lv_obj_t *preview_page = page();
    assert(orb_home_host_attach(clone, preview_page, &model));
    assert(context_of(orb_app_host_current(clone)).rooms_scroll == saved_scroll);
    assert(scroll_to(orb_app_host_current(clone), 0) == 0);
    assert(context_of(orb_app_host_current(clone)).rooms_scroll == -1);
    assert(context_of(home).rooms_scroll == saved_scroll);
    orb_app_host_detach_page(clone, preview_page);
    assert(!orb_app_host_current(clone));
    lv_obj_delete(preview_page);
    orb_app_host_destroy(clone);

    /* Leaving through page deletion saves; the next page restores. */
    orb_app_host_detach_page(host, first);
    assert(!orb_app_host_current(host));
    lv_obj_delete(first);
    lv_obj_t *second = page();
    assert(orb_home_host_attach(host, second, &model));
    home = orb_app_host_current(host);
    assert(context_of(home).rooms_scroll == saved_scroll);

    /* A restored room context returns through the host's presentation with
     * a reverse prepare request carrying the exact saved token. */
    home_context_t room = context_of(home);
    room.surface = ORB_HOME_ROOM;
    snprintf(room.room, sizeof(room.room), "office");
    room.room_token[0] = 100; room.room_token[1] = 120; room.room_token[2] = 179; room.room_token[3] = 199;
    unsigned char bytes[HOME_CONTEXT_BYTES] = {0};
    orb_screen_context_t *context = (orb_screen_context_t *)bytes;
    context->version = orb_app_home_descriptor()->context_version;
    context->size = HOME_CONTEXT_BYTES;
    memcpy(context->payload, &room, sizeof(room));
    assert(orb_screen_restore(home, context, sizeof(bytes)));
    settle();
    assert(orb_app_home_can_back(home));
    unsigned before = s_transitions;
    assert(orb_screen_back(home) == ORB_BACK_CONSUMED);
    assert(s_transitions == before + 1 && s_transition_reverse && s_transition_prepare);
    assert(s_transition_area.x1 == 100 && s_transition_area.y1 == 120 &&
        s_transition_area.x2 == 179 && s_transition_area.y2 == 199);
    settle();
    assert(s_transitions == before + 2 && !s_transition_prepare);
    assert(context_of(home).surface == ORB_HOME_ROOMS);

    /* Benchmark-style restore moves values back without a replay. */
    orb_app_host_t *saved = NULL;
    assert(orb_app_host_clone(&saved, host, true));
    assert(scroll_to(home, 0) == 0);
    orb_app_host_detach_page(host, second);
    lv_obj_delete(second);
    assert(orb_app_host_restore(&host, &saved) && !saved);
    lv_obj_t *third = page();
    assert(orb_home_host_attach(host, third, &model));
    assert(context_of(orb_app_host_current(host)).rooms_scroll == saved_scroll);

    /* The landing list leaves the rim to the shell's native scroll path. */
    float value, step;
    assert(!orb_app_host_rim(host));
    assert(!orb_app_home_dial(orb_app_host_current(host), &value, &step));
    assert(!orb_app_home_dial_writes(orb_app_host_current(host)));
    assert(!orb_home_take_request(&(orb_home_request_t){0}));

    /* A live host executes the typed write through the unchanged queue. */
    restore_selected(orb_app_host_current(host), "office", "office-lamp");
    orb_app_home_rotate(orb_app_host_current(host), .25f);
    orb_app_home_rotary_finish(orb_app_host_current(host), true);
    orb_home_request_t request, none;
    assert(orb_home_take_request(&request) && request.kind == ORB_HOME_REQUEST_SET &&
        !strcmp(request.id, "office-lamp") && request.values.present == ORB_HOME_BRIGHTNESS &&
        request.values.brightness == 25);
    assert(!orb_home_take_request(&none));
    orb_home_complete(&request, &request.values, NULL);
    orb_app_host_poll(host);
    settle();

    /* A leased twist on the accessory dial previews then commits one write
     * with physical case provenance. */
    assert(orb_app_host_rim(host) && nav.rotary.owner.detent == .01f);
    const void *owner = nav.rotary.owner.ctx;
    assert(owner && owner != host);
    /* A rim contact that ends while Home is covered (a sheet or transition
     * arrived mid-drag) only drops the dial preview: the framework refuses the
     * lease finish and the presenter must not write the previewed level. */
    orb_screen_instance_t *covered = orb_app_host_current(host);
    assert(orb_screen_rim_begin(covered, ORB_INPUT_CROWN, true));
    orb_app_home_rotate(covered, .6f);
    orb_screen_visibility(covered, ORB_VISIBILITY_COVERED);
    orb_app_host_rim_finish(host, true);
    assert(!orb_home_take_request(&none));
    orb_screen_visibility(covered, ORB_VISIBILITY_ACTIVE);
    orb_app_host_rim_finish(host, false); /* the drag ends uncommitted */
    assert(!orb_home_take_request(&none));
    settle();
    assert(orb_app_host_rim(host) && nav.rotary.owner.detent == .01f);
    bool changed = false;
    assert(orb_app_host_twist(host, 5, &changed) && changed);
    assert(orb_home_take_request(&request) && !strcmp(request.id, "office-lamp") &&
        request.values.present == ORB_HOME_BRIGHTNESS && request.values.brightness == 30);
    assert(!orb_home_take_request(&none));
    orb_home_complete(&request, &request.values, NULL);
    settle();
    /* A finger on the glass leaves the twist to the shell's own step, which
     * refuses it, and no write is queued. */
    nav.touch.down = true;
    assert(orb_app_host_twist(host, 1, &changed) && !changed);
    nav.touch.down = false;
    assert(!orb_home_take_request(&none));

    /* Parent deletion without an explicit detach still releases the screen
     * and its lease; the shared binding waits for the shell's next rebind. */
    lv_obj_delete(third);
    assert(!orb_app_host_current(host));
    assert(nav.rotary.bound && nav.rotary.owner.ctx == owner);
    orb_app_host_destroy(host);
    quiescent();
}

int main(void)
{
    lv_init();
    assert(sim_display_create());
    orb_ui_init(NULL);
    assert(orb_home_init());
    orb_screen_registry_t registry;
    orb_screen_registry_init(&registry);
    const char *reason = NULL;
    assert(orb_screen_registry_add(&registry, orb_app_home_manifest(), &reason));
    assert(orb_screen_registry_find(&registry, ORB_APP_HOME_SCREEN_ID) == orb_app_home_descriptor());
    assert(orb_app_home_descriptor()->context_bytes == HOME_CONTEXT_BYTES);
    orb_nav_t nav;
    orb_nav_init(&nav, ORB_APP_HOME_SCREEN_ID);
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.nav = &nav, .intent = intent, .project = project});
    /* Warm fonts, kit caches and the store before taking baselines. */
    publish(2);
    orb_screen_instance_t warm = {0};
    assert(orb_screen_create(&warm, &host, orb_app_home_descriptor(), lv_screen_active(), NULL, false));
    orb_screen_destroy(&warm);
    settle();
    s_timer_baseline = timer_count();
    s_workspace_baseline = used(ORB_MEMORY_WORKSPACE);
    s_model_baseline = used(ORB_MEMORY_MODEL);

    intent_codec();
    lifecycle(&host);
    preview(&host);
    typed_writes(&host);
    compatibility_host();
    printf("app_home: lifecycle, preview and compatibility host passed\n");
    return 0;
}
