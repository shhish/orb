#include "orb_game_play.h"
#include "orb_kit.h"
#include "orb_sound.h"
#ifdef ESP_PLATFORM
#include "orb_memory.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAU 6.28318530718f
#define PICKER_CX 233
#define PICKER_CY 172
#define PICKER_SCALE 0.39f

void orb_game_preview(orb_game_state_t *state, orb_game_kind_t kind)
{
    orb_game_init(state, kind, 0x6f726231u);
    if (!state || kind != ORB_GAME_SNAKE) return;
    /* A short real turn (grace intact) reads as a snake, not specks. */
    for (unsigned i = 0; i < 40; ++i) orb_game_step(state, -0.5f, 0, true, false);
}

struct orb_game_play_view {
    lv_obj_t *root, *art[3], *title, *progress, *play, *dots;
    /* Only the centred game is drawn as an arena (neighbours are glyphs), so
     * hold one miniature. Five full states and sprite sets exceeded the 64 KB
     * games budget on the board and the picker drew nothing. */
    orb_game_state_t miniature;
    orb_game_sprite_t sprites[ORB_GAME_MAX_SPRITES];
    size_t sprite_count;
    char progress_text[ORB_GAME_COUNT][48];
    orb_game_play_action_cb_t callback;
    void *context;
    unsigned index;
    bool system_input;
    bool (*input_allowed)(void *context);
};

/* It shares the 160 KB games budget with a running game beneath it. */
_Static_assert(sizeof(struct orb_game_play_view) <= 48 * 1024, "Play picker outgrew its games budget share");

static void deleted(lv_event_t *e);

/* --- minimal content drawing, mirroring orb_game_view's flat shapes ---- */

static void disc(lv_layer_t *layer, int x, int y, int r, uint32_t colour,
                 uint8_t opacity, int border)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = lv_color_hex(colour);
    d.bg_opa = border ? LV_OPA_TRANSP : opacity;
    d.border_color = lv_color_hex(colour);
    d.border_opa = opacity;
    d.border_width = border;
    lv_area_t a = {x - r, y - r, x + r, y + r};
    lv_draw_rect(layer, &d, &a);
}

static void arc(lv_layer_t *layer, int x, int y, int r, int width,
                uint32_t colour, uint8_t opacity, float start, float end)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center = (lv_point_t){x, y};
    /* SVG measures the path centreline; LVGL measures the outer edge. */
    d.radius = (uint16_t)(r + width / 2);
    d.width = width;
    d.rounded = true;
    d.start_angle = (int16_t)lroundf(start * 360 / TAU);
    d.end_angle = (int16_t)lroundf(end * 360 / TAU);
    d.opa = opacity;
    d.color = lv_color_hex(colour);
    lv_draw_arc(layer, &d);
}

static void star(lv_layer_t *layer, int x, int y, int r, uint32_t colour, uint8_t opacity)
{
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.opa = opacity;
    t.color = lv_color_hex(colour);
    lv_point_precise_t v[10];
    for (int i = 0; i < 10; ++i) {
        float rr = (i % 2 == 0) ? (float)r : (float)r * 0.5f;
        float a = -TAU / 4 + (float)i * TAU / 10;
        v[i] = (lv_point_precise_t){x + cosf(a) * rr, y + sinf(a) * rr};
    }
    for (int i = 0; i < 10; ++i) {
        t.p[0] = (lv_point_precise_t){x, y};
        t.p[1] = v[i];
        t.p[2] = v[(i + 1) % 10];
        lv_draw_triangle(layer, &t);
    }
}

