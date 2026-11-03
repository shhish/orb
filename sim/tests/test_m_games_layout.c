/* Design 2.0 game chrome gate. Frozen real model states make geometry defects
 * reproducible; this fixture never changes gameplay, clocks or storage. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_game_play.h"
#include "orb_game_view.h"
#include "orb_kit.h"
#include "sim_display.h"
#include "src/misc/lv_text_private.h"

static uint32_t s_now;
static unsigned s_checks, s_failures, s_actions;
static const char *s_case;
static const char *s_directory;
static lv_display_t *s_display;
static FILE *s_manifest;
static orb_game_view_t *s_view;
static const orb_game_state_t *s_state;
static uint32_t s_best;
static bool s_save_error;

typedef struct {
    lv_obj_t *object, *control;
    lv_area_t ink;
    const char *text;
} label_ink_t;
static label_ink_t s_labels[96];
static unsigned s_label_count, s_primaries, s_titles;
typedef struct { lv_obj_t *object; lv_area_t bounds; bool circle; } control_t;
static control_t s_controls[48];
static unsigned s_control_count;
static uint8_t s_pixels[SIM_D * SIM_D * 3];
extern size_t sim_memory_allocation_calls(void);

static uint32_t tick(void) { return s_now; }
static void check(bool condition, const char *why)
{
    ++s_checks;
    if (condition) return;
    ++s_failures;
    fprintf(stderr, "games layout [%s]: %s\n", s_case, why);
}
static void settle(void)
{
    /* Advance material/spring timers without ever stepping the game model. */
    for (unsigned i = 0; i < 45; ++i) {
        s_now += 25;
        if (s_view && s_state) orb_game_view_update(s_view, s_state, s_best, false, s_save_error);
        lv_timer_handler();
    }
    lv_obj_update_layout(lv_screen_active());
    lv_refr_now(s_display);
}
static bool intersects(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 <= b->x2 && a->x2 >= b->x1 && a->y1 <= b->y2 && a->y2 >= b->y1;
}
static bool intersection(lv_area_t *area, const lv_area_t *clip)
{
    area->x1 = LV_MAX(area->x1, clip->x1);
    area->x2 = LV_MIN(area->x2, clip->x2);
    area->y1 = LV_MAX(area->y1, clip->y1);
    area->y2 = LV_MIN(area->y2, clip->y2);
    return area->x1 <= area->x2 && area->y1 <= area->y2;
}
static bool label_ink(lv_obj_t *object, lv_area_t *ink)
{
    const char *text = lv_label_get_text(object);
    const lv_font_t *font = lv_obj_get_style_text_font(object, 0);
    lv_area_t bounds;
    /* Letter positions are relative to the content box, including when a
     * balanced caption uses alignment-side padding inside its fixed lane. */
    lv_obj_get_content_coords(object, &bounds);
    bool any = false;
    uint32_t byte = 0, index = 0;
    while (text[byte]) {
        uint32_t cp = lv_text_encoded_next(text, &byte);
        uint32_t peek = byte, next = lv_text_encoded_next(text, &peek);
        lv_font_glyph_dsc_t glyph;
        lv_point_t position;
        lv_label_get_letter_pos(object, index++, &position);
        if (!lv_font_get_glyph_dsc(font, &glyph, cp, next) || !glyph.box_w || !glyph.box_h) continue;
        lv_area_t glyph_ink = {
            .x1 = bounds.x1 + position.x + glyph.ofs_x,
            .y1 = bounds.y1 + position.y + font->line_height - font->base_line - glyph.box_h - glyph.ofs_y,
        };
        glyph_ink.x2 = glyph_ink.x1 + glyph.box_w - 1;
        glyph_ink.y2 = glyph_ink.y1 + glyph.box_h - 1;
        /* Old odometer digits intentionally leave their clipped viewport. Only
         * ink that reaches the panel can overlap another visible object. */
        bool visible = true;
        for (lv_obj_t *parent = lv_obj_get_parent(object); parent; parent = lv_obj_get_parent(parent)) {
            if (lv_obj_is_overflow_visible(parent)) continue;
            lv_area_t clip;
            lv_obj_get_coords(parent, &clip);
            if (!intersection(&glyph_ink, &clip)) { visible = false; break; }
        }
        if (!visible) continue;
        if (!any) *ink = glyph_ink;
        else {
            ink->x1 = LV_MIN(ink->x1, glyph_ink.x1); ink->x2 = LV_MAX(ink->x2, glyph_ink.x2);
            ink->y1 = LV_MIN(ink->y1, glyph_ink.y1); ink->y2 = LV_MAX(ink->y2, glyph_ink.y2);
        }
        any = true;
    }
    return any;
}
static void inspect(lv_obj_t *object, lv_obj_t *control)
{
    if (!lv_obj_is_visible(object) || !lv_obj_get_style_opa(object, 0)) return;
    orb_kit_rim_text_info_t rim;
    if (orb_kit_rim_text_info(object, &rim)) {
        ++s_titles;
        check(rim.angle_degrees <= ORB_KIT_TITLE_MAX_DEGREES + .01f, "rim title exceeds 120 degrees");
        return; /* The cached arc slices are not ordinary rectangular labels. */
    }
    int width = lv_obj_get_width(object), height = lv_obj_get_height(object);
    if (lv_obj_is_clickable(object) && width < SIM_D && height < SIM_D) {
        control = object;
        if (width <= 346 && height <= ORB_KIT_DISC) {
            check(s_control_count < sizeof(s_controls) / sizeof(s_controls[0]), "too many controls");
            if (s_control_count < sizeof(s_controls) / sizeof(s_controls[0])) {
                control_t *entry = &s_controls[s_control_count++];
                entry->object = object; entry->circle = width == height;
                lv_obj_get_coords(object, &entry->bounds);
            }
        }
        if (orb_kit_is_primary(object)) {
            ++s_primaries;
            check(height == ORB_KIT_PRIMARY_H || (width == ORB_KIT_DISC && height == ORB_KIT_DISC),
                  "primary has a non-kit height");
        }
    }
    if (lv_obj_check_type(object, &lv_label_class) && lv_obj_get_style_text_opa(object, 0)) {
        lv_area_t ink;
        if (label_ink(object, &ink)) {
            check(s_label_count < sizeof(s_labels) / sizeof(s_labels[0]), "too many label fixtures");
            if (s_label_count < sizeof(s_labels) / sizeof(s_labels[0]))
                s_labels[s_label_count++] = (label_ink_t){object, control, ink, lv_label_get_text(object)};
            /* Measure glyph ink, not full-width label padding, at both ends of
             * each line box. HIG 3/4: text stays inside the 16px safe circle. */
            int x[] = {ink.x1, ink.x2}, y[] = {ink.y1, ink.y2};
            for (unsigned a = 0; a < 2; ++a) for (unsigned b = 0; b < 2; ++b) {
                if (hypotf(x[a] - 233, y[b] - 233) > 218) {
                    char why[256];
                    snprintf(why, sizeof(why), "text outside safe circle: %s [%d,%d]-[%d,%d]",
                             lv_label_get_text(object), ink.x1, ink.y1, ink.x2, ink.y2);
                    check(false, why);
                }
            }
            orb_kit_text_fit_info_t fit;
            if (orb_kit_hero_fit_info(object, &fit)) check(fit.complete, "hero fitting lost complete identity");
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        inspect(lv_obj_get_child(object, (int32_t)i), control);
}
static lv_obj_t *find_caption(lv_obj_t *object, const char *caption)
{
    if (!lv_obj_is_visible(object)) return NULL;
    if (lv_obj_check_type(object, &lv_label_class) && !strcmp(lv_label_get_text(object), caption))
        return lv_obj_get_parent(object);
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = find_caption(lv_obj_get_child(object, (int32_t)i), caption);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *find_rim(lv_obj_t *object, const char *text)
{
    if (!lv_obj_is_visible(object)) return NULL;
    const char *rim = orb_kit_rim_text_get(object);
    if (rim && !strcmp(rim, text)) return object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = find_rim(lv_obj_get_child(object, (int32_t)i), text);
        if (found) return found;
    }
    return NULL;
}
static void geometry(unsigned expected_primaries)
{
    s_label_count = s_primaries = s_titles = s_control_count = 0;
    inspect(lv_screen_active(), NULL);
    check(s_primaries == expected_primaries, "screen must expose exactly its one primary (or none while playing)");
    for (unsigned i = 0; i < s_label_count; ++i) for (unsigned j = 0; j < i; ++j) {
        if (!intersects(&s_labels[i].ink, &s_labels[j].ink)) continue;
        char why[320];
        snprintf(why, sizeof(why), "text overlap: '%s' [%d,%d]-[%d,%d] / '%s' [%d,%d]-[%d,%d]",
            s_labels[i].text, s_labels[i].ink.x1, s_labels[i].ink.y1, s_labels[i].ink.x2, s_labels[i].ink.y2,
            s_labels[j].text, s_labels[j].ink.x1, s_labels[j].ink.y1, s_labels[j].ink.x2, s_labels[j].ink.y2);
        check(false, why);
    }
    for (unsigned i = 0; i < s_label_count; ++i) for (unsigned j = 0; j < s_control_count; ++j) {
        if (s_labels[i].control == s_controls[j].object) continue;
        bool overlap = intersects(&s_labels[i].ink, &s_controls[j].bounds);
        if (overlap && s_controls[j].circle) {
            float x = (s_controls[j].bounds.x1 + s_controls[j].bounds.x2) * .5f;
            float y = (s_controls[j].bounds.y1 + s_controls[j].bounds.y2) * .5f;
            float nearest_x = LV_CLAMP(s_labels[i].ink.x1, x, s_labels[i].ink.x2);
            float nearest_y = LV_CLAMP(s_labels[i].ink.y1, y, s_labels[i].ink.y2);
            overlap = hypotf(nearest_x - x, nearest_y - y) < lv_area_get_width(&s_controls[j].bounds) * .5f;
        }
        if (overlap) {
            char why[256];
            snprintf(why, sizeof(why), "text overlaps another control: '%s' / [%d,%d]-[%d,%d]",
                s_labels[i].text, s_controls[j].bounds.x1, s_controls[j].bounds.y1,
                s_controls[j].bounds.x2, s_controls[j].bounds.y2);
            check(false, why);
        }
    }
    /* HIG3: the shared80px scoreboard and Snake steering discs retain at
     * least20px between bezels. A focus ring never buys room by shrinking or
     * scaling a role; only placement may establish this gap. */
    for (unsigned i = 0; i < s_control_count; ++i) for (unsigned j = 0; j < i; ++j) {
        if (!s_controls[i].circle || !s_controls[j].circle ||
            lv_area_get_width(&s_controls[i].bounds) != ORB_KIT_DISC ||
            lv_area_get_width(&s_controls[j].bounds) != ORB_KIT_DISC) continue;
        float dx = (s_controls[i].bounds.x1 + s_controls[i].bounds.x2 -
                    s_controls[j].bounds.x1 - s_controls[j].bounds.x2) * .5f;
        float dy = (s_controls[i].bounds.y1 + s_controls[i].bounds.y2 -
                    s_controls[j].bounds.y1 - s_controls[j].bounds.y2) * .5f;
        check(hypotf(dx, dy) - ORB_KIT_DISC >= 19, "standard kit discs have less than20px bezel spacing");
    }
}
static void visible_label_ink(void)
{
    sim_display_rgb(s_pixels);
    for (unsigned i = 0; i < s_label_count; ++i) {
        label_ink_t *label = &s_labels[i];
        const lv_font_t *font = lv_obj_get_style_text_font(label->object, 0);
        if (label->control) {
            check(lv_obj_get_height(label->object) <= font->line_height + 2,
                  "kit control label wraps into a second line");
            lv_point_t full_text;
            lv_text_get_size(&full_text, label->text, font,
                lv_obj_get_style_text_letter_space(label->object, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            check(full_text.x <= lv_obj_get_width(label->object), "control label clips a real score or action");
        }
        uint32_t rgb = lv_color_to_u32(lv_obj_get_style_text_color(label->object, 0));
        unsigned opacity = lv_obj_get_style_text_opa(label->object, 0);
        unsigned r = ((rgb >> 16) & 255) * opacity / 255;
        unsigned g = ((rgb >> 8) & 255) * opacity / 255;
        unsigned b = (rgb & 255) * opacity / 255;
        unsigned visible = 0;
        for (int y = LV_MAX(0, label->ink.y1); y <= LV_MIN(SIM_D - 1, label->ink.y2); ++y)
            for (int x = LV_MAX(0, label->ink.x1); x <= LV_MIN(SIM_D - 1, label->ink.x2); ++x) {
                const uint8_t *pixel = &s_pixels[(y * SIM_D + x) * 3];
                if (abs((int)pixel[0] - (int)r) < 42 && abs((int)pixel[1] - (int)g) < 42 &&
                    abs((int)pixel[2] - (int)b) < 42) ++visible;
            }
        if (visible < 8) {
            char why[200];
            snprintf(why, sizeof(why), "visible label has no painted ink: '%s'", label->text);
            check(false, why);
        }
    }
}
static void clicked_observer(lv_event_t *event)
{
    unsigned *clicks = lv_event_get_user_data(event);
    ++*clicks;
}
static void all_control_contacts(lv_obj_t *object)
{
    if (!lv_obj_is_visible(object)) return;
    if (lv_obj_is_clickable(object) && lv_obj_get_width(object) < SIM_D &&
        lv_obj_get_height(object) < SIM_D) {
        unsigned clicks = 0;
        lv_obj_add_event_cb(object, clicked_observer, LV_EVENT_CLICKED, &clicks);
        lv_obj_send_event(object, LV_EVENT_PRESSED, NULL);
        lv_obj_send_event(object, LV_EVENT_PRESS_LOST, NULL);
        check(orb_kit_press_is_cancelled(object), "tappable does not retire its cancelled contact through the kit");
        lv_obj_send_event(object, LV_EVENT_CLICKED, NULL);
        check(clicks == 0, "cancelled game chrome contact reaches activation");
        lv_obj_remove_event_cb_with_user_data(object, clicked_observer, &clicks);
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        all_control_contacts(lv_obj_get_child(object, (int32_t)i));
}
static void shot_now(const char *name, const char *reference, unsigned primaries)
{
    char path[1024];
    s_case = name;
    lv_obj_update_layout(lv_screen_active());
    lv_refr_now(s_display);
    geometry(primaries);
    visible_label_ink();
    snprintf(path, sizeof(path), "%s/%s.png", s_directory, name);
    check(sim_display_save_png(path), "cannot save screen PNG");
    fprintf(s_manifest, "%s\t%s\n", name, reference);
    all_control_contacts(lv_screen_active());
    if (s_view && s_state) {
        size_t calls = sim_memory_allocation_calls();
        for (unsigned i = 0; i < 20; ++i) orb_game_view_update(s_view, s_state, s_best, false, s_save_error);
        check(calls == sim_memory_allocation_calls(), "unchanged frozen game update allocates component storage");
    }
}
static void shot(const char *name, const char *reference, unsigned primaries)
{
    s_case = name;
    settle();
    shot_now(name, reference, primaries);
}
static void picker_action(orb_game_kind_t kind, void *context)
{
    (void)kind; (void)context;
    ++s_actions;
}
static void contact_and_focus(lv_obj_t *primary)
{
    s_case = "kit-contact-and-focus";
    check(primary != NULL, "missing kit Play action");
    if (!primary) return;
    unsigned before = s_actions;
    lv_obj_send_event(primary, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(primary, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_send_event(primary, LV_EVENT_CLICKED, NULL);
    check(s_actions == before, "cancelled picker press launched a game");
    lv_obj_send_event(primary, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(primary, LV_EVENT_CLICKED, NULL);
    lv_obj_send_event(primary, LV_EVENT_RELEASED, NULL);
    check(s_actions == before + 1, "fresh picker press did not activate");
    lv_obj_t *disc = orb_kit_disc_create(lv_screen_active(), ORB_KIT_DISC_STANDARD,
        ORB_KIT_NEUTRAL_GLASS, ORB_KIT_PLAY, ORB_ICON_GAME_CONTROLLER);
    lv_obj_set_pos(disc, 193, 180);
    settle();
    lv_area_t before_box, after_box;
    lv_obj_get_coords(disc, &before_box);
    orb_kit_disc_set_focus(disc, true);
    lv_obj_update_layout(disc);
    lv_obj_get_coords(disc, &after_box);
    check(!memcmp(&before_box, &after_box, sizeof(before_box)), "focus scaled or moved an 80px disc");
    check(lv_obj_get_style_outline_width(disc, 0) == ORB_KIT_FOCUS_WIDTH &&
        lv_obj_get_style_outline_pad(disc, 0) == ORB_KIT_FOCUS_OFFSET,
        "focus lost the kit ring/gap");
    lv_obj_delete(disc);
}
static void inset_contract(void)
{
    /* Pop is the owned full-rim timer. Also verify the public inset primitive
     * directly, so a kit regression cannot make the ring/title cross again. */
    s_case = "inset-title-contract";
    lv_obj_t *ring = orb_kit_ring_create(lv_screen_active(), 213, 14, ORB_KIT_DATE);
    lv_obj_t *title = orb_kit_rim_title_inset_create(lv_screen_active(), "POP", ORB_KIT_PLAY);
    orb_kit_rim_text_info_t info;
    check(orb_kit_rim_text_info(title, &info) && !info.ellipsised, "inset title metadata missing");
    check(ORB_KIT_TITLE_INSET_RADIUS == 176, "inset title baseline changed");
    check(orb_kit_rim_title_set_inset(title, true), "idempotent inset title request failed");
    settle();
    sim_display_rgb(s_pixels);
    unsigned ink_pixels = 0;
    for (unsigned y = 0; y < SIM_D; ++y) for (unsigned x = 0; x < SIM_D; ++x) {
        const uint8_t *pixel = &s_pixels[(y * SIM_D + x) * 3];
        if (pixel[1] <= 20 || pixel[1] < pixel[0] * 1.3f || pixel[1] < pixel[2] * 1.3f) continue;
        ++ink_pixels;
        check(hypotf((float)x - 233, (float)y - 233) < 206, "inset title ink crosses inner rim edge");
    }
    check(ink_pixels > 20, "inset title rendered no measurable ink");
    lv_obj_delete(title); lv_obj_delete(ring);
}
static void pickers(void)
{
    static const char *const progress[ORB_GAME_COUNT] = {
        "Stage 3 of 3 · 12 keepsakes", "Settle into the time", "Best 4294967295", "Best 4294967295", "Best 4294967295",
    };
    orb_game_play_view_t *view = orb_game_play_view_create(lv_screen_active(), picker_action, NULL, progress);
    check(view != NULL, "picker creation failed");
    if (!view) return;
    for (orb_game_kind_t kind = 0; kind < ORB_GAME_COUNT; ++kind) {
        orb_game_play_view_select(view, kind);
        char name[100];
        snprintf(name, sizeof(name), "picker-%s-long-progress", orb_game_id(kind));
        shot(name, "2.0/Play", 1);
        check(orb_game_play_view_selected(view) == kind, "selection changed during draw");
    }
    contact_and_focus(find_caption(lv_screen_active(), "Play"));
    orb_game_play_view_destroy(view);
    view = orb_game_play_view_create(lv_screen_active(), NULL, NULL, NULL);
    shot("picker-unknown-progress", "2.0/Play", 1);
    orb_game_play_view_destroy(view);
}
static void games(void)
{
    static const char *const references[ORB_GAME_COUNT] = {
        "1.0/Marble", "1.0/Beads", "1.0/Orbit", "1.0/RingSnake", "1.0/Pop",
    };
    const char *scenes[] = {"tutorial", "play", "touch-controls", "pause", "pause-save-error",
        "motion-unavailable", "calibrating", "max-score", "game-over", "game-over-save-error"};
    for (orb_game_kind_t kind = 0; kind < ORB_GAME_COUNT; ++kind) {
        orb_game_view_t *view = orb_game_view_create(lv_screen_active(), kind, NULL, NULL);
        check(view != NULL, "game view creation failed");
        if (!view) continue;
        for (unsigned scene = 0; scene < sizeof(scenes) / sizeof(scenes[0]); ++scene) {
            /* Marble and Beads have no losing game-over scene. Their pause is
             * the round-preserving sheet and their own reward/garden is below. */
            if (scene >= 8 && (kind == ORB_GAME_MARBLE || kind == ORB_GAME_BEADS)) continue;
            orb_game_state_t state;
            orb_game_init(&state, kind, 0x4f5242u);
            state.started = scene != 0;
            state.score = scene == 7 ? UINT32_MAX : 10;
            state.lives = 1;
            state.controls = scene == 2 || (scene == 5 && kind == ORB_GAME_SNAKE);
            state.user_paused = scene == 3 || scene == 4;
            state.motion_unavailable = scene == 5;
            state.touch_fallback_hint = scene == 5;
            state.tilt_calibrating = scene == 6;
            state.game_over = scene >= 8;
            if (kind == ORB_GAME_BEADS) orb_game_beads_set_time(&state, 17, 28);
            s_view = view; s_state = &state;
            s_best = scene == 7 ? UINT32_MAX : 40; s_save_error = scene == 4 || scene == 9;
            orb_game_view_update(view, &state, s_best, false, s_save_error);
            char name[100];
            snprintf(name, sizeof(name), "%s-%s", orb_game_id(kind), scenes[scene]);
            unsigned primaries = scene == 0 || scene == 3 || scene == 4 || scene >= 8 ? 1 : 0;
            shot(name, scene >= 8 ? "2.0/GameOver" : references[kind], primaries);
        }
        if (kind == ORB_GAME_MARBLE) {
            orb_game_state_t state;
            orb_game_init(&state, kind, 0x4f5242u);
            state.game.marble.garden_open = true;
            s_state = &state; s_best = 40; s_save_error = false;
            orb_game_view_update(view, &state, 40, false, false);
            shot("marble-empty-garden", "1.0/Marble", 1);
            state.game.marble.keepsake_count = ORB_MARBLE_KEEPSAKES_MAX;
            for (unsigned i = 0; i < ORB_MARBLE_KEEPSAKES_MAX; ++i) {
                float angle = i * 2.39996323f, radius = 26 + 22 * sqrtf(i);
                state.game.marble.keepsakes[i] = (orb_marble_keepsake_t){
                    .kind=i % 3, .stage=1 + i % 3,
                    .x=(int16_t)lroundf(cosf(angle) * radius), .y=(int16_t)lroundf(sinf(angle) * radius)};
            }
            orb_game_view_update(view, &state, 40, false, true);
            s_save_error = true;
            shot("marble-full-garden-save-error", "1.0/Marble", 1);
            orb_game_init(&state, kind, 0x4f5242u);
            state.started = true;
            state.game.marble.stage_cleared = true;
            state.game.marble.keepsake_count = 1;
            state.game.marble.keepsakes[0] = (orb_marble_keepsake_t){.kind=1, .stage=1};
            s_best = 40; s_save_error = false;
            orb_game_view_update(view, &state, s_best, false, false);
            shot_now("marble-clear-bloom", "1.0/Marble", 0);
            shot("marble-clear-planted", "1.0/Marble", 1);
            check(find_caption(lv_screen_active(), "Next stage") != NULL, "planted stage lost Next stage");
            state.game.marble.stage_cleared = false;
            orb_game_view_update(view, &state, s_best, false, false);
            state.game.marble.stage = ORB_MARBLE_STAGES - 1;
            state.game.marble.stage_cleared = true;
            orb_game_view_update(view, &state, s_best, false, false);
            shot_now("marble-final-bloom", "1.0/Marble", 0);
            shot("marble-final-finish", "1.0/Marble", 1);
            check(find_caption(lv_screen_active(), "Finish") != NULL, "final planted stage lost Finish");
            state.game.marble.stage_cleared = false;
            state.game_over = true;
            state.score = 300;
            orb_game_view_update(view, &state, s_best, false, false);
            shot("marble-round-receipt", "2.0/GameOver", 1);
            check(find_caption(lv_screen_active(), "300") != NULL,
                  "Marble result loses the shared score hero");
            s_save_error = true;
            orb_game_view_update(view, &state, s_best, false, true);
            shot("marble-round-receipt-save-error", "2.0/GameOver", 1);
        } else if (kind == ORB_GAME_BEADS) {
            orb_game_state_t state;
            orb_game_init(&state, kind, 0x4f5242u);
            state.started = true;
            s_state = &state; s_best = 0; s_save_error = false;
            orb_game_view_update(view, &state, s_best, false, false);
            shot("beads-time-unavailable", "1.0/Beads", 0);
            /* The condition takes the top rim (review #9/#35), in title caps. */
            check(find_rim(lv_screen_active(), "TIME NOT SET") != NULL,
                  "Beads invents wall-clock time while unavailable");
        } else {
            orb_game_state_t state;
            orb_game_init(&state, kind, 0x4f5242u);
            state.started = true;
            state.score = 999999;
            s_state = &state; s_best = 999999; s_save_error = false;
            orb_game_view_update(view, &state, s_best, false, false);
            char name[100];
            snprintf(name, sizeof(name), "%s-six-digit-play", orb_game_id(kind));
            shot(name, references[kind], 0);
            state.game_over = true;
            orb_game_view_update(view, &state, s_best, false, false);
            snprintf(name, sizeof(name), "%s-six-digit-game-over", orb_game_id(kind));
            shot(name, "2.0/GameOver", 1);
            check(find_caption(lv_screen_active(), "999999") != NULL,
                  "six-digit game-over hero loses its exact score");
        }
        s_view = NULL; s_state = NULL;
        orb_game_view_destroy(view);
    }
}
int main(int argc, char **argv)
{
    if (argc != 3) return EXIT_FAILURE;
    s_directory = argv[1];
    s_case = "initialization";
    bool ax = strcmp(argv[2], "default") != 0, bold = !strcmp(argv[2], "ax3-bold-rt");
    lv_init(); lv_tick_set_cb(tick);
    s_display = sim_display_create();
    if (!s_display) return EXIT_FAILURE;
    orb_ui_init(NULL);
    if (orb_fonts_set_content_size(ax ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) != ORB_FONTS_OK ||
        orb_fonts_set_bold_text(bold) != ORB_FONTS_OK) return EXIT_FAILURE;
    orb_ui_set_reduce_transparency(bold);
    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.tsv", s_directory);
    s_manifest = fopen(manifest_path, "w");
    if (!s_manifest) return EXIT_FAILURE;
    pickers(); inset_contract(); games();
    fclose(s_manifest);
    printf("games layout %s: %u checks, %u failures\n", argv[2], s_checks, s_failures);
    return s_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
