/* Reading-surface templates shared by every game (polish review #28–#40):
 * Start, Pause, Result, Garden and the in-play overlay. One layout per
 * state, so each fix lands in all five games. */
#include "orb_game_view_private.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void game_compact(uint32_t value, char *out, size_t capacity)
{
    if (value < 1000) snprintf(out, capacity, "%lu", (unsigned long)value);
    else if (value < 1000000) snprintf(out, capacity, "%luk", (unsigned long)(value / 1000));
    else if (value < 1000000000) snprintf(out, capacity, "%lum", (unsigned long)(value / 1000000));
    else snprintf(out, capacity, "%lub", (unsigned long)(value / 1000000000));
}

void game_visible(lv_obj_t *object, bool show)
{
    lv_obj_set_hidden(object, !show);
}

void game_centre_text(lv_obj_t *label, int y)
{
    lv_obj_update_layout(label);
    lv_obj_set_y(label, y - lv_obj_get_height(label) / 2);
}

bool game_tilt(orb_game_kind_t kind)
{
    return kind == ORB_GAME_MARBLE || kind == ORB_GAME_BEADS || kind == ORB_GAME_SNAKE;
}

static const orb_text_style_t s_hint_styles[] = {ORB_TEXT_BODY, ORB_TEXT_FOOTNOTE1, ORB_TEXT_FOOTNOTE2};
/* Reading-surface status lines stay quiet beneath the rim title, which keeps
 * five-item Pause stacks inside the circle at AX3 (review #28/#37). */
static const orb_text_style_t s_status_styles[] = {ORB_TEXT_FOOTNOTE1, ORB_TEXT_FOOTNOTE2};
static const orb_text_style_t s_tutorial_styles[] = {ORB_TEXT_TITLE3, ORB_TEXT_BODY, ORB_TEXT_FOOTNOTE1};