static void draw_miniature(lv_layer_t *layer, const orb_game_state_t *mini,
                           const orb_game_sprite_t *sprites, size_t count,
                           int cx, int cy, float scale)
{
    /* Static arena content, scaled into the picker's 180 px disc. */
    switch (mini->kind) {
    case ORB_GAME_MARBLE:
        /* Design 1.0's still is the garden with both gate openings visible;
         * it is content art, not an extra set of full circles over the gates. */
        arc(layer, cx, cy, (int)lroundf(188 * scale), (int)lroundf(22 * scale),
            0x48484a, 255, TAU * 320 / 360, TAU * 640 / 360);
        arc(layer, cx, cy, (int)lroundf(126 * scale), (int)lroundf(22 * scale),
            0x48484a, 255, TAU * 175 / 360, TAU * 485 / 360);
        disc(layer, cx + (int)lroundf((41 - 233) * scale),
             cy + (int)lroundf((303 - 233) * scale), (int)lroundf(22 * scale),
             0xffffff, 255, 0);
        static const int petals[5][2] = {{233,215}, {250,228}, {244,249},
            {222,249}, {216,228}};
        for (unsigned i = 0; i < 5; ++i)
            disc(layer, cx + (int)lroundf((petals[i][0] - 233) * scale),
                 cy + (int)lroundf((petals[i][1] - 233) * scale),
                 (int)lroundf(18 * scale), 0x30d158, 255, 0);
        disc(layer, cx, cy, (int)lroundf(10 * scale), 0xffffff, 255, 0);
        return;
    case ORB_GAME_ORBIT:
        /* Gray3: a 1 px lane must stay visible on the art disc's Gray6. */
        disc(layer, cx, cy, (int)lroundf(ORB_GAME_ORBIT_R0 * scale), 0x48484a, 255, 1);
        disc(layer, cx, cy, (int)lroundf(ORB_GAME_ORBIT_R1 * scale), 0x48484a, 255, 1);
        disc(layer, cx, cy, (int)lroundf(ORB_GAME_ORBIT_R2 * scale), 0x48484a, 255, 1);
        disc(layer, cx, cy, (int)lroundf(ORB_GAME_ORBIT_SUN * scale), 0xffffff, 255, 0);
        break;
    default:
        break;
    }
    for (size_t i = 0; i < count; ++i) {
        const orb_game_sprite_t *s = &sprites[i];
        if (!s->radius || !s->opacity) continue;
        int x = cx + (int)lroundf((s->x - ORB_GAME_CX) * scale);
        int y = cy + (int)lroundf((s->y - ORB_GAME_CY) * scale);
        int r = (int)lroundf((float)s->radius * scale);
        if (r < 1) r = 1;
        switch (s->shape) {
        case ORB_GAME_GATE:
            arc(layer, x, y, (int)lroundf(s->arc_radius * scale), r * 2,
                s->colour, s->opacity, s->arc_start, s->arc_end);
            break;
        case ORB_GAME_STAR:
            star(layer, x, y, r, s->colour, s->opacity);
            break;
        case ORB_GAME_FLOWER: {
            disc(layer, x, y, r, s->colour, s->opacity, 0);
            break;
        }
        case ORB_GAME_RING:
            disc(layer, x, y, r, s->colour, s->opacity, 1);
            break;
        case ORB_GAME_BUBBLE:
            /* Review #1/#33: the same flat translucent body and rim as play,
             * not a solid yellow coin. */
            disc(layer, x, y, r, s->colour, (uint8_t)(s->opacity * 31 / 255), 0);
            disc(layer, x, y, r, s->colour, s->opacity, 1);
            break;
        default:
            disc(layer, x, y, r, s->colour, s->opacity, 0);
            break;
        }
    }
}

static void draw_peek(lv_layer_t *layer, orb_game_kind_t kind, int x, int y)
{
    /* HIG §§1/3: a restrained identity glyph peeks in from the bezel. The
     * neighbour is deliberately cropped, rather than a second full arena. */
    uint32_t accent = orb_game_accent(kind);
    if (kind == ORB_GAME_BEADS) {
        for (int i = -1; i <= 1; ++i)
            disc(layer, x + i * 12, y, 4, accent, 153, 0);
    } else if (kind == ORB_GAME_ORBIT) {
        disc(layer, x, y, 15, 0x8e8e93, 153, 1);
        disc(layer, x, y, 5, 0xffffff, 153, 0);
        disc(layer, x + 15, y, 4, accent, 153, 0);
    } else if (kind == ORB_GAME_SNAKE) {
        for (int i = -1; i <= 1; ++i)
            disc(layer, x + i * 9, y, 5, accent, 153, 0);
    } else {
        disc(layer, x, y, 13, accent, 153, 0);
    }
}

static void draw(lv_event_t *e)
{
    orb_game_play_view_t *v = lv_event_get_user_data(e);
    lv_obj_t *art = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t area;
    lv_obj_get_coords(art, &area);
    int x = (area.x1 + area.x2 + 1) / 2;
    int y = (area.y1 + area.y2 + 1) / 2;
    int direction = (int)(intptr_t)lv_obj_get_user_data(art);
    /* These circles are game-world artwork. The containing 180 px disc and
     * its press response come exclusively from the Design 2.0 kit. Drawing
     * consumes a still prepared at selection time, never model setup. */
    if (direction) {
        unsigned neighbour = (v->index + ORB_GAME_COUNT + direction) % ORB_GAME_COUNT;
        draw_peek(layer, (orb_game_kind_t)neighbour, x - direction * 60, y);
    } else {
        draw_miniature(layer, &v->miniature, v->sprites, v->sprite_count,
                       x, y, PICKER_SCALE);
    }
}

