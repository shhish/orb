#include "orb_apps.h"
#include "orb_fs.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned s_checks, s_enters, s_exits, s_pauses, s_ticks, s_prepares;
static bool s_storage_ready;
static esp_err_t s_enter_error, s_prepare_error;
static orb_app_input_t s_input;
#define CHECK(value) do { ++s_checks; assert(value); } while (0)

bool orb_fs_valid_app_id(const char *id)
{
    if (!id || !*id || strlen(id) > ORB_FS_APP_ID_MAX) return false;
    for (const char *p = id; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return false;
    }
    return true;
}
bool orb_fs_ready(void) { return s_storage_ready; }
esp_err_t orb_fs_app_prepare(const char *id)
{
    CHECK(id != NULL);
    ++s_prepares;
    return s_prepare_error;
}
static esp_err_t enter(void) { ++s_enters; return s_enter_error; }
static void leave(void) { ++s_exits; CHECK(orb_apps_active() == NULL); }
static void pause_app(bool paused) { ++s_pauses; CHECK(orb_apps_paused() == paused); }
static void tick(const orb_app_input_t *input) { ++s_ticks; s_input = *input; }
static const orb_app_entry_t s_entry = { enter, leave, pause_app, tick };
static const orb_app_manifest_t s_test = { "test", "Test", "x", 0x0091ff, 0, &s_entry };
static const orb_app_manifest_t s_storage = { "stored", "Stored", "x", 0x0091ff, ORB_APP_NEEDS_STORAGE, &s_entry };

