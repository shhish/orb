#define _DARWIN_C_SOURCE
#include "orb_settings.h"
#include "orb_fs.h"
#include "orb_fs_port.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char s_base[] = "/tmp/orb-settings-XXXXXX";
static pthread_mutex_t s_fs_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned s_checks, s_locks, s_renames;
static bool s_fail_rename, s_enqueue;
static orb_faces_prefs_t s_during;
static uint64_t s_during_ticket;
#define CHECK(value) do { ++s_checks; assert(value); } while (0)

const char *orb_fs_port_base(void) { return s_base; }
esp_err_t orb_fs_port_mount(void) { return ESP_OK; }
void orb_fs_port_lock(void) { ++s_locks; CHECK(!pthread_mutex_lock(&s_fs_lock)); }
void orb_fs_port_unlock(void) { CHECK(!pthread_mutex_unlock(&s_fs_lock)); }
esp_err_t orb_fs_port_usage(size_t *total, size_t *used)
{
    *total = 4u * 1024u * 1024u;
    *used = 0;
    return ESP_OK;
}

int test_rename(const char *source, const char *target)
{
    ++s_renames;
    if (s_enqueue) {
        s_enqueue = false;
        unsigned locks = s_locks;
        s_during_ticket = orb_settings_faces_request(&s_during);
        CHECK(s_during_ticket && orb_settings_faces_result(s_during_ticket) == ORB_SETTINGS_PENDING);
        orb_settings_snapshot_t state;
        orb_settings_snapshot(&state);
        CHECK(state.saving && !state.save_error && locks == s_locks);
        /* Even accidental reentry cannot process a newer write before the
         * claimed older write has finished publishing its result. */
        orb_settings_service();
        CHECK(locks == s_locks);
    }
    if (s_fail_rename) { errno = EIO; return -1; }
    return rename(source, target);
}

static void expect_current(const orb_faces_prefs_t *expected)
{
    orb_faces_prefs_t current;
    orb_settings_faces_current(&current);
    CHECK(!memcmp(&current, expected, sizeof(current)));
}

static void expect_record(const orb_faces_prefs_t *expected)
{
    orb_faces_prefs_t stored;
    size_t size = 0;
    CHECK(orb_fs_read("faces", "preferences", &stored, sizeof(stored), &size) == ESP_OK);
    CHECK(size == sizeof(stored) && !memcmp(&stored, expected, size));
}

static void load_records(const orb_faces_prefs_t *defaults)
{
    orb_faces_prefs_t valid = *defaults;
    valid.face = ORB_FACE_MERIDIAN;
    valid.colour = 8;
    valid.reserved[0] = 0x5a;
    CHECK(orb_fs_write("faces", "preferences", &valid, sizeof(valid)) == ESP_OK);
    orb_settings_init();
    expect_current(&valid);
    orb_settings_snapshot_t state;
    orb_settings_snapshot(&state);
    CHECK(!state.saving && !state.save_error);

    orb_faces_prefs_t bad = valid;
    bad.version = 1;
    CHECK(orb_fs_write("faces", "preferences", &bad, sizeof(bad)) == ESP_OK);
    orb_settings_init();
    expect_current(defaults);
    bad = valid; bad.colour = 9;
    CHECK(orb_fs_write("faces", "preferences", &bad, sizeof(bad)) == ESP_OK);
    orb_settings_init();
    expect_current(defaults);
    CHECK(orb_fs_write("faces", "preferences", &valid, sizeof(valid) - 1) == ESP_OK);
    orb_settings_init();
    expect_current(defaults);
    unsigned char oversized[sizeof(valid) + 1] = {0};
    memcpy(oversized, &valid, sizeof(valid));
    CHECK(orb_fs_write("faces", "preferences", oversized, sizeof(oversized)) == ESP_OK);
    orb_settings_init();
    expect_current(defaults);
    orb_settings_snapshot(&state);
    CHECK(!state.saving && !state.save_error); /* Read failures remain silent. */
}

static void fifo_and_lifetime(const orb_faces_prefs_t *defaults)
{
    orb_settings_init();
    orb_faces_prefs_t candidates[ORB_SETTINGS_FACE_REQUESTS];
    uint64_t tickets[ORB_SETTINGS_FACE_REQUESTS];
    unsigned renames = s_renames;
    for (unsigned i = 0; i < ORB_SETTINGS_FACE_REQUESTS; i++) {
        candidates[i] = *defaults;
        candidates[i].colour = (uint8_t)i;
        tickets[i] = orb_settings_faces_request(&candidates[i]);
        CHECK(tickets[i] && orb_settings_faces_result(tickets[i]) == ORB_SETTINGS_PENDING);
    }
    CHECK(!orb_settings_faces_request(defaults) && renames == s_renames);
    orb_settings_faces_release(tickets[0]);
    CHECK(orb_settings_faces_result(tickets[0]) == ORB_SETTINGS_UNKNOWN);
    for (unsigned i = 0; i < ORB_SETTINGS_FACE_REQUESTS; i++) {
        orb_settings_service();
        CHECK(s_renames == renames + i + 1);
        expect_record(&candidates[i]); /* Every admitted write, in order. */
        expect_current(&candidates[i]);
        CHECK(orb_settings_faces_result(tickets[i]) == (i ? ORB_SETTINGS_SAVED : ORB_SETTINGS_UNKNOWN));
        if (i + 1 < ORB_SETTINGS_FACE_REQUESTS)
            CHECK(orb_settings_faces_result(tickets[i + 1]) == ORB_SETTINGS_PENDING);
    }
    orb_settings_snapshot_t state;
    orb_settings_snapshot(&state);
    CHECK(!state.saving && state.save_error); /* Rejection is not hidden by older results. */
    for (unsigned i = 1; i < ORB_SETTINGS_FACE_REQUESTS; i++) orb_settings_faces_release(tickets[i]);
    uint64_t ticket = orb_settings_faces_request(defaults);
    CHECK(ticket);
    orb_settings_faces_release(ticket); /* Destroyed requester owns no result. */
    orb_settings_service();
    expect_record(defaults);
    CHECK(orb_settings_faces_result(ticket) == ORB_SETTINGS_UNKNOWN);
    orb_settings_snapshot(&state);
    CHECK(!state.saving && !state.save_error);
}