static int text_width(lv_obj_t *label, const char *value)
{
    lv_point_t size;
    lv_text_get_size(&size, value, lv_obj_get_style_text_font(label, 0),
        lv_obj_get_style_text_letter_space(label, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

/* Review #37: one line in the largest listed style that fits, so copy never
 * leaves a one-word widow; only the smallest style may wrap, never clip or
 * ellipsise (HIG §4). Bold Text and Dynamic Type follow orb_text_apply. */
static void fit_line(lv_obj_t *label, const char *value, int width,
                     const orb_text_style_t *styles, unsigned count)
{
    lv_obj_set_width(label, width);
    for (unsigned i = 0; i < count; ++i) {
        orb_text_apply(label, styles[i], ORB_TEXT_REGULAR, 0);
        if (text_width(label, value) <= width) break;
    }
    lv_label_set_text_static(label, value);
    lv_obj_set_style_text_color(label, ORB_KIT_SECONDARY, 0);
    lv_obj_set_style_text_opa(label, ORB_KIT_SECONDARY_OPA, 0);
    lv_obj_update_layout(label);
}

/* A secondary line centred on y, chord-fitted with the text rim margin. */
static int line_at(lv_obj_t *label, const char *value, int y,
                   const orb_text_style_t *styles, unsigned count)
{
    int width = orb_chord_width(y - 22, y + 22, ORB_RIM_MARGIN_TEXT);
    fit_line(label, value, width, styles, count);
    lv_obj_set_x(label, 233 - width / 2);
    game_centre_text(label, y);
    return lv_obj_get_height(label);
}

/* Review #7/#37: the first top line that keeps a centred text box of this
 * ink width below the visible rim title (its ink lies outside r200 within
 * the title's own angle). Measured from the kit's fitted title, so AX3 and
 * Bold widen the clearance exactly when the title grows. */
static int title_floor(const orb_game_view_t *v, int width)
{
    orb_kit_rim_text_info_t info;
    if (lv_obj_is_hidden(v->title) || !orb_kit_rim_text_info(v->title, &info)) return 0;
    float half = (info.angle_degrees * .5f + 3) * TAU / 360;
    for (int y = 0; y < 233; ++y) {
        float dy = 233.0f - (float)y, reach = fminf(width * .5f, dy * tanf(half));
        if (reach * reach + dy * dy <= TITLE_CLEAR * TITLE_CLEAR) return y;
    }
    return 233;
}

/* The rim title is rebuilt (an idle raster) only when its words change. */
static void set_title(orb_game_view_t *v, const char *main, const char *suffix)
{
    if (!strcmp(v->title_main, main) && !strcmp(v->title_suffix, suffix)) return;
    snprintf(v->title_main, sizeof(v->title_main), "%s", main);
    snprintf(v->title_suffix, sizeof(v->title_suffix), "%s", suffix);
    orb_kit_rim_text_set(v->title, v->title_main, v->title_suffix);
}

/* Review #9/#16/#35: explicit input and sensor conditions use the same copy
 * everywhere, including touch-first games. Their tap rules do not change. */
static const char *condition(const orb_game_view_t *v, const orb_game_state_t *state)
{
    if (state->motion_unavailable) return "Motion unavailable";
    if (state->controls) return "Touch controls on";
    if (v->kind == ORB_GAME_BEADS && !state->game.beads.time_valid) return "Time not set";
    return NULL;
}

static const char *place_name(orb_game_kind_t kind)
{
    switch (kind) {
    case ORB_GAME_MARBLE: return "Marble";
    case ORB_GAME_BEADS: return "Beads";
    case ORB_GAME_ORBIT: return "Orbit";
    case ORB_GAME_SNAKE: return "Snake";
    default: return "Pop";
    }
}

/* One in-play overlay for all games (HIG §§1/8). Calibration is a frozen
 * reading state: the arena hides and "Hold still" is centred under the
 * game's name. A condition (motion, touch, time) shows for three seconds with
 * no matte box, then lives on Pause. Its reserved band is
 * the top rim, in the game's title ink where every reading surface puts its
 * title: outside Marble's
 * outer wall (r194) and above Beads' resting pool. Snake's lethal wall runs
 * through that band, so its line sits below the centre-up start instead,
 * where its first-second teaching already reads. Marble's rim otherwise
 * carries its stage and score HUD. */
void game_play_overlay(orb_game_view_t *v, const orb_game_state_t *state, uint32_t now, bool force)
{
    bool playing = v->scene == SCENE_PLAY;
    bool calibrating = playing && game_tilt(v->kind) && state->tilt_calibrating;
    const char *status = playing ? condition(v, state) : NULL;
    if (status != v->status || (force && playing)) v->status_at = now;
    v->status = status;
    bool shown = status && now - v->status_at < STATUS_MS;
    if (!playing) return;
    bool rim_status = shown && v->kind != ORB_GAME_SNAKE;
    if (v->kind == ORB_GAME_POP) {
        /* The countdown owns the outer rim. A brief condition replaces the
         * score, like Marble's status/HUD lane, rather than painting a caption
         * over tappable bubbles or hiding words behind the score disc. */
        orb_kit_rim_title_set_inset(v->title, rim_status);
        game_visible(v->score_hit, !shown);
        lv_obj_set_clickable(v->score_hit, !shown);
    }
    if (calibrating) set_title(v, place_name(v->kind), "");
    else if (rim_status) set_title(v, status, "");
    else if (v->kind == ORB_GAME_MARBLE) {
        /* One colour lane: a suffix lane would add and drop arc widgets as
         * scenes change, and frames must never create or delete objects. */
        char hud[40], score[16];
        game_compact(state->score, score, sizeof(score));
        snprintf(hud, sizeof(hud), "Stage %u of %u · %s", (unsigned)state->game.marble.stage + 1u,
                 (unsigned)ORB_MARBLE_STAGES, score);
        set_title(v, hud, "");
    }
    game_visible(v->title, calibrating || rim_status || v->kind == ORB_GAME_MARBLE);
    game_visible(v->canvas, !calibrating);
    bool field_line = !calibrating && v->kind == ORB_GAME_SNAKE &&
        (shown || state->ticks < ORB_GAME_SNAKE_GRACE);
    const char *line = calibrating ? "Hold still" : !field_line ? NULL : shown ? status :
        state->controls ? "Hold to steer" : "Tilt to steer";
    if (line && strcmp(lv_label_get_text(v->hud_title), line))
        line_at(v->hud_title, line, calibrating ? 233 : 384, s_hint_styles, 3);
    game_visible(v->hud_title, line != NULL);
}

lv_color_t game_accent(orb_game_kind_t kind)
{
    switch (kind) {
    /* Review #40: system indigo on its own dark tint is about 3:1; the
     * increased-contrast indigo keeps Orbit's identity at 4.5:1+ (HIG §5). */
    case ORB_GAME_ORBIT: return ORB_KIT_DOT_CONTRAST;
    case ORB_GAME_SNAKE: return ORB_KIT_CLAUDE;
    case ORB_GAME_POP: return ORB_KIT_HOME;
    case ORB_GAME_BEADS: return ORB_KIT_SOUND;
    default: return ORB_KIT_PLAY;
    }
}

void game_place(lv_obj_t *obj, int x, int y)
{
    lv_obj_update_layout(obj);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, x - 233, y - lv_obj_get_height(obj) / 2);
}

/* HIG §§3/4/8: the only reading layouts. All line boxes are inside the
 * circle and every action retains its kit role through Dynamic Type changes. */
static void show_control(lv_obj_t *obj, bool show)
{
    if (!obj) return;
    if (!show) {
        orb_kit_press_cancel(obj);
        orb_glass_set_amount(obj, 0);
        lv_obj_set_hidden(obj, true);
        lv_obj_set_clickable(obj, false);
    } else if (lv_obj_is_hidden(obj)) {
        lv_obj_set_clickable(obj, true);
        if (!orb_glass_materialise(obj, true)) lv_obj_set_hidden(obj, false);
    }
}

/* The start line teaches the input this round will actually use. */
static const char *tutorial(orb_game_kind_t kind, bool touch)
{
    switch (kind) {
    case ORB_GAME_MARBLE: return touch ? "Tap a side to roll" : "Tilt to roll";
    case ORB_GAME_BEADS: return touch ? "Hold to pour" : "Tilt to pour";
    case ORB_GAME_ORBIT: return "Tap to change orbit";
    case ORB_GAME_SNAKE: return touch ? "Hold to steer" : "Tilt to steer";
    default: return "Tap the bubbles";
    }
}

/* Pause's status line: one phrase per condition (glossary §2), else the
 * game's reminder. A save error takes the same slot (review #30). */
static const char *pause_copy(const orb_game_view_t *v, const orb_game_state_t *state, bool save_error)
{
    if (save_error) return "Not saved";
    if (state->motion_unavailable) return "Motion unavailable";
    if (v->kind == ORB_GAME_BEADS && !state->game.beads.time_valid) return "Time not set";
    if (v->kind == ORB_GAME_MARBLE) return "Turn rim to rewind";
    if (v->kind == ORB_GAME_BEADS) return state->controls ? "Hold to pour" : "Rest for time";
    return "Your round is kept";
}

static void stack_place(lv_obj_t *obj, int *y)
{
    lv_obj_update_layout(obj);
    int height = lv_obj_get_height(obj);
    game_place(obj, 233, *y + height / 2);
    *y += height + CONTROL_GAP;
}

/* Review #28: one Pause for every game, top to bottom: the status line,
 * Resume (the one tinted action; HIG §7 puts the likeliest choice first),
 * End round, the game's input row, then Exit last. Beads is an open-ended
 * toy and omits End round. Garden lives on Marble's Start/Result toolbar,
 * keeping Pause to three glass controls. */
static void pause_layout(orb_game_view_t *v, const char *copy, lv_obj_t *secondary)
{
    /* A condition replaces the hint without moving any action. Reserve the
     * same single-line style/height and title clearance for every phrase. */
    int status = line_at(v->detail, copy, 120, s_status_styles, 2);
    lv_obj_t *controls[] = {v->primary, secondary, v->done};
    int total = status + 16 + (v->settings_list ? ORB_KIT_ROW_H + CONTROL_GAP : 0) - CONTROL_GAP;
    for (unsigned i = 0; i < 3; ++i) {
        if (!controls[i]) continue;
        lv_obj_update_layout(controls[i]);
        total += lv_obj_get_height(controls[i]) + CONTROL_GAP;
    }
    int y = 233 - total / 2 + 8;
    y = LV_MAX(y, title_floor(v, 346));
    game_centre_text(v->detail, y + status / 2);
    y += status + 16;
    stack_place(v->primary, &y);
    if (secondary) stack_place(secondary, &y);
    if (v->settings_list) {
        lv_obj_set_y(v->settings_list, y);
        /* This single fixed option is native content, not a scrolling list.
         * Fit its capsule to the actual chord without shoulder scaling the
         * label into the switch at AX3 (HIG §§3/4). */
        float dy = y + ORB_KIT_ROW_H / 2 - 233;
        float inner = 233 - ORB_RIM_MARGIN_GLASS - ORB_KIT_ROW_H / 2;
        int width = ORB_KIT_ROW_H + 2 * (int)sqrtf(fmaxf(0, inner * inner - dy * dy));
        lv_obj_set_size(v->settings, width, ORB_KIT_ROW_H);
        lv_obj_set_x(v->settings, 233 - width / 2);
        orb_text_apply(v->settings_label, ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, 0);
        lv_obj_set_width(v->settings_label, width - 172);
        lv_obj_align(v->settings_label, LV_ALIGN_LEFT_MID, 64, 0);
        lv_obj_align(v->settings_icon, LV_ALIGN_LEFT_MID, 16, 0);
        lv_obj_align(v->controls_track, LV_ALIGN_RIGHT_MID, -16, 0);
        y += ORB_KIT_ROW_H + CONTROL_GAP;
    }
    stack_place(v->done, &y);
}

/* A secondary line below the rim title: centred at y unless the title's
 * own arc needs it lower. */
static void line_below_title(orb_game_view_t *v, lv_obj_t *label, const char *copy, int y)
{
    int height = line_at(label, copy, y, s_status_styles, 2);
    int floor = title_floor(v, text_width(label, copy));
    if (y - height / 2 < floor) game_centre_text(label, floor + height / 2);
}

static void hero(orb_game_view_t *v, uint32_t score)
{
    /* Review #29: every result leads with its number, Marble included. */
    if (score < 1000000) snprintf(v->big_score_text, sizeof(v->big_score_text), "%lu", (unsigned long)score);
    else game_compact(score, v->big_score_text, sizeof(v->big_score_text));
    if (score < 1000) orb_clock_apply(v->big_score, ORB_CLOCK_MEDIUM, 0);
    else orb_text_apply(v->big_score, ORB_TEXT_TITLE1, ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR, 0);
    lv_label_set_text_static(v->big_score, v->big_score_text);
    game_centre_text(v->big_score, HERO_Y);
}

void game_chrome(orb_game_view_t *v, const orb_game_state_t *state, bool save_error)
{
    bool playing = v->scene == SCENE_PLAY, paused = v->scene == SCENE_PAUSE;
    bool start = v->scene == SCENE_START, end = v->scene == SCENE_END;
    bool marble = v->kind == ORB_GAME_MARBLE;
    bool garden = v->scene == SCENE_GARDEN, clear = v->scene == SCENE_CLEAR;
    bool settled = !clear || v->reward_settled;
    /* Orbit, Pop and Snake carry a live score disc and can end a round. */
    bool disc = v->kind == ORB_GAME_ORBIT || v->kind == ORB_GAME_POP || v->kind == ORB_GAME_SNAKE;
    /* Fonts or scene changed: the in-play line refits on its next pass. */
    lv_label_set_text_static(v->hud_title, "");
    if (!playing) {
        if (v->kind == ORB_GAME_POP) orb_kit_rim_title_set_inset(v->title, false);
        char title[48];
        bool complete = (v->kind == ORB_GAME_SNAKE && state->game.snake.won) ||
            (marble && state->game.marble.stages_cleared >= ORB_MARBLE_STAGES);
        /* Results lead with the shared state, leaving the diagonal toolbar
         * clear even at AX3. Identity was established by Start and play. */
        snprintf(title, sizeof(title), "%s%s", end ? "" : place_name(v->kind),
            end ? (complete ? "Complete" : "Round over") : paused ? " · Paused" :
            garden ? " · Garden" : clear ? " · Planted" : "");
        set_title(v, title, "");
        game_visible(v->title, settled);
        game_visible(v->canvas, true);
        game_visible(v->hud_title, false);
    }
    game_visible(v->art, start);
    /* Review #31: one leave term per role. Exit abandons a paused round,
     * Done acknowledges a result or closes the Garden sheet, X closes Start. */
    const char *primary = paused ? "Resume" : garden ? "Done" : clear ?
        (state->game.marble.stage + 1u == ORB_MARBLE_STAGES ? "Finish" : "Next stage") : "Play";
    int primary_action = paused ? ORB_GAME_PAUSE : garden ? ORB_GAME_GARDEN :
        clear ? ORB_GAME_PRIMARY : ORB_GAME_START;
    game_action_content(v, &v->primary, primary, primary_action, true);
    game_action_content(v, &v->done, paused ? "Exit" : "Done", ORB_GAME_CLOSE, false);
    show_control(v->close, start);
    show_control(v->primary, (start || paused || garden || clear) && settled);
    show_control(v->replay, end);
    show_control(v->done, paused || end);
    show_control(v->finish, paused && v->kind != ORB_GAME_BEADS);
    show_control(v->garden, marble && (start || end));
    if (v->settings_list) {
        game_visible(v->settings_list, paused);
        orb_kit_switch_set_checked(v->controls_track, state->controls, false);
    }
    const char *copy = "";
    bool empty_garden = garden && !state->game.marble.keepsake_count;
    if (start) {
        copy = tutorial(v->kind, state->controls);
        line_at(v->detail, copy, TUTORIAL_Y, s_tutorial_styles, 3);
        game_place(v->primary, 233, START_PRIMARY_Y);
    } else if (paused) {
        copy = pause_copy(v, state, save_error);
        pause_layout(v, copy, v->kind != ORB_GAME_BEADS ? v->finish : NULL);
    } else if (clear) {
        copy = "A keepsake for you";
        line_below_title(v, v->detail, copy, 190);
        game_place(v->primary, 233, RESULT_DONE_Y);
    } else if (garden) {
        if (save_error) copy = "Not saved";
        else if (empty_garden) copy = "No keepsakes yet";
        else {
            snprintf(v->detail_text, sizeof(v->detail_text), "%u keepsake%s",
                state->game.marble.keepsake_count, state->game.marble.keepsake_count == 1 ? "" : "s");
            copy = v->detail_text;
        }
        line_below_title(v, v->detail, copy, 128);
        if (empty_garden && !save_error) {
            /* Glossary §2.3: distinguish the empty heading from its hint. */
            orb_text_apply(v->detail, ORB_TEXT_FOOTNOTE1, ORB_TEXT_EMPHASIZED, 0);
            lv_obj_update_layout(v->detail);
            int floor = title_floor(v, text_width(v->detail, copy));
            game_centre_text(v->detail, LV_MAX(128, floor + lv_obj_get_height(v->detail) / 2));
        }
        game_place(v->primary, 233, START_PRIMARY_Y);
    }
    game_visible(v->detail, (start || paused || garden || clear) && settled && copy[0]);
    if (end) {
        char best[12];
        game_compact(v->best_value, best, sizeof(best));
        if (save_error) snprintf(v->best_text, sizeof(v->best_text), "Not saved");
        else if (marble) snprintf(v->best_text, sizeof(v->best_text), "Your garden is kept");
        else snprintf(v->best_text, sizeof(v->best_text), "Best %s", best);
        line_at(v->best, v->best_text, RESULT_LINE_Y, s_status_styles, 2);
        hero(v, state->score);
        game_place(v->replay, 233, RESULT_PRIMARY_Y);
        game_place(v->done, 233, RESULT_DONE_Y);
    } else if (empty_garden) {
        /* Glossary §2.3: the empty state names what is absent, then how to
         * fill it, in the space the planted spiral would take. */
        line_at(v->best, "Clear a stage to plant one", 233, s_status_styles, 2);
    }
    game_visible(v->best, end || empty_garden);
    game_visible(v->big_score, end);
    if (!playing || !disc) orb_kit_press_cancel(v->score_hit);
    bool hud = playing && disc && !(game_tilt(v->kind) && state->tilt_calibrating);
    game_visible(v->score_hit, hud);
    /* Snake's whole field steers; its disc only reports. Pop/Orbit tap it to pause. */
    lv_obj_set_clickable(v->score_hit, hud && v->kind != ORB_GAME_SNAKE);
    game_place(v->score_hit, 233, v->kind == ORB_GAME_ORBIT ? 233 : 233 + (int)ORB_GAME_HUD_Y);
    bool two_lines = v->kind == ORB_GAME_ORBIT || v->kind == ORB_GAME_POP;
    lv_obj_set_y(v->score_clip, two_lines ? 5 : 19);
    lv_obj_set_height(v->score_clip, 42);
    game_visible(v->hud_time, playing && v->kind == ORB_GAME_POP);
    for (unsigned i = 0; i < 3; ++i) {
        bool left = i < state->lives;
        lv_obj_set_style_bg_opa(v->life[i], left ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(v->life[i], left ? 0 : 2, 0);
        lv_obj_set_style_border_opa(v->life[i], LV_OPA_60, 0);
        game_visible(v->life[i], playing && v->kind == ORB_GAME_ORBIT);
    }
    for (unsigned i = 0; i < 4; ++i) {
        show_control(v->arrows[i], false);
    }
    game_visible(v->stage_dots, marble && playing && !state->tilt_calibrating);
    if (marble) orb_kit_page_dots_set_active(v->stage_dots, state->game.marble.stage, game_accent(v->kind));
    game_visible(v->timer_ring, playing && v->kind == ORB_GAME_POP);
}

/* Review #36/#40: the disc's second line reads the round's limit. Pop's
 * seconds label its rim countdown; Orbit's lives are three marks, filled
 * while left and outlined when spent (shape, not colour), instead of a
 * "1 left" phrase that filled the 80 px disc. */
void game_hud_create(orb_game_view_t *v)
{
    v->hud_time = lv_label_create(v->score_hit);
    orb_text_apply(v->hud_time, ORB_TEXT_FOOTNOTE2, ORB_TEXT_TABULAR, 0);
    lv_obj_set_style_text_color(v->hud_time, ORB_KIT_SECONDARY, 0);
    lv_obj_set_style_text_opa(v->hud_time, ORB_KIT_SECONDARY_OPA, 0);
    lv_obj_set_style_text_align(v->hud_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(v->hud_time, 80);
    lv_label_set_text_static(v->hud_time, v->time_text);
    lv_obj_set_clickable(v->hud_time, false);
    game_centre_text(v->hud_time, 58);
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_t *mark = v->life[i] = lv_obj_create(v->score_hit);
        lv_obj_remove_style_all(mark);
        lv_obj_set_size(mark, 8, 8);
        lv_obj_set_pos(mark, 40 - 4 + ((int)i - 1) * 14, 54);
        lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(mark, lv_color_white(), 0);
        lv_obj_set_style_bg_color(mark, lv_color_white(), 0);
        lv_obj_set_clickable(mark, false);
        lv_obj_set_scrollable(mark, false);
    }
}