static void clicked(lv_event_t *e)
{
    orb_game_play_view_t *v = lv_event_get_user_data(e);
    if (v->input_allowed && !v->input_allowed(v->context)) return;
    if (v->callback)
        v->callback((orb_game_kind_t)v->index, v->context);
}

static void peek_clicked(lv_event_t *e)
{
    orb_game_play_view_t *v = lv_event_get_user_data(e);
    if (v->input_allowed && !v->input_allowed(v->context)) return;
    int direction = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    unsigned next = (v->index + ORB_GAME_COUNT + direction) % ORB_GAME_COUNT;
    orb_game_play_view_select(v, (orb_game_kind_t)next);
    (void)orb_sound_play(ORB_SOUND_TAP);
}

static void swipe(lv_event_t *e)
{
    orb_game_play_view_t *v = lv_event_get_user_data(e);
    if (v->system_input) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_event_get_indev(e));
    if (dir == LV_DIR_LEFT) v->index = (v->index + 1) % ORB_GAME_COUNT;
    else if (dir == LV_DIR_RIGHT) v->index = (v->index + ORB_GAME_COUNT - 1) % ORB_GAME_COUNT;
    else return;
    for (unsigned i = 0; i < 3; i++) orb_kit_press_cancel(v->art[i]);
    orb_kit_press_cancel(v->play);
    orb_game_play_view_select(v, (orb_game_kind_t)v->index);
    (void)orb_sound_play(ORB_SOUND_DETENT);
}

static void layout(orb_game_play_view_t *view)
{
    bool ax3 = orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3;
    int art_y = ax3 ? 154 : PICKER_CY;
    /* HIG §§3/4/8: keep the art at 180 px and move the stack upward for
     * AX3, rather than growing a disc or hiding part of a game name. */
    for (unsigned i = 0; i < 3; i++) lv_obj_set_y(view->art[i], art_y - ORB_KIT_ART / 2);
    int title_y = ax3 ? 252 : 272;
    lv_obj_set_pos(view->title, (ORB_SCREEN_D - lv_obj_get_width(view->title)) / 2, title_y);
    int progress_y = LV_MAX(ax3 ? 306 : 312, title_y + lv_obj_get_height(view->title) + 3);
    int height = lv_obj_get_style_text_font(view->progress, 0)->line_height;
    /* The full safe chord matters at AX3 + Bold: the real maximum Marble
     * receipt is 367 px wide, one pixel beyond the old arbitrary 366 cap. */
    int width = orb_chord_width(progress_y, progress_y + height - 1, ORB_RIM_MARGIN_TEXT);
    lv_obj_set_size(view->progress, width, height);
    lv_obj_set_pos(view->progress, (ORB_SCREEN_D - width) / 2, progress_y);
    orb_kit_place_bottom_arc(view->play);
}

static void fonts_changed(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)subject;
    layout(lv_observer_get_user_data(observer));
}

void orb_game_play_view_select(orb_game_play_view_t *view, orb_game_kind_t kind)
{
    if (!view) return;
    view->index = (unsigned)kind % ORB_GAME_COUNT;
    kind = (orb_game_kind_t)view->index;
    orb_kit_hero_fit(view->title, orb_game_name(kind), 366, 2, NULL);
    lv_label_set_text(view->progress, view->progress_text[view->index]);
    lv_obj_set_hidden(view->progress, !view->progress_text[view->index][0]);
    /* Prepare the one retained arena while idle. Selecting with the rim may
     * update content, but no allocation or game initialization runs in draw. */
    orb_game_preview(&view->miniature, kind);
    view->sprite_count = orb_game_sprites(&view->miniature, view->sprites, ORB_GAME_MAX_SPRITES);
    orb_kit_page_dots_set_active(view->dots, view->index, ORB_KIT_PLAY);
    layout(view);
    for (unsigned i = 0; i < 3; i++) lv_obj_invalidate(view->art[i]);
}

