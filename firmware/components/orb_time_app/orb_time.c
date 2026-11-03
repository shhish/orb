#include "orb_color_tokens.h"
#include "orb_apps.h"
#include "orb_time.h"

/* Additive provider hooks keep orb_apps independent of UI libraries. An app
 * has no capability requirements: audio failure must leave the visual usable. */
static esp_err_t enter(void) { return ESP_OK; }
static void leave(void) {}
static void pause_app(bool paused) { (void)paused; }
static void tick(const orb_app_input_t *input) { (void)input; }
static const orb_app_entry_t s_entry = {enter, leave, pause_app, tick};
static const orb_app_manifest_t s_manifest = {
    .id = "time", .name = "Time", .icon = "\xee\x82\x91",
    .colour = ORB_RGB_LEGACY_TEAL, .needs = 0, .entry = &s_entry,
};

void orb_apps_register_utilities(void) { (void)orb_apps_register(&s_manifest); }
void orb_apps_poll_utilities(int64_t now_us) { orb_time_poll(now_us, time(NULL)); }
