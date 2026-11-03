#include "orb_settings.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned s_checks;
#define CHECK(value) do { ++s_checks; assert(value); } while (0)

static void validation(const orb_faces_prefs_t *defaults)
{
    CHECK(sizeof(*defaults) == 16);
    CHECK(defaults->version == 2 && defaults->face == ORB_FACE_ORB && defaults->colour == 0);
    CHECK(defaults->slot[0] == ORB_FACE_CLAUDE && defaults->slot[1] == ORB_FACE_HOME &&
        defaults->slot[2] == ORB_FACE_BATTERY);
    CHECK(defaults->meridian_slot[0] == ORB_FACE_WEATHER && defaults->meridian_slot[1] == ORB_FACE_CLAUDE &&
        defaults->meridian_slot[2] == ORB_FACE_BATTERY && defaults->meridian_slot[3] == ORB_FACE_DATE);
    CHECK(defaults->activity_slot[0] == ORB_FACE_CLAUDE && defaults->activity_slot[1] == ORB_FACE_CODEX &&
        defaults->activity_slot[2] == ORB_FACE_HOME);
    CHECK(orb_settings_faces_valid(defaults) && !orb_settings_faces_valid(NULL));
    orb_faces_prefs_t bad = *defaults;
    bad.version = 1; CHECK(!orb_settings_faces_valid(&bad));
    bad = *defaults; bad.face = ORB_FACE_COUNT; CHECK(!orb_settings_faces_valid(&bad));
    bad = *defaults; bad.colour = 9; CHECK(!orb_settings_faces_valid(&bad));
    for (unsigned i = 0; i < 3; i++) {
        bad = *defaults; bad.slot[i] = ORB_FACE_PROVIDER_COUNT; CHECK(!orb_settings_faces_valid(&bad));
        bad = *defaults; bad.activity_slot[i] = ORB_FACE_PROVIDER_COUNT; CHECK(!orb_settings_faces_valid(&bad));
    }
    for (unsigned i = 0; i < 4; i++) {
        bad = *defaults; bad.meridian_slot[i] = ORB_FACE_PROVIDER_COUNT; CHECK(!orb_settings_faces_valid(&bad));
    }
    bad = *defaults; memset(bad.reserved, 0xa5, sizeof(bad.reserved));
    CHECK(orb_settings_faces_valid(&bad));
}

int main(void)
{
    orb_faces_prefs_t defaults, current;
    orb_settings_faces_current(&defaults); /* Standalone presenters need no init. */
    validation(&defaults);
    orb_settings_init();
    orb_settings_snapshot_t state;
    orb_settings_snapshot(&state);
    CHECK(state.revision && !state.saving && !state.save_error);
    uint64_t revision = state.revision;

    orb_faces_prefs_t candidate = defaults;
    candidate.face = ORB_FACE_ACTIVITY;
    CHECK(orb_settings_faces_accept(&candidate));
    orb_settings_faces_current(&current);
    CHECK(!memcmp(&current, &candidate, sizeof(current)));
    orb_settings_snapshot(&state);
    CHECK(state.revision > revision && !state.saving && !state.save_error);
    CHECK(!orb_settings_faces_accept(NULL));

    candidate.colour = 3;
    orb_faces_prefs_t expected = candidate;
    uint64_t ticket = orb_settings_faces_request(&candidate);
    CHECK(ticket && orb_settings_faces_result(ticket) == ORB_SETTINGS_SAVED);
    candidate.colour = 4;
    orb_settings_faces_current(&current);
    CHECK(!memcmp(&current, &expected, sizeof(current))); /* Copy, not caller storage. */
    orb_settings_snapshot(&state);
    CHECK(!state.saving && !state.save_error);
    revision = state.revision;
    orb_settings_service();
    orb_settings_snapshot(&state);
    CHECK(state.revision == revision); /* Host pump adds no timing or I/O. */
    orb_settings_faces_release(ticket);
    CHECK(orb_settings_faces_result(ticket) == ORB_SETTINGS_UNKNOWN);
    CHECK(orb_settings_faces_result(0) == ORB_SETTINGS_UNKNOWN);
    orb_settings_faces_release(0);

    uint64_t tickets[ORB_SETTINGS_FACE_REQUESTS];
    for (unsigned i = 0; i < ORB_SETTINGS_FACE_REQUESTS; i++) {
        candidate.colour = (uint8_t)i;
        tickets[i] = orb_settings_faces_request(&candidate);
        CHECK(tickets[i] && orb_settings_faces_result(tickets[i]) == ORB_SETTINGS_SAVED);
    }
    CHECK(!orb_settings_faces_request(&candidate));
    orb_settings_snapshot(&state);
    CHECK(!state.saving && state.save_error);
    for (unsigned i = 0; i < ORB_SETTINGS_FACE_REQUESTS; i++) {
        CHECK(orb_settings_faces_result(tickets[i]) == ORB_SETTINGS_SAVED);
        orb_settings_faces_release(tickets[i]);
    }
    ticket = orb_settings_faces_request(&candidate);
    CHECK(ticket && orb_settings_faces_result(ticket) == ORB_SETTINGS_SAVED);
    orb_settings_snapshot(&state);
    CHECK(!state.saving && !state.save_error);
    orb_settings_faces_release(ticket);
    CHECK(!orb_settings_faces_request(NULL));
    orb_settings_snapshot(&state);
    CHECK(state.save_error);
    CHECK(orb_settings_faces_accept(&defaults));
    orb_settings_snapshot(&state);
    CHECK(!state.save_error);
    orb_settings_faces_current(NULL);
    orb_settings_snapshot(NULL);
    printf("settings memory: %u checks passed\n", s_checks);
    return 0;
}