int main(void)
{
    orb_apps_init(0);
    CHECK(orb_apps_count() == 1);
    CHECK(!orb_apps_active());
    CHECK(!orb_apps_paused());
    CHECK(orb_apps_at(0) == orb_apps_find("marble"));
    CHECK(!orb_apps_at(1));
    CHECK(!orb_apps_at(SIZE_MAX));
    CHECK(!orb_apps_find(NULL));
    CHECK(orb_apps_enter("missing") == ESP_ERR_NOT_FOUND);
    CHECK(orb_apps_enter("marble") == ESP_ERR_NOT_SUPPORTED);
    CHECK(orb_apps_register(NULL) == ESP_ERR_INVALID_ARG);
    CHECK(orb_apps_register(orb_apps_at(0)) == ESP_ERR_INVALID_STATE);
    orb_app_manifest_t bad = s_test;
    bad.id = "../bad"; CHECK(orb_apps_register(&bad) == ESP_ERR_INVALID_ARG);
    bad = s_test; bad.name = ""; CHECK(orb_apps_register(&bad) == ESP_ERR_INVALID_ARG);
    bad = s_test; bad.icon = NULL; CHECK(orb_apps_register(&bad) == ESP_ERR_INVALID_ARG);
    bad = s_test; bad.colour = 0x1000000; CHECK(orb_apps_register(&bad) == ESP_ERR_INVALID_ARG);
    bad = s_test; bad.needs = 16; CHECK(orb_apps_register(&bad) == ESP_ERR_INVALID_ARG);
    bad = s_test; bad.entry = NULL; CHECK(orb_apps_register(&bad) == ESP_ERR_INVALID_ARG);
    orb_app_entry_t bad_entry = s_entry;
    bad_entry.pause = NULL; bad = s_test; bad.entry = &bad_entry;
    CHECK(orb_apps_register(&bad) == ESP_ERR_INVALID_ARG);
    CHECK(orb_apps_register(&s_test) == ESP_OK);
    CHECK(orb_apps_register(&s_storage) == ESP_OK);
    CHECK(orb_apps_enter("stored") == ESP_ERR_NOT_SUPPORTED);
    CHECK(orb_apps_enter("test") == ESP_OK);
    CHECK(s_enters == 1);
    CHECK(orb_apps_enter("test") == ESP_ERR_INVALID_STATE);
    orb_apps_poll(0, true, true, 0, 0, 1, true);
    orb_apps_poll(1199999, true, true, 0, 0, 1, true);
    CHECK(orb_apps_active() == &s_test);
    orb_apps_poll(1200000, true, true, 0, 0, 1, true);
    CHECK(!orb_apps_active());
    CHECK(s_exits == 1);
    CHECK(orb_apps_enter("test") == ESP_OK);
    orb_apps_poll(2000000, true, false, 0, 0, 1, true);
    CHECK(orb_apps_paused());
    CHECK(s_pauses == 1);
    unsigned ticks = s_ticks;
    orb_apps_poll(3000000, false, false, 0, 0, 1, true);
    orb_apps_poll(4000000, true, false, 0, 0, 1, true);
    orb_apps_poll(5199999, true, false, 0, 0, 1, true);
    CHECK(orb_apps_active() == &s_test);
    CHECK(s_ticks == ticks);
    orb_apps_poll(5200000, true, false, 0, 0, 1, true);
    CHECK(!orb_apps_active());
    CHECK(!orb_apps_paused());
    CHECK(orb_apps_enter("test") == ESP_OK);
    orb_apps_poll(100, false, false, 0, 0, 1, true);
    orb_apps_poll(200, false, true, NAN, INFINITY, 1, true);
    CHECK(!orb_apps_paused());
    CHECK(!s_input.motion_valid && s_input.accel_x == 0 && s_input.accel_y == 0);
    CHECK(s_input.now_us == 200);
    CHECK(s_input.motion_sample_us == 200);
    orb_apps_poll(220, false, true, -0.5f, 0, 0.866f, true);
    CHECK(s_input.motion_valid && s_input.accel_x == -0.5f && s_input.accel_z == 0.866f);
    CHECK(s_input.motion_sample_us == 220);
    CHECK(!s_input.gyro_valid && s_input.gyro_x == 0 &&
          s_input.gyro_y == 0 && s_input.gyro_z == 0);
    orb_apps_poll(240, false, true, 0, 0, NAN, true);
    CHECK(!s_input.motion_valid && s_input.accel_z == 0);
    /* A delayed app service must preserve sensor age, while the calibrated
     * physical gyro arrives independently of acceleration-based steering. */
    orb_app_input_t measured = {
        .now_us = 250,
        .motion_sample_us = 245,
        .accel_x = -0.087f, .accel_y = 0.040f, .accel_z = 0.916f,
        .motion_valid = true,
        .gyro_x = -0.25f, .gyro_y = 2.5f, .gyro_z = -1.25f,
        .gyro_valid = true,
    };
    orb_apps_poll_input(&measured, false, true);
    CHECK(s_input.now_us == 250 && s_input.motion_sample_us == 245);
    CHECK(s_input.motion_valid && s_input.gyro_valid &&
          s_input.gyro_x == -0.25f && s_input.gyro_y == 2.5f && s_input.gyro_z == -1.25f);
    measured.now_us = 260;
    measured.gyro_y = INFINITY;
    orb_apps_poll_input(&measured, false, true);
    CHECK(s_input.motion_valid && !s_input.gyro_valid &&
          s_input.gyro_x == 0 && s_input.gyro_y == 0 && s_input.gyro_z == 0);
    CHECK(isinf(measured.gyro_y)); /* Caller-owned sample remains untouched. */
    measured.now_us = 270;
    measured.gyro_y = 2.5f;
    measured.motion_valid = false;
    orb_apps_poll_input(&measured, false, true);
    CHECK(!s_input.motion_valid && !s_input.gyro_valid && s_input.gyro_y == 0);
    ticks = s_ticks;
    orb_apps_poll_input(NULL, false, true);
    CHECK(s_ticks == ticks);
    /* PWR's sheet pause is applied before app tick, survives waking and never
     * resumes the game while the display still owns a pause reason. */
    unsigned pause_ticks = s_ticks;
    orb_apps_request_pause(true);
    orb_apps_poll(300, false, true, 0, 0, 1, true);
    CHECK(orb_apps_paused() && s_ticks == pause_ticks);
    orb_apps_poll(400, false, false, 0, 0, 1, true);
    orb_apps_poll(500, false, true, 0, 0, 1, true);
    CHECK(orb_apps_paused() && s_ticks == pause_ticks);
    orb_apps_request_pause(false);
    orb_apps_poll(600, false, false, 0, 0, 1, true);
    CHECK(orb_apps_paused() && s_ticks == pause_ticks);
    orb_apps_poll(700, false, true, 0, 0, 1, true);
    CHECK(!orb_apps_paused() && s_ticks == pause_ticks + 1);
    orb_apps_request_pause(true);
    orb_apps_poll(800, false, true, 0, 0, 1, true);
    CHECK(orb_apps_paused());
    orb_apps_exit();
    CHECK(orb_apps_enter("test") == ESP_OK);
    orb_apps_poll(900, false, true, 0, 0, 1, true);
    CHECK(!orb_apps_paused());
    orb_apps_exit();
    unsigned exits = s_exits;
    orb_apps_exit();
    CHECK(s_exits == exits);
    s_enter_error = ESP_FAIL;
    CHECK(orb_apps_enter("test") == ESP_FAIL);
    CHECK(!orb_apps_active());
    CHECK(s_exits == exits + 1);
    s_enter_error = ESP_OK;
    s_storage_ready = true;
    s_prepare_error = ESP_FAIL;
    CHECK(orb_apps_enter("test") == ESP_FAIL);
    CHECK(!orb_apps_active());
    CHECK(s_prepares == 1);
    s_prepare_error = ESP_OK;
    CHECK(orb_apps_enter("test") == ESP_OK);
    CHECK(s_prepares == 2);
    orb_apps_init(ORB_APP_NEEDS_IMU | ORB_APP_NEEDS_STORAGE);
    CHECK(!orb_apps_active());
    CHECK(orb_apps_count() == 1);
    CHECK(orb_apps_register(&s_storage) == ESP_OK);
    CHECK(orb_apps_enter("stored") == ESP_OK);
    orb_apps_exit();
    CHECK(orb_apps_enter("marble") == ESP_OK);
    orb_marble_snapshot_t view;
    CHECK(orb_marble_snapshot(&view));
    CHECK(view.x == 0 && view.y == 0);
    /* Physical specific force: right side down ax < 0 moves right; bottom
     * side down ay > 0 moves down in the LVGL coordinate system. */
    orb_apps_poll(0, false, true, -0.5f, 0.5f, 0.7071f, true);
    orb_apps_poll(20000, false, true, -0.5f, 0.5f, 0.7071f, true);
    CHECK(orb_marble_snapshot(&view) && view.x > 0 && view.y > 0);
    for (int64_t i = 2; i < 1000; ++i)
        orb_apps_poll(i * 20000, false, true, -0.5f, 0.5f, 0.7071f, true);
    CHECK(orb_marble_snapshot(&view));
    CHECK(hypotf(view.x, view.y) <= 0.90001f);
    float old_x = view.x, old_y = view.y;
    orb_apps_poll(20000000, false, false, -0.5f, 0.5f, 0.7071f, true);
    orb_apps_poll(90000000, false, false, -0.5f, 0.5f, 0.7071f, true);
    CHECK(orb_marble_snapshot(&view) && view.paused);
    CHECK(view.x == old_x && view.y == old_y);
    orb_apps_poll(90020000, false, true, -0.5f, 0.5f, 0.7071f, true);
    CHECK(orb_marble_snapshot(&view) && !view.paused);
    CHECK(view.x == old_x && view.y == old_y);
    orb_apps_exit();
    CHECK(!orb_marble_snapshot(&view));
    CHECK(!orb_marble_snapshot(NULL));
    CHECK(orb_apps_enter("marble") == ESP_OK);
    orb_marble_nudge(1, 0);
    orb_apps_poll(0, false, true, 0, 0, 1, false);
    CHECK(orb_marble_snapshot(&view));
    CHECK(view.x > 0); /* Accessible button input works without the IMU. */
    orb_apps_exit();
    static orb_app_manifest_t extra[7];
    static char ids[7][16];
    orb_apps_init(ORB_APP_NEEDS_IMU);
    for (unsigned i = 0; i < 7; ++i) {
        snprintf(ids[i], sizeof(ids[i]), "app%u", i);
        extra[i] = s_test; extra[i].id = ids[i];
        CHECK(orb_apps_register(&extra[i]) == ESP_OK);
    }
    CHECK(orb_apps_count() == ORB_APPS_MAX);
    CHECK(orb_apps_register(&s_test) == ESP_ERR_NO_MEM);
    printf("orb_apps: %u checks passed\n", s_checks);
    return 0;
}
