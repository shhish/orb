#include "orb_color_tokens.h"
#include "orb_apps.h"

#include <math.h>
#include <string.h>

static struct {
    orb_marble_snapshot_t view;
    int64_t previous;
    float nudge_x, nudge_y;
    bool entered;
} s_marble;

static esp_err_t enter(void)
{
    memset(&s_marble, 0, sizeof(s_marble));
    s_marble.entered = true;
    s_marble.previous = -1;
    return ESP_OK;
}

static void leave(void) { s_marble.entered = false; }
static void pause_app(bool paused)
{
    s_marble.view.paused = paused;
    s_marble.previous = -1; /* No jump after a long sleep. */
    s_marble.nudge_x = s_marble.nudge_y = 0;
}

static float bounded(float value)
{
    if (!isfinite(value)) return 0;
    return value < -1 ? -1 : (value > 1 ? 1 : value);
}

static void tick(const orb_app_input_t *input)
{
    float dt = 0;
    if (s_marble.previous >= 0 && input->now_us > s_marble.previous)
        dt = (float)(input->now_us - s_marble.previous) / 1000000.0f;
    if (dt > 0.05f) dt = 0.05f;
    s_marble.previous = input->now_us;
    if (dt == 0 && s_marble.nudge_x == 0 && s_marble.nudge_y == 0) return;
    float x = input->motion_valid ? bounded(input->accel_x) : 0;
    float y = input->motion_valid ? bounded(input->accel_y) : 0;
    /* Specific force points up; screen y points down. Touch nudges already
     * describe the desired movement and keep their accessible direction. */
    s_marble.view.x += -x * dt + s_marble.nudge_x;
    s_marble.view.y += y * dt - s_marble.nudge_y;
    s_marble.nudge_x = s_marble.nudge_y = 0;
    float length = hypotf(s_marble.view.x, s_marble.view.y);
    /* Controller coordinates fit a circle inset by 10%; the eventual renderer
     * can preserve round-first safe space without any rectangle-edge clamp. */
    if (length > 0.9f) {
        s_marble.view.x *= 0.9f / length;
        s_marble.view.y *= 0.9f / length;
    }
}

static const orb_app_entry_t s_entry = { enter, leave, pause_app, tick };
const orb_app_manifest_t orb_marble_manifest = {
    .id = "marble", .name = "Marble", .icon = "\xee\x84\x86", /* ORB_ICON_CIRCLE_FILL, Framework7. */
    .colour = ORB_RGB_LEGACY_BLUE, /* hig.md section 5 system blue; shell draws a flat icon. */
    .needs = ORB_APP_NEEDS_IMU,
    .entry = &s_entry,
};

bool orb_marble_snapshot(orb_marble_snapshot_t *out)
{
    if (!out || !s_marble.entered) return false;
    *out = s_marble.view;
    return true;
}

void orb_marble_nudge(float x, float y)
{
    if (!s_marble.entered || s_marble.view.paused) return;
    s_marble.nudge_x = bounded(x) * 0.08f;
    s_marble.nudge_y = bounded(y) * 0.08f;
}