orb_game_play_view_t *orb_game_play_view_create(lv_obj_t *parent,
                                                orb_game_play_action_cb_t callback,
                                                void *context,
                                                const char *const progress[ORB_GAME_COUNT])
{
    if (!parent) return NULL;
#ifdef ESP_PLATFORM
    orb_game_play_view_t *v = orb_memory_calloc(ORB_MEMORY_GAMES, 1, sizeof(*v));
#else
    orb_game_play_view_t *v = calloc(1, sizeof(*v));
#endif
    if (!v) return NULL;
    v->callback = callback;
    v->context = context;
    v->root = lv_obj_create(parent);
    lv_obj_remove_style_all(v->root);
    lv_obj_add_event_cb(v->root, deleted, LV_EVENT_DELETE, v);
    lv_obj_set_size(v->root, 466, 466);
    lv_obj_set_style_bg_color(v->root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(v->root, LV_OPA_COVER, 0);
    lv_obj_set_clickable(v->root, true);
    lv_obj_set_scrollable(v->root, false);
    lv_obj_set_event_bubble(v->root, true);
    lv_obj_add_event_cb(v->root, swipe, LV_EVENT_GESTURE, v);

    for (unsigned i = 0; i < ORB_GAME_COUNT; ++i) {
        snprintf(v->progress_text[i], sizeof(v->progress_text[i]), "%s",
                 progress && progress[i] ? progress[i] : "");
    }

    orb_kit_rim_title_create(v->root, "PLAY", ORB_KIT_PLAY);
    /* Art is content, not glass. All three use the same kit role even though
     * the neighbour centres intentionally sit beyond the bezel (HIG §1). */
    static const int centres[] = {-30, PICKER_CX, 496};
    for (unsigned i = 0; i < 3; i++) {
        int direction = (int)i - 1;
        lv_obj_t *art = orb_kit_disc_create(v->root, ORB_KIT_DISC_ART,
            ORB_KIT_ACCENT_FILL, ORB_GRAY6, NULL);
        v->art[i] = art;
        lv_obj_set_pos(art, centres[i] - ORB_KIT_ART / 2, PICKER_CY - ORB_KIT_ART / 2);
        lv_obj_set_event_bubble(art, true);
        lv_obj_set_user_data(art, (void *)(intptr_t)direction);
        lv_obj_add_event_cb(art, draw, LV_EVENT_DRAW_MAIN, v);
        lv_obj_add_event_cb(art, direction ? peek_clicked : clicked, LV_EVENT_CLICKED, v);
    }

    v->title = lv_label_create(v->root);
    lv_obj_set_style_text_color(v->title, ORB_LABEL, 0);
    lv_obj_set_style_text_align(v->title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_clickable(v->title, false);
    v->progress = lv_label_create(v->root);
    orb_text_apply(v->progress, ORB_TEXT_FOOTNOTE2, ORB_TEXT_TABULAR, 0);
    lv_obj_set_style_text_color(v->progress, ORB_KIT_SECONDARY, 0);
    lv_obj_set_style_text_opa(v->progress, ORB_KIT_SECONDARY_OPA, 0);
    lv_obj_set_style_text_align(v->progress, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(v->progress, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_max_lines(v->progress, 1);
    lv_obj_set_clickable(v->progress, false);
    v->play = orb_kit_button_primary_create(v->root, "Play", ORB_KIT_PLAY);
    lv_obj_set_event_bubble(v->play, true);
    lv_obj_add_event_cb(v->play, clicked, LV_EVENT_CLICKED, v);
    v->dots = orb_kit_page_dots_create(v->root, ORB_GAME_COUNT);
    /* Review #1: the kit's r186 row sat 6 px under the Play capsule. On the
     * rim at r214 it clears Play by 20 px; five dots stay concentric to
     * within 0.3 px of the translated arc. */
    lv_obj_align(v->dots, LV_ALIGN_CENTER, 0, 214 - 186);
    orb_game_play_view_select(v, ORB_GAME_MARBLE);
    lv_subject_add_observer_obj(orb_fonts_subject(), fonts_changed, v->root, v);
    return v;
}

static void deleted(lv_event_t *e)
{
#ifdef ESP_PLATFORM
    orb_memory_free(lv_event_get_user_data(e));
#else
    free(lv_event_get_user_data(e));
#endif
}

void orb_game_play_view_destroy(orb_game_play_view_t *view)
{
    if (view) lv_obj_delete(view->root);
}

void orb_game_play_view_set_system_input(orb_game_play_view_t *view, bool enabled)
{
    if (view) view->system_input = enabled;
}

orb_game_kind_t orb_game_play_view_selected(const orb_game_play_view_t *view)
{
    return view ? (orb_game_kind_t)view->index : ORB_GAME_MARBLE;
}

void orb_game_play_view_set_input_guard(orb_game_play_view_t *view,
                                         bool (*allowed)(void *context))
{
    if (view) view->input_allowed = allowed;
}