int main(void)
{
    orb_faces_prefs_t defaults;
    orb_settings_faces_current(&defaults);
    orb_settings_init();
    orb_settings_snapshot_t state;
    orb_settings_snapshot(&state);
    CHECK(!state.saving && !state.save_error);
    uint64_t ticket = orb_settings_faces_request(&defaults);
    CHECK(ticket && orb_settings_faces_result(ticket) == ORB_SETTINGS_PENDING);
    orb_settings_service();
    CHECK(orb_settings_faces_result(ticket) == ORB_SETTINGS_FAILED);
    orb_settings_snapshot(&state);
    CHECK(!state.saving && state.save_error);
    expect_current(&defaults);
    orb_settings_faces_release(ticket);

    CHECK(mkdtemp(s_base));
    CHECK(orb_fs_mount() == ESP_OK);
    orb_settings_init();
    orb_settings_snapshot(&state);
    CHECK(!state.saving && !state.save_error);
    char faces_path[128];
    int length = snprintf(faces_path, sizeof(faces_path), "%s/apps/faces", s_base);
    CHECK(length > 0 && (size_t)length < sizeof(faces_path));
    struct stat status;
    CHECK(stat(faces_path, &status) && errno == ENOENT);
    orb_faces_prefs_t first = defaults;
    first.face = ORB_FACE_ACTIVITY;
    first.colour = 2;
    unsigned locks = s_locks;
    uint64_t revision = state.revision;
    ticket = orb_settings_faces_request(&first);
    CHECK(ticket && s_locks == locks && stat(faces_path, &status) && errno == ENOENT);
    orb_settings_snapshot(&state);
    CHECK(state.revision > revision && state.saving && !state.save_error);
    expect_current(&defaults);
    orb_settings_service();
    CHECK(orb_settings_faces_result(ticket) == ORB_SETTINGS_SAVED);
    expect_record(&first);
    expect_current(&first);
    orb_settings_faces_release(ticket);

    first.colour = 3;
    ticket = orb_settings_faces_request(&first);
    s_during = first;
    s_during.colour = 4;
    s_fail_rename = s_enqueue = true;
    orb_settings_service();
    CHECK(orb_settings_faces_result(ticket) == ORB_SETTINGS_FAILED);
    CHECK(orb_settings_faces_result(s_during_ticket) == ORB_SETTINGS_PENDING);
    orb_settings_snapshot(&state);
    CHECK(state.saving && !state.save_error);
    s_fail_rename = false;
    orb_settings_service();
    CHECK(orb_settings_faces_result(s_during_ticket) == ORB_SETTINGS_SAVED);
    expect_record(&s_during);
    orb_settings_faces_release(ticket);
    orb_settings_faces_release(s_during_ticket);

    ticket = orb_settings_faces_request(&first);
    s_fail_rename = true;
    orb_settings_service();
    CHECK(orb_settings_faces_result(ticket) == ORB_SETTINGS_FAILED);
    orb_settings_snapshot(&state);
    CHECK(!state.saving && state.save_error);
    expect_record(&s_during);
    expect_current(&s_during);
    unsigned renames = s_renames;
    revision = state.revision;
    orb_settings_service();
    orb_settings_snapshot(&state);
    CHECK(renames == s_renames && state.revision == revision); /* No automatic retry. */
    orb_settings_faces_release(ticket);
    s_fail_rename = false;
    ticket = orb_settings_faces_request(&first); /* Existing explicit retry trigger. */
    orb_settings_service();
    CHECK(orb_settings_faces_result(ticket) == ORB_SETTINGS_SAVED);
    orb_settings_faces_release(ticket);
    expect_record(&first);

    fifo_and_lifetime(&defaults);
    load_records(&defaults);

    CHECK(orb_fs_remove("faces", "preferences") == ESP_OK);
    CHECK(!rmdir(faces_path));
    FILE *blocked = fopen(faces_path, "w");
    CHECK(blocked && !fclose(blocked));
    orb_settings_init();
    expect_current(&defaults);
    orb_settings_snapshot(&state);
    CHECK(!state.save_error);
    ticket = orb_settings_faces_request(&first);
    orb_settings_service();
    CHECK(orb_settings_faces_result(ticket) == ORB_SETTINGS_FAILED); /* Prepare failure. */
    orb_settings_faces_release(ticket);
    CHECK(!unlink(faces_path));
    char apps_path[128];
    length = snprintf(apps_path, sizeof(apps_path), "%s/apps", s_base);
    CHECK(length > 0 && (size_t)length < sizeof(apps_path));
    CHECK(!rmdir(apps_path) && !rmdir(s_base));
    printf("settings storage: %u checks passed\n", s_checks);
    return 0;
}
