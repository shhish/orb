/* Production presenter and asynchronous settings service over a bounded storage
 * port. Public app actions exercise persistence independently of widget topology. */
#include "orb_faces.h"
#include "orb_fs.h"
#include "orb_kit.h"
#include "orb_model.h"
#include "orb_settings.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned s_prepares, s_writes;
static bool s_fail;
static orb_faces_prefs_t s_record;

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
    if (s_fail) return ESP_FAIL;
    memcpy(&s_record, data, size);
    return ESP_OK;
}

static void update(orb_faces_t *faces)
{
    const orb_faces_snapshot_t data = {.now = 1793208527, .weather_override = true,
        .battery_percent = 80, .usage_percent = {-1, -1}};
    /* Identical snapshots still reconcile the service's completed save. */
    orb_faces_update(faces, &data);
}

static bool saving(void)
{
    orb_settings_snapshot_t state;
    orb_settings_snapshot(&state);
    assert(state.revision != 0);
    return state.saving;
}

int main(void)
{
    lv_init();
    assert(sim_display_create());
    orb_ui_init(NULL);
    assert(orb_model_init());
    orb_settings_init();
    orb_faces_t *faces = orb_faces_create(lv_screen_active(), NULL);
    assert(faces);
    update(faces);

    orb_faces_set_mode(faces, ORB_FACES_GALLERY);
    orb_faces_set_mode(faces, ORB_FACES_COLOUR);
    orb_faces_rotate(faces, 1);
    assert(!saving() && !s_prepares && !s_writes); /* Editing never autosaves. */
    assert(orb_faces_back(faces));
    assert(saving() && !s_prepares && !s_writes);
    assert(orb_faces_mode(faces) == ORB_FACES_COLOUR && !orb_faces_save_error(faces));
    assert(orb_faces_back(faces) && saving()); /* One in-flight action, no replay. */
    s_fail = true;
    orb_settings_service();
    assert(s_prepares == 1 && s_writes == 1 && !saving());
    update(faces);
    assert(orb_faces_mode(faces) == ORB_FACES_COLOUR && orb_faces_save_error(faces));
    update(faces); orb_settings_service();
    assert(s_writes == 1); /* A failed result waits for the next explicit trigger. */
    s_fail = false;
    assert(orb_faces_back(faces) && saving());
    orb_settings_service(); update(faces);
    assert(s_writes == 2 && s_record.colour == 1);
    assert(orb_faces_mode(faces) == ORB_FACES_GALLERY && !orb_faces_save_error(faces));

    orb_faces_set_mode(faces, ORB_FACES_COLOUR);
    orb_faces_rotate(faces, 2);
    assert(orb_faces_back(faces) && saving());
    orb_faces_rotate(faces, 3); /* A late completion cannot clear a newer draft. */
    orb_settings_service(); update(faces);
    assert(s_record.colour == 2 && orb_faces_mode(faces) == ORB_FACES_COLOUR);
    assert(orb_faces_back(faces) && saving());
    orb_settings_service(); update(faces);
    assert(s_record.colour == 3 && orb_faces_mode(faces) == ORB_FACES_GALLERY);

    orb_faces_set_mode(faces, ORB_FACES_FACE);
    orb_faces_select(faces, ORB_FACE_ACTIVITY);
    assert(!saving());
    assert(!orb_faces_save(faces) && saving());
    orb_faces_destroy(faces); /* Detaches the observer, without cancelling I/O. */
    orb_settings_service();
    orb_faces_prefs_t current;
    orb_faces_current_preferences(&current);
    assert(current.face == ORB_FACE_ACTIVITY && !saving());
    faces = orb_faces_create(lv_screen_active(), NULL);
    assert(faces);
    orb_faces_preferences(faces, &current);
    assert(current.face == ORB_FACE_ACTIVITY && current.colour == 3);

    /* A confirmed selection remains selected if privacy covers the view before
     * storage completes, and failure still restores its unsaved browsing state. */
    orb_faces_set_mode(faces, ORB_FACES_GALLERY);
    orb_faces_rotate(faces, ORB_FACE_ORB);
    orb_faces_confirm_selection(faces);
    assert(saving() && orb_faces_mode(faces) == ORB_FACES_GALLERY);
    orb_faces_set_always_on(faces, true);
    orb_settings_service(); update(faces);
    orb_faces_preferences(faces, &current);
    assert(current.face == ORB_FACE_ORB && s_record.face == ORB_FACE_ORB);
    assert(!orb_faces_save_error(faces) && orb_faces_save(faces) && !saving());
    orb_faces_set_always_on(faces, false);
    orb_faces_set_mode(faces, ORB_FACES_GALLERY);
    orb_faces_rotate(faces, ORB_FACE_ACTIVITY);
    s_fail = true;
    orb_faces_confirm_selection(faces);
    orb_settings_service(); update(faces);
    assert(orb_faces_save_error(faces) && orb_faces_mode(faces) == ORB_FACES_GALLERY);
    s_fail = false;
    assert(orb_faces_back(faces) && orb_faces_mode(faces) == ORB_FACES_FACE);
    assert(!saving());
    orb_faces_preferences(faces, &current);
    assert(current.face == ORB_FACE_ORB);
    orb_faces_set_mode(faces, ORB_FACES_GALLERY);
    orb_faces_rotate(faces, ORB_FACE_MERIDIAN);
    orb_faces_set_mode(faces, ORB_FACES_COLOUR);
    orb_faces_rotate(faces, 4);
    orb_faces_set_always_on(faces, true);
    assert(saving() && orb_faces_mode(faces) == ORB_FACES_FACE);
    orb_settings_service(); update(faces);
    orb_faces_preferences(faces, &current);
    assert(current.face == ORB_FACE_ORB && s_record.face == ORB_FACE_ORB);
    assert(s_record.colour == 4 && orb_faces_mode(faces) == ORB_FACES_FACE);
    orb_faces_set_always_on(faces, false);
    orb_faces_destroy(faces);
    assert(s_prepares == s_writes && !saving());
    lv_deinit();
    puts("Faces asynchronous settings: navigation, failure, retry, draft and detached lifetime passed");
    return 0;
}
