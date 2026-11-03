/* Real LVGL and game runtime: a retained game under an opaque privacy root
 * must stop its presentation timer, then resume without a physics catch-up. */
#include "orb_app_play.h"
#include "orb_apps.h"
#include "orb_fs.h"
#include "orb_home.h"
#include "orb_model.h"
#include "orb_ui.h"
#include "sim_display.h"
#include "src/misc/lv_timer_private.h"

#include <stdlib.h>
#include <stdio.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); exit(1); \
} } while (0)
#include <string.h>

static uint32_t s_now;
static unsigned s_callbacks, s_frames;
static lv_timer_cb_t s_game_timer;

bool orb_fs_valid_app_id(const char *id) { return id && *id && strlen(id) <= ORB_FS_APP_ID_MAX; }
bool orb_fs_ready(void) { return false; }
esp_err_t orb_fs_app_prepare(const char *id) { return orb_fs_valid_app_id(id) ? ESP_OK : ESP_ERR_INVALID_ARG; }
esp_err_t orb_fs_read(const char *id, const char *key, void *data, size_t capacity, size_t *size)
{
    (void)id; (void)key; (void)data; (void)capacity; *size = 0;
    return ESP_ERR_NOT_FOUND;
}
esp_err_t orb_fs_write(const char *id, const char *key, const void *data, size_t size)
{
    (void)id; (void)key; (void)data; (void)size;
    return ESP_OK;
}

static uint32_t tick(void) { return s_now; }
static void observe(lv_timer_cb_t callback, bool entering)
{
    if (entering && callback == s_game_timer) s_callbacks++;
}
static void advance(unsigned milliseconds, bool visible)
{
    for (unsigned i = 0; i < milliseconds; i += 20) {
        s_now += 20;
        orb_apps_poll((int64_t)s_now * 1000, false, visible, 0, 0, 1, false);
        lv_timer_handler();
        if (sim_display_take_changed()) s_frames++;
    }
}

int main(void)
{
    lv_init(); lv_tick_set_cb(tick);
    CHECK(sim_display_create());
    orb_ui_init(NULL); CHECK(orb_model_init()); CHECK(orb_home_init());
    CHECK(orb_app_play_init());
    orb_apps_init(ORB_APP_NEEDS_IMU | ORB_APP_NEEDS_STORAGE);
    lv_timer_set_observer_cb(observe);
    static const char *const games[] = {"marble", "beads", "orbit", "ring-snake", "pop"};
    for (unsigned i = 0; i < sizeof(games) / sizeof(games[0]); i++) {
        CHECK(orb_apps_enter(games[i]) == ESP_OK);
        /* Runtime creates its presentation timer last, at the list head. */
        lv_timer_t *timer = lv_timer_get_next(NULL);
        CHECK(timer && timer->period == 17);
        s_game_timer = timer->timer_cb;
        s_callbacks = 0;
        advance(1000, true);
        CHECK(s_callbacks > 0 && !lv_timer_get_paused(timer));
        lv_obj_t *privacy = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(privacy);
        lv_obj_set_size(privacy, 466, 466);
        lv_obj_set_style_bg_color(privacy, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(privacy, LV_OPA_COVER, 0);
        advance(1500, false); /* Allow existing bounded material effects to settle. */
        CHECK(orb_apps_paused() && lv_timer_get_paused(timer));
        s_callbacks = s_frames = 0;
        advance(2000, false);
        CHECK(s_callbacks == 0 && s_frames == 0);
        lv_obj_delete(privacy);
        s_callbacks = 0;
        advance(500, true);
        CHECK(!orb_apps_paused() && !lv_timer_get_paused(timer) && s_callbacks > 0);
        orb_apps_exit();
        s_game_timer = NULL;
        advance(100, true);
    }
    puts("game idle: all five retained games stop timer and refresh under privacy root");
    return 0;
}
