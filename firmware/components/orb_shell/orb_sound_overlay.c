/* Design 2.0 / HIG §§1–5: kit identities and controls, black content,
 * and a display-only volume observer. No full-screen cache. */
#include "orb_sound_overlay.h"
#include "orb_fonts.h"
#include "orb_input.h"
#include "orb_sound.h"
#include "orb_model.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include "orb_kit_motion.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static lv_obj_t *canvas(lv_display_t *display)
{
    lv_obj_t *root = lv_obj_create(lv_display_get_layer_top(display));
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, 466, 466);
    lv_obj_set_style_radius(root, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(root, false);
    lv_obj_set_clickable(root, true); /* Modal surfaces consume taps, never pass through to Send. */
    return root;
}

static lv_obj_t *text(lv_obj_t *parent, const char *value, orb_text_style_t style,
                       bool bold, int y, int width, lv_color_t ink)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_clickable(label, false);
    lv_obj_set_scrollable(label, false);
    orb_text_apply(label, style, bold ? ORB_TEXT_EMPHASIZED : ORB_TEXT_REGULAR, 0);
    lv_label_set_text(label, value);
    int height = lv_obj_get_style_text_font(label, 0)->line_height;
    width = LV_MIN(width, orb_chord_width(y, y + height - 1, ORB_RIM_MARGIN_TEXT));
    lv_obj_set_size(label, width, height);
    lv_obj_set_pos(label, (466 - width) / 2, y);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, ink, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    return label;
}

/* The HUD reports a change; it never becomes an input owner. Volume is
 * edited only by the visible Control Center or Sound settings slider. */
static void volume_build(orb_sound_overlay_t *view, lv_display_t *display)
{
    view->volume = canvas(display);
    view->volume_presented = false;
    orb_kit_motion_set_active(view->volume, view->motion_active);
    lv_obj_set_clickable(view->volume, false);
    view->volume_arc = orb_kit_level_create(view->volume, ORB_KIT_SOUND, 0, 100, 1, NULL, NULL);
    lv_obj_set_clickable(view->volume_arc, false);
    orb_kit_level_set_title(view->volume_arc, "Volume");
    orb_kit_level_set_text_style(view->volume_arc, ORB_TEXT_LARGE_TITLE, ORB_TEXT_EMPHASIZED);
    orb_kit_level_set_value_center(view->volume_arc, 233, 240);
    view->volume_hint = text(view->volume, "", ORB_TEXT_FOOTNOTE2,
                             false, 284, 300, ORB_KIT_SECONDARY);
    lv_obj_set_style_text_opa(view->volume_hint, ORB_KIT_SECONDARY_OPA, 0);
}

/* HIG §§5/8: the source in words, the level in glyph steps, and Silent as a
 * slashed speaker over a grey arc, never a full speaker at 60% in pink. */
static void volume_present(orb_sound_overlay_t *view, unsigned volume, bool media)
{
    orb_sound_preferences_t preferences;
    orb_sound_preferences(&preferences);
    bool silent = !media && preferences.silent;
    orb_kit_level_set_value(view->volume_arc, volume, view->volume_presented);
    view->volume_presented = true;
    orb_kit_level_set_icon(view->volume_arc, silent ? ORB_ICON_SPEAKER_SLASH : volume == 0 ? ORB_ICON_SPEAKER_NONE :
        volume < 50 ? ORB_ICON_SPEAKER_LOW : ORB_ICON_SPEAKER_HIGH);
    orb_kit_level_set_state(view->volume_arc, silent ? ORB_KIT_GREY : ORB_KIT_SOUND,
        silent ? ORB_KIT_SECONDARY : ORB_KIT_SOUND, true);
    lv_label_set_text(view->volume_hint, silent ? "Silent" : media ? "Mac volume" : "Speaker");
}

void orb_sound_overlay_init(orb_sound_overlay_t *view)
{
    memset(view, 0, sizeof(*view)); view->seen_volume = orb_sound_volume();
    view->motion_active = true;
}

void orb_sound_overlay_set_motion_active(orb_sound_overlay_t *view, bool active)
{
    view->motion_active = active;
    orb_kit_motion_set_active(view->volume, active);
    orb_kit_motion_set_active(orb_app_voice_spoken_root(view->spoken), active);
}

void orb_sound_overlay_set_control_visible(orb_sound_overlay_t *view, bool visible)
{
    view->control_visible = visible;
    if (!visible) return;
    /* Consume the observation while its own control already explains it. This
     * also prevents a delayed HUD when the owner leaves the editor. */
    view->seen_volume = orb_sound_volume();
    if (view->volume) {
        lv_obj_delete(view->volume);
        view->volume = view->volume_arc = view->volume_hint = NULL;
    }
}

void orb_sound_overlay_poll(orb_sound_overlay_t *view, lv_display_t *display, bool visible)
{
    unsigned volume = orb_sound_volume();
    if (volume != view->seen_volume) {
        view->seen_volume = volume;
        view->volume_at = lv_tick_get();
        if (visible && !view->control_visible && !view->volume) volume_build(view, display);
        if (view->volume) volume_present(view, volume, false);
    }
    if (view->volume && (!visible || view->control_visible || lv_tick_get() - view->volume_at >= 1500)) {
        lv_obj_delete(view->volume);
        view->volume = view->volume_arc = view->volume_hint = NULL;
    }
    orb_app_voice_spoken_show(view->spoken, visible);
    if (view->volume) lv_obj_move_foreground(view->volume);
}

static bool speech_stop(void *context)
{
    orb_sound_overlay_t *view = context;
    /* The owner can replace its stop port during playback; read it live as
     * the original control callback did, without a stale cached callback. */
    return view->stop && view->stop(view->stop_ctx);
}

void orb_sound_overlay_speech(orb_sound_overlay_t *view, lv_display_t *display,
                              bool active, bool buffering, const char *caption, int agent)
{
    if (!view->spoken && active) view->spoken = orb_app_voice_spoken_create(false, speech_stop, view);
    (void)orb_app_voice_spoken_present(view->spoken, display, active, buffering, caption, agent);
    orb_kit_motion_set_active(orb_app_voice_spoken_root(view->spoken), view->motion_active);
    if (active && view->volume) lv_obj_move_foreground(view->volume);
}

bool orb_sound_overlay_stop(orb_sound_overlay_t *view)
{
    return orb_app_voice_spoken_stop(view->spoken);
}

void orb_sound_overlay_destroy(orb_sound_overlay_t *view)
{
    if (view->volume) lv_obj_delete(view->volume);
    orb_app_voice_spoken_destroy(view->spoken);
    memset(view, 0, sizeof(*view));
}

size_t orb_sound_overlay_size(void) { return sizeof(orb_sound_overlay_t) + orb_app_voice_spoken_size(); }

void orb_sound_overlay_media(orb_sound_overlay_t *view, lv_display_t *display, unsigned volume)
{
    if (volume > 100 || view->control_visible) return;
    view->volume_at = lv_tick_get();
    if (!view->volume) volume_build(view, display);
    volume_present(view, volume, true);
}

void orb_sound_overlay_volume(orb_sound_overlay_t *view, lv_display_t *display)
{
    if (view->control_visible) return;
    /* Explicit external preview; visible editors already show the current level. */
    view->seen_volume = 101;
    orb_sound_overlay_poll(view, display, true);
}

bool orb_sound_overlay_has_speech(const orb_sound_overlay_t *view)
{ return orb_app_voice_spoken_root(view->spoken) != NULL; }
