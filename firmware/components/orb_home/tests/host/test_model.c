/* Protocol invariants independent of JSON parsing or simulator-only fixtures. */
#include "orb_home.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_checks;
static orb_home_snapshot_t s_view;
static orb_home_room_t s_room = {.id = "r0", .name = "Room"};
static orb_home_accessory_t s_accessory = {
    .id = "a0", .name = "Light", .room = "r0", .kind = ORB_HOME_LIGHT,
    .reachable = true, .favourite = true, .writable = ORB_HOME_WRITABLE,
    .state = {.present = ORB_HOME_ON | ORB_HOME_BRIGHTNESS, .on = true, .brightness = 40},
};
static orb_home_scene_t s_scene = {.id = "s0", .name = "Scene", .favourite = true};
#define CHECK(c) do { s_checks++; if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)

static void refresh(void) { CHECK(orb_home_snapshot(&s_view)); }
static void fixture(void)
{
    orb_home_reset();
    orb_home_set_connection(true, false);
    CHECK(orb_home_snapshot_chunk_available(&s_room, 1, &s_accessory, 1, &s_scene, 1, true, true, false));
    refresh(); CHECK(s_view.online && s_view.synced && s_view.available);
}
static void availability(void)
{
    fixture();
    CHECK(orb_home_toggle("a0"));
    CHECK(orb_home_snapshot_chunk_available(&s_room, 1, NULL, 0, NULL, 0, true, true, true));
    refresh();
    CHECK(!s_view.synced && s_view.accessory_count == 1 && !s_view.accessories[0].pending && s_view.accessories[0].state.on);
    CHECK(!orb_home_toggle("a0") && !orb_home_scene("s0"));
    CHECK(!orb_home_take_request(&(orb_home_request_t){0}));
    CHECK(!orb_home_snapshot_chunk_available(NULL, 0, &s_accessory, 1, NULL, 0, false, false, false));
    refresh(); CHECK(s_view.accessory_count == 1 && !s_view.synced);
    CHECK(!orb_home_snapshot_chunk_available(NULL, 0, &s_accessory, 1, NULL, 0, true, false, false));
    orb_home_accessory_t offline = s_accessory; offline.reachable = false;
    CHECK(orb_home_snapshot_chunk_available(&s_room, 1, &offline, 1, &s_scene, 1, false, true, false));
    refresh(); CHECK(s_view.online && !s_view.available && s_view.synced && s_view.accessory_count == 1);
    CHECK(!orb_home_toggle("a0") && !orb_home_scene("s0"));
    CHECK(!orb_home_update("a0", &(orb_home_state_t){.present = ORB_HOME_ON}, false, false, 0));
    CHECK(orb_home_snapshot_chunk_available(&s_room, 1, &s_accessory, 1, &s_scene, 1, true, true, false));
    CHECK(orb_home_toggle("a0"));
    orb_home_snapshot_discard(); refresh();
    CHECK(!s_view.synced && !s_view.accessories[0].pending && s_view.accessories[0].state.on);
    CHECK(!orb_home_take_request(&(orb_home_request_t){0}));
    CHECK(orb_home_snapshot_chunk_available(&s_room, 1, NULL, 0, NULL, 0, true, true, true));
    orb_home_set_connection(false, false);
    orb_home_set_connection(true, false);
    CHECK(!orb_home_snapshot_chunk_available(NULL, 0, &s_accessory, 1, NULL, 0, true, false, false));
    CHECK(orb_home_snapshot_chunk_available(NULL, 0, NULL, 0, NULL, 0, false, true, false));
    refresh(); CHECK(!s_view.accessory_count && !s_view.room_count && !s_view.scene_count && !s_view.available);
}
static void bounds(void)
{
    fixture();
    orb_home_accessory_t accessory = s_accessory;
    strcpy(accessory.id, "a1234567890123456");
    CHECK(!orb_home_snapshot_chunk(&s_room, 1, &accessory, 1, NULL, 0, true, false));
    strcpy(accessory.id, "a123456789012345");
    CHECK(orb_home_snapshot_chunk(&s_room, 1, &accessory, 1, NULL, 0, true, false));
    strcpy(accessory.id, "a\xc3\xa9");
    CHECK(!orb_home_snapshot_chunk(&s_room, 1, &accessory, 1, NULL, 0, true, false));
    accessory = s_accessory; accessory.writable |= ORB_HOME_TEMP;
    CHECK(!orb_home_snapshot_chunk(&s_room, 1, &accessory, 1, NULL, 0, true, false));
    accessory = s_accessory; strcpy(accessory.room, "missing");
    CHECK(!orb_home_snapshot_chunk(&s_room, 1, &accessory, 1, NULL, 0, true, false));
    CHECK(!orb_home_snapshot_chunk(NULL, 0, &s_accessory, 1, NULL, 0, true, false));
    accessory = s_accessory; accessory.name[0] = 0;
    CHECK(orb_home_snapshot_chunk(&s_room, 1, &accessory, 1, NULL, 0, true, false));
    for (orb_home_kind_t kind = ORB_HOME_DOORBELL; kind <= ORB_HOME_OTHER; kind++) {
        accessory.kind = kind; CHECK(orb_home_snapshot_chunk(&s_room, 1, &accessory, 1, NULL, 0, true, false));
    }
    orb_home_scene_t scenes[ORB_HOME_MAX_SCENES + 1] = {0};
    for (size_t i = 0; i <= ORB_HOME_MAX_SCENES; i++) snprintf(scenes[i].id, sizeof(scenes[i].id), "s%zu", i);
    CHECK(orb_home_snapshot_chunk(NULL, 0, NULL, 0, scenes, ORB_HOME_MAX_SCENES, true, true));
    CHECK(!orb_home_snapshot_chunk(NULL, 0, NULL, 0, scenes + ORB_HOME_MAX_SCENES, 1, false, false));
    CHECK(orb_home_snapshot_chunk(NULL, 0, NULL, 0, scenes, ORB_HOME_MAX_SCENES, true, false));
    refresh(); CHECK(s_view.scene_count == 32);
    orb_home_room_t rooms[ORB_HOME_MAX_ROOMS + 1] = {0};
    for (size_t i = 0; i <= ORB_HOME_MAX_ROOMS; i++) snprintf(rooms[i].id, sizeof(rooms[i].id), "r%zu", i);
    CHECK(orb_home_snapshot_chunk(rooms, ORB_HOME_MAX_ROOMS, NULL, 0, NULL, 0, true, true));
    CHECK(!orb_home_snapshot_chunk(rooms + ORB_HOME_MAX_ROOMS, 1, NULL, 0, NULL, 0, false, false));
    CHECK(orb_home_snapshot_chunk(rooms, ORB_HOME_MAX_ROOMS, NULL, 0, NULL, 0, true, false));
    refresh(); CHECK(s_view.room_count == 16);
}
static void invalid_chunk_discards(void)
{
    /* Every rejected continuation poisons the replacement, even if it fails
     * the cheap count/pointer checks before per-object validation. */
    for (unsigned invalid = 0; invalid < 6; invalid++) {
        fixture();
        CHECK(orb_home_snapshot_chunk(&s_room, 1, NULL, 0, NULL, 0, true, true));
        CHECK(!orb_home_snapshot_chunk(
            invalid == 0 ? &s_room : NULL, invalid == 0 ? ORB_HOME_MAX_ROOMS + 1 : invalid == 3,
            invalid == 1 ? &s_accessory : NULL, invalid == 1 ? ORB_HOME_MAX_ACCESSORIES + 1 : invalid == 4,
            invalid == 2 ? &s_scene : NULL, invalid == 2 ? ORB_HOME_MAX_SCENES + 1 : invalid == 5,
            false, true));
        CHECK(!orb_home_snapshot_chunk(NULL, 0, &s_accessory, 1, NULL, 0, false, false));
        refresh(); CHECK(!s_view.synced && s_view.accessory_count == 1);
        CHECK(!orb_home_toggle("a0") && !orb_home_scene("s0"));
    }
    /* A malformed first chunk also retires current actuator intents. */
    fixture(); CHECK(orb_home_toggle("a0"));
    CHECK(!orb_home_snapshot_chunk(NULL, 1, NULL, 0, NULL, 0, true, false));
    refresh(); CHECK(!s_view.synced && !s_view.accessories[0].pending && s_view.accessories[0].state.on);
    CHECK(!orb_home_take_request(&(orb_home_request_t){0}));
    fixture();
}
static void intent_generation(void)
{
    fixture(); uint64_t generation = s_view.generation;
    CHECK(orb_home_update("a0", &(orb_home_state_t){.present = ORB_HOME_BRIGHTNESS, .brightness = 30}, false, false, 0));
    refresh(); CHECK(s_view.generation == generation);
    CHECK(orb_home_toggle("a0"));
    orb_home_request_t request; CHECK(orb_home_take_request(&request));
    orb_home_complete(&request, &request.values, NULL);
    refresh(); CHECK(s_view.generation == generation);
    CHECK(orb_home_snapshot_chunk(&s_room, 1, &s_accessory, 1, &s_scene, 1, true, false));
    refresh(); CHECK(s_view.generation != generation); generation = s_view.generation;
    /* Neither the transient offline state nor snapshot staging need be
     * observed by a poll for a previously captured gesture to expire. */
    orb_home_set_connection(false, false); orb_home_set_connection(true, false);
    CHECK(orb_home_snapshot_chunk(&s_room, 1, &s_accessory, 1, &s_scene, 1, true, false));
    refresh(); CHECK(s_view.generation != generation); generation = s_view.generation;
    orb_home_snapshot_discard();
    refresh(); CHECK(s_view.generation != generation); generation = s_view.generation;
    orb_home_reset(); refresh(); CHECK(s_view.generation != generation);
    fixture();
}
static void full_queue_feedback(void)
{
    for (unsigned action = 0; action < 3; action++) {
        fixture();
        orb_home_accessory_t accessories[6];
        for (unsigned i = 0; i < 6; i++) {
            accessories[i] = s_accessory;
            snprintf(accessories[i].id, sizeof(accessories[i].id), "a%u", i);
        }
        accessories[5].writable = 0;
        orb_home_scene_t scenes[] = {s_scene, {.id = "unsafe", .contains_unlock = true}};
        CHECK(orb_home_snapshot_chunk(&s_room, 1, accessories, 6, scenes, 2, true, false));
        for (unsigned i = 0; i < ORB_HOME_MAX_PENDING; i++) CHECK(orb_home_toggle(accessories[i].id));
        refresh(); uint64_t revision = s_view.revision;
        CHECK(!orb_home_toggle("missing") && !orb_home_toggle("a5"));
        CHECK(!orb_home_set("a4", &(orb_home_state_t){.present = ORB_HOME_BRIGHTNESS, .brightness = NAN}, false));
        CHECK(!orb_home_set("a4", &(orb_home_state_t){.present = ORB_HOME_LOCKED, .locked = false}, false));
        CHECK(!orb_home_scene("missing") && !orb_home_scene("unsafe"));
        refresh(); CHECK(!s_view.error[0] && s_view.revision == revision);
        if (action == 0) CHECK(!orb_home_toggle("a4"));
        else if (action == 1) CHECK(!orb_home_set("a4", &(orb_home_state_t){.present = ORB_HOME_BRIGHTNESS, .brightness = 60}, false));
        else CHECK(!orb_home_scene("s0"));
        refresh(); CHECK(!strcmp(s_view.error, "Home is busy"));
        CHECK(!strcmp(s_view.error_id, action == 2 ? "s0" : "a4") && s_view.revision != revision);
        CHECK(!s_view.accessories[4].pending && s_view.accessories[4].state.on && !s_view.scene_pending);
    }
    fixture();
}
static void ranges_and_replies(void)
{
    fixture();
    orb_home_request_t request;
    const struct { uint32_t bit; size_t offset; float low, high; } fields[] = {
        {ORB_HOME_CT, offsetof(orb_home_state_t, ct), 50, 1000},
        {ORB_HOME_TARGET, offsetof(orb_home_state_t, target), -100, 100},
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        orb_home_state_t state = {.present = fields[i].bit};
        float *value = (float *)((char *)&state + fields[i].offset);
        const float accepted[] = {fields[i].low, fields[i].high};
        for (size_t j = 0; j < 2; j++) {
            *value = accepted[j]; CHECK(orb_home_set("a0", &state, false));
            CHECK(orb_home_take_request(&request)); orb_home_complete(&request, &state, NULL);
        }
        const float rejected[] = {fields[i].low - 1, fields[i].high + 1, NAN, INFINITY};
        for (size_t j = 0; j < 4; j++) { *value = rejected[j]; CHECK(!orb_home_set("a0", &state, false)); }
    }
    orb_home_state_t temperature = {.present = ORB_HOME_TEMP, .temp = -100};
    CHECK(orb_home_update("a0", &temperature, false, false, 0));
    temperature.temp = 100; CHECK(orb_home_update("a0", &temperature, false, false, 0));
    temperature.temp = 101; CHECK(!orb_home_update("a0", &temperature, false, false, 0));
    CHECK(!orb_home_set("a0", &(orb_home_state_t){.present = ORB_HOME_TEMP, .temp = 22}, false));
    CHECK(orb_home_update("a0", &(orb_home_state_t){.present = ORB_HOME_ON, .on = true}, false, false, 0));
    CHECK(orb_home_toggle("a0")); CHECK(orb_home_take_request(&request));
    orb_home_complete(&request, &(orb_home_state_t){0}, NULL);
    refresh(); CHECK(!s_view.accessories[0].state.present && !s_view.accessories[0].pending);
    CHECK(orb_home_update("a0", &(orb_home_state_t){.present = ORB_HOME_ON, .on = true}, false, false, 0));
    CHECK(orb_home_toggle("a0")); CHECK(orb_home_take_request(&request));
    orb_home_complete(&request, NULL, NULL);
    refresh(); CHECK(!s_view.accessories[0].state.present && !s_view.accessories[0].pending);
    CHECK(orb_home_update("a0", &(orb_home_state_t){.present = ORB_HOME_ON, .on = true}, false, false, 0));
    CHECK(orb_home_toggle("a0")); CHECK(orb_home_take_request(&request));
    CHECK(orb_home_update("a0", &(orb_home_state_t){.present = ORB_HOME_ON, .on = true}, false, false, 0));
    refresh(); CHECK(s_view.accessories[0].state.on && s_view.accessories[0].pending);
    CHECK(orb_home_update("a0", &(orb_home_state_t){.present = ORB_HOME_BRIGHTNESS, .brightness = 28}, false, false, 0));
    refresh(); CHECK(s_view.accessories[0].state.on && s_view.accessories[0].state.brightness == 28);
    orb_home_complete(&request, &(orb_home_state_t){.present = ORB_HOME_ON, .on = false}, NULL);
    refresh(); CHECK(s_view.accessories[0].state.on && !s_view.accessories[0].pending);
    CHECK(s_view.accessories[0].state.present == (ORB_HOME_ON | ORB_HOME_BRIGHTNESS));
}
static void summary_counts(void)
{
    CHECK(orb_home_on_count(NULL) == 0);
    orb_home_snapshot_t view = {0};
    view.accessory_count = 6;
    view.accessories[0] = (orb_home_accessory_t){.kind = ORB_HOME_LIGHT,
        .state = {.present = ORB_HOME_ON, .on = true}};
    view.accessories[1] = (orb_home_accessory_t){.kind = ORB_HOME_SWITCH,
        .state = {.present = ORB_HOME_ON, .on = true}};
    view.accessories[2] = (orb_home_accessory_t){.kind = ORB_HOME_LIGHT,
        .state = {.on = true}}; /* An absent value is unknown, not on. */
    view.accessories[3] = (orb_home_accessory_t){.kind = ORB_HOME_LIGHT,
        .state = {.present = ORB_HOME_ON}};
    view.accessories[4] = (orb_home_accessory_t){.kind = ORB_HOME_FAN,
        .state = {.present = ORB_HOME_ON, .on = true}};
    view.accessories[5] = (orb_home_accessory_t){.kind = ORB_HOME_THERMOSTAT,
        .state = {.present = ORB_HOME_ON, .on = true}};
    CHECK(!orb_home_summary_accessory(NULL));
    CHECK(orb_home_on_count(&view) == 2);
    view.accessories[1].state.on = false;
    CHECK(orb_home_on_count(&view) == 1);
    view.accessories[0].state.on = false;
    CHECK(orb_home_on_count(&view) == 0);

    fixture();
    CHECK(orb_home_on_count(&s_view) == 1);
    CHECK(orb_home_toggle("a0")); refresh();
    CHECK(orb_home_on_count(&s_view) == 0);
    orb_home_request_t request;
    CHECK(orb_home_take_request(&request));
    orb_home_complete(&request, NULL, "unreachable"); refresh();
    CHECK(orb_home_on_count(&s_view) == 1);
    CHECK(orb_home_update("a0", &(orb_home_state_t){.present = ORB_HOME_ON}, false, false, 0));
    refresh(); CHECK(orb_home_on_count(&s_view) == 0);
}
static void unknown_lock(void)
{
    fixture();
    orb_home_accessory_t accessory = s_accessory;
    accessory.kind = ORB_HOME_LOCK;
    accessory.writable = ORB_HOME_LOCKED;
    accessory.state = (orb_home_state_t){0};
    CHECK(orb_home_snapshot_chunk(&s_room, 1, &accessory, 1, NULL, 0, true, false));
    CHECK(!orb_home_set("a0", &(orb_home_state_t){.present = ORB_HOME_LOCKED, .locked = false}, true));
    CHECK(!orb_home_set("a0", &(orb_home_state_t){.present = ORB_HOME_LOCKED, .locked = true}, false));
    CHECK(!orb_home_take_request(&(orb_home_request_t){0}));
    CHECK(orb_home_update("a0", &(orb_home_state_t){.present = ORB_HOME_LOCKED, .locked = true}, false, false, 0));
    CHECK(!orb_home_set("a0", &(orb_home_state_t){.present = ORB_HOME_LOCKED, .locked = false}, false));
    CHECK(orb_home_set("a0", &(orb_home_state_t){.present = ORB_HOME_LOCKED, .locked = false}, true));
}
static void room_off(void)
{
    fixture();
    orb_home_accessory_t accessories[12] = {0};
    for (size_t i = 0; i < 12; i++) {
        accessories[i] = s_accessory;
        snprintf(accessories[i].id, sizeof(accessories[i].id), "a%zu", i);
        accessories[i].kind = i % 3 == 0 ? ORB_HOME_LIGHT : i % 3 == 1 ? ORB_HOME_SWITCH : ORB_HOME_OUTLET;
        accessories[i].writable = ORB_HOME_ON;
    }
    accessories[8].state.on = false;
    accessories[9].kind = ORB_HOME_FAN;
    accessories[10].reachable = false;
    accessories[11].writable = 0;
    CHECK(orb_home_snapshot_chunk(&s_room, 1, accessories, 12, &s_scene, 1, true, false));
    CHECK(!orb_home_room_off("missing"));
    CHECK(orb_home_room_off("r0")); refresh();
    CHECK(s_view.room_off_pending && !strcmp(s_view.room_off_id, "r0"));
    CHECK(s_view.room_off_total == 10 && s_view.room_off_completed == 2 && s_view.room_off_failed == 2);
    CHECK(!orb_home_room_off("r0") && !orb_home_scene("s0") && !orb_home_toggle("a7"));
    for (size_t i = 0; i < 8; i++) CHECK(s_view.accessories[i].pending);
    CHECK(!s_view.accessories[8].pending && !s_view.accessories[9].pending);
    uint64_t seen = 0;
    orb_home_request_t request;
    unsigned count = 0;
    while (orb_home_take_request(&request)) {
        CHECK(request.kind == ORB_HOME_REQUEST_SET && request.values.present == ORB_HOME_ON && !request.values.on);
        unsigned index = (unsigned)strtoul(request.id + 1, NULL, 10);
        CHECK(index < 8 && !(seen & (UINT64_C(1) << index)));
        CHECK(index == count); /* Refilling a freed slot preserves queue order. */
        seen |= UINT64_C(1) << index;
        orb_home_complete(&request, &request.values, index == 3 ? "conflict" : NULL);
        count++;
    }
    refresh();
    CHECK(count == 8 && seen == 255 && !s_view.room_off_pending);
    CHECK(s_view.room_off_completed == 10 && s_view.room_off_failed == 3);
    CHECK(s_view.accessories[3].state.on && s_view.accessories[9].state.on);
    CHECK(!strcmp(s_view.error_id, "a3")); /* Later successes keep the batch failure. */
    for (size_t i = 0; i < 12; i++) CHECK(!s_view.accessories[i].pending);
    CHECK(orb_home_scene("s0"));
    CHECK(!orb_home_room_off("r0"));
    CHECK(orb_home_take_request(&request)); orb_home_complete(&request, NULL, "conflict"); refresh();
    CHECK(!strcmp(s_view.error, "Couldn’t run Scene"));

    CHECK(orb_home_snapshot_chunk(&s_room, 1, accessories, 12, &s_scene, 1, true, false));
    CHECK(orb_home_room_off("r0") && orb_home_take_request(&request));
    orb_home_set_connection(false, false); refresh();
    CHECK(!s_view.room_off_pending && s_view.room_off_completed == 10 && s_view.room_off_failed == 10);
    for (size_t i = 0; i < 12; i++) CHECK(!s_view.accessories[i].pending);
    CHECK(!orb_home_take_request(&(orb_home_request_t){0}));
    orb_home_complete(&request, &request.values, NULL); refresh();
    CHECK(s_view.accessories[0].state.on && s_view.room_off_failed == 10);

    orb_home_set_connection(true, false);
    CHECK(orb_home_snapshot_chunk(&s_room, 1, accessories, 12, &s_scene, 1, true, false));
    CHECK(orb_home_room_off("r0") && orb_home_take_request(&request));
    CHECK(orb_home_snapshot_chunk(&s_room, 1, &s_accessory, 1, &s_scene, 1, true, false));
    orb_home_complete(&request, &request.values, NULL); refresh();
    CHECK(!s_view.room_off_pending && s_view.accessory_count == 1 && s_view.accessories[0].state.on);
    CHECK(!orb_home_take_request(&(orb_home_request_t){0}));

    /* The final mask bit and byte-sized progress counters support a full room,
     * without a second queue allocation or dispatching more than four writes. */
    orb_home_accessory_t full[ORB_HOME_MAX_ACCESSORIES];
    for (size_t i = 0; i < ORB_HOME_MAX_ACCESSORIES; i++) {
        full[i] = s_accessory; snprintf(full[i].id, sizeof(full[i].id), "a%zu", i);
    }
    CHECK(orb_home_snapshot_chunk(&s_room, 1, full, ORB_HOME_MAX_ACCESSORIES, NULL, 0, true, false));
    CHECK(orb_home_room_off("r0"));
    orb_home_request_t initial[ORB_HOME_MAX_PENDING];
    for (size_t i = 0; i < ORB_HOME_MAX_PENDING; i++) CHECK(orb_home_take_request(&initial[i]));
    CHECK(!orb_home_take_request(&request));
    for (size_t i = 0; i < ORB_HOME_MAX_PENDING; i++) orb_home_complete(&initial[i], &initial[i].values, NULL);
    count = ORB_HOME_MAX_PENDING;
    while (orb_home_take_request(&request)) { orb_home_complete(&request, &request.values, NULL); count++; }
    refresh(); CHECK(count == 64 && s_view.room_off_completed == 64 && !s_view.room_off_failed && !s_view.room_off_pending);
}
int main(void)
{
    CHECK(orb_home_init()); availability(); bounds(); invalid_chunk_discards(); intent_generation(); full_queue_feedback(); ranges_and_replies(); summary_counts(); unknown_lock(); room_off();
    printf("home protocol model: %u checks passed\n", s_checks); return 0;
}
