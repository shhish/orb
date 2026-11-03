/*
 * The Sessions demo, laid out round-first (docs/hig.md §3, §7):
 *
 *  - Content, at the back: a heading and session rows, scrolling. Rows are
 *    plain dark platters, not glass; colour only says state, next to a symbol
 *    and words. Each row is a raster cache, so scrolling copies bitmaps.
 *  - Scroll edge: content fades to black as it scrolls up under the toolbar
 *    and the time, and again above the bottom action. Rows shrink to the
 *    round text-safe chord while their source bitmaps stay cached.
 *  - Controls, floating on Liquid Glass: back and add on the 45-degree
 *    diagonals, the time between them at 12 o'clock, and the one tinted
 *    primary action at 6 o'clock.
 *
 * Content shifts furthest with tilt and the controls least (parallax).
 */
#include "orb_demo.h"

#include <stddef.h>
#include <math.h>

#include "orb_ui.h"
#include "orb_kit_shell_controls.h"

#define PHASE_MS 8000                       /* each phase: one scroll down and back up */
#define ROW_W ORB_PT(204)                   /* about the inscribed square */
#define ROW_H ORB_PT(52)
#define ROW_RADIUS ORB_PT(20)
#define TOOLBAR_C 101                       /* toolbar button centres, x and y: on the 45-degree diagonals */
#define PRIMARY_W ORB_PT(120)
#define PRIMARY_CY (ORB_SCREEN_D - ORB_CONTROL) /* clears the bezel by more than the rim margin */
#define CONTENT_TOP (TOOLBAR_C + ORB_CONTROL / 2 + ORB_PT(8))     /* at rest, below the toolbar */
#define CONTENT_BOTTOM (PRIMARY_CY - ORB_CONTROL / 2 - ORB_PT(8)) /* and above the primary action */
#define PARALLAX_PX 8                       /* the furthest layer's largest shift */

typedef enum {
    STATE_RUNNING,
    STATE_APPROVAL,
    STATE_WAITING,
    STATE_DONE,
    STATE_IDLE,
} state_t;

static const struct {
    const char *symbol;
    uint32_t color;
} s_states[] = {
    [STATE_RUNNING] = { ORB_ICON_ARROWS_CLOCKWISE, ORB_RGB_LEGACY_BLUE },
    [STATE_APPROVAL] = { ORB_ICON_WARNING, ORB_RGB_POWER_LOW },
    [STATE_WAITING] = { ORB_ICON_BELL, ORB_RGB_LEGACY_ORANGE },
    [STATE_DONE] = { ORB_ICON_CHECK, ORB_RGB_GREEN },
    [STATE_IDLE] = { ORB_ICON_PAUSE, ORB_RGB_GRAY },
};

static const struct {
    const char *title, *detail;
    state_t state;
} s_sessions[] = {
    { "demo-project", "Needs approval", STATE_APPROVAL },
    { "Kal", "Waiting for you", STATE_WAITING },
    { "Orb firmware", "Building glass", STATE_RUNNING },
    { "example.test", "Writing post", STATE_RUNNING },
    { "Stocks app", "Running tests", STATE_RUNNING },
    { "Xteink", "Fetching articles", STATE_RUNNING },
    { "Cortex", "Thinking", STATE_RUNNING },
    { "Plex", "Compressing", STATE_RUNNING },
    { "Recipes", "Done", STATE_DONE },
    { "Homebridge", "Idle", STATE_IDLE },
};

static lv_obj_t *s_list, *s_primary;
static bool s_glass_phase;
static bool s_pressed;
static bool s_reduced;
static lv_timer_t *s_phase_timer, *s_press_timer;

static lv_obj_t *add_label(lv_obj_t *parent, const char *text, orb_text_style_t style, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    orb_text_apply(l, style, ORB_TEXT_REGULAR, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

/* A plain container that lays nothing out by itself and lets touches through to what's below. */
static lv_obj_t *add_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_clickable(o, false);
    lv_obj_set_scrollable(o, false);
    return o;
}

static void add_row(lv_obj_t *list, const char *title, const char *detail, state_t state)
{
    lv_obj_t *row = add_box(list);
    lv_obj_set_size(row, ROW_W, ROW_H);
    lv_obj_set_style_radius(row, ROW_RADIUS, 0);
    lv_obj_set_style_bg_color(row, ORB_GRAY6, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(row, ORB_PT(12), 0);
    lv_obj_set_style_pad_column(row, ORB_PT(8), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* State in a symbol and in words, never in colour alone. */
    add_label(row, s_states[state].symbol, ORB_TEXT_HEADLINE, orb_color_from_rgb(s_states[state].color));

    lv_obj_t *text = add_box(row);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *t = add_label(text, title, ORB_TEXT_HEADLINE, ORB_LABEL);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(t, LV_PCT(100));
    lv_obj_t *d = add_label(text, detail, ORB_TEXT_FOOTNOTE1, ORB_LABEL_SECONDARY);
    lv_obj_set_style_text_opa(d, ORB_LABEL_SECONDARY_OPA, 0);
    lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(d, LV_PCT(100));
    /* HIG §4: enlarged line boxes must fit before the curved transform. The
     * default stays at ROW_H; AX3 needs the actual font leading plus padding. */
    const lv_font_t *title_font = lv_obj_get_style_text_font(t, LV_PART_MAIN);
    const lv_font_t *detail_font = lv_obj_get_style_text_font(d, LV_PART_MAIN);
    int32_t text_height = title_font->line_height + detail_font->line_height + 2 * ORB_PT(4);
    lv_obj_set_height(row, LV_MAX(ROW_H, text_height));

    /* Drawn once, over the black canvas it always sits on: scrolling copies an opaque bitmap. */
    lv_obj_set_raster_matte(row, lv_color_black());
    lv_obj_set_raster_rows(row, true);
}

/* A uniformly scaled row must fit inside the text-safe circle at both of its
 * outer corners. Solve (s*w/2)^2 + (abs(cy-R) + s*h/2)^2 <= safe_r^2.
 * This changes only compositing: scrolling never regenerates the row bitmap. */
static uint16_t row_scale(const lv_area_t *a)
{
    float hw = lv_area_get_width(a) * 0.5f, hh = lv_area_get_height(a) * 0.5f;
    float dy = fabsf((a->y1 + a->y2 + 1) * 0.5f - ORB_SCREEN_R);
    /* Reserve the maximum diagonal parallax shift as well as the text margin. */
    float r = ORB_SCREEN_R - ORB_RIM_MARGIN_TEXT - PARALLAX_PX * 1.5f - 2;
    if (dy >= r) return 0;
    float aa = hw * hw + hh * hh;
    float scale = (-dy * hh + sqrtf(aa * r * r - hw * hw * dy * dy)) / aa;
    return (uint16_t)(LV_CLAMP(0.0f, scale, 1.0f) * LV_SCALE_NONE);
}

static void fade_edges(lv_event_t *e)
{
    LV_UNUSED(e);
    uint32_t n = lv_obj_get_child_count(s_list);
    const int32_t top_end = TOOLBAR_C + ORB_PT(10);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(s_list, i);
        lv_area_t a;
        lv_obj_get_coords(c, &a);
        int32_t cy = (a.y1 + a.y2) / 2, opa = LV_OPA_COVER;
        if (cy < CONTENT_TOP) {
            opa = LV_OPA_COVER * (cy - top_end) / (CONTENT_TOP - top_end);
        } else if (cy > CONTENT_BOTTOM) {
            opa = LV_OPA_COVER * (PRIMARY_CY - cy) / (PRIMARY_CY - CONTENT_BOTTOM);
        }
        lv_obj_set_raster_opa(c, (lv_opa_t)LV_CLAMP(LV_OPA_TRANSP, opa, LV_OPA_COVER));
        if (i > 0) lv_obj_set_raster_scale(c, row_scale(&a));
    }
}



static void scroll_exec(void *obj, int32_t v)
{
    lv_obj_scroll_to_y(obj, v, LV_ANIM_OFF);
}

static void scroll_start(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_list);
    lv_anim_set_exec_cb(&a, scroll_exec);
    lv_anim_set_values(&a, 0, lv_obj_get_scroll_y(s_list) + lv_obj_get_scroll_bottom(s_list));
    lv_anim_set_duration(&a, PHASE_MS / 2);
    lv_anim_set_reverse_duration(&a, PHASE_MS / 2);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

/* The light swept round the top fast enough to move the highlight every frame. */
static void light_exec(void *var, int32_t v)
{
    LV_UNUSED(var);
    if (!orb_ui_reduce_motion()) orb_glass_set_light(v / 10.0f, 0.6f);
}

static void light_start(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_glass_phase);
    lv_anim_set_exec_cb(&a, light_exec);
    lv_anim_set_values(&a, 2400, 3000);
    lv_anim_set_duration(&a, 400);
    lv_anim_set_reverse_duration(&a, 400);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

/* Glass phase: the primary action is pressed and released every half second. */
static void press_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!s_glass_phase || orb_ui_reduce_motion()) {
        return;
    }
    s_pressed = !s_pressed;
    lv_obj_send_event(s_primary, s_pressed ? LV_EVENT_PRESSED : LV_EVENT_RELEASED, NULL);
}

static void phase_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (orb_ui_reduce_motion()) return;
    s_glass_phase = !s_glass_phase;
    if (s_glass_phase) {
        lv_anim_delete(s_list, scroll_exec);
        light_start();
    } else {
        lv_anim_delete(&s_glass_phase, light_exec);
        if (s_pressed) {
            s_pressed = false;
            lv_obj_send_event(s_primary, LV_EVENT_RELEASED, NULL);
        }
        scroll_start();
    }
}

/* The developer benchmark creates and deletes this scene repeatedly. Its
 * timers and non-object light animation must not outlive the screen. */
static void demo_delete(lv_event_t *e)
{
    lv_anim_delete(s_list, scroll_exec);
    lv_anim_delete(&s_glass_phase, light_exec);
    if (s_phase_timer) lv_timer_delete(s_phase_timer);
    if (s_press_timer) lv_timer_delete(s_press_timer);
    s_phase_timer = s_press_timer = NULL;
    s_list = s_primary = NULL;
}

void orb_demo_build(lv_obj_t *screen)
{
    s_glass_phase = s_pressed = false;
    lv_obj_add_event_cb(screen, demo_delete, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(screen, false);

    /* Content: at rest it starts below the toolbar and ends above the primary action (§2.1). */
    s_list = lv_obj_create(screen);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, ORB_SCREEN_D, ORB_SCREEN_D);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(s_list, CONTENT_TOP, 0);
    lv_obj_set_style_pad_bottom(s_list, ORB_SCREEN_D - CONTENT_BOTTOM, 0);
    lv_obj_set_style_pad_row(s_list, ORB_PT(6), 0);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *heading = add_label(s_list, "Sessions", ORB_TEXT_TITLE3, ORB_LABEL);
    lv_obj_set_raster(heading, true);
    for (size_t i = 0; i < sizeof(s_sessions) / sizeof(s_sessions[0]); i++) {
        add_row(s_list, s_sessions[i].title, s_sessions[i].detail, s_sessions[i].state);
    }

    lv_obj_add_event_cb(s_list, fade_edges, LV_EVENT_SCROLL, NULL);

    /* Controls: one glass layer above the content, never glass on glass. */
    lv_obj_t *chrome = add_box(screen);
    lv_obj_set_size(chrome, ORB_SCREEN_D, ORB_SCREEN_D);
    lv_obj_t *time = add_label(chrome, "10:09", ORB_TEXT_HEADLINE, ORB_LABEL);
    lv_obj_align(time, LV_ALIGN_TOP_MID, 0, TOOLBAR_C - ORB_CONTROL / 2 - ORB_PT(8));
    lv_obj_set_raster(time, true);
    orb_kit_demo_button_create(chrome, ORB_ICON_CHEVRON_LEFT, TOOLBAR_C, TOOLBAR_C, ORB_CONTROL, ORB_GLASS_REGULAR, ORB_GRAY);
    orb_kit_demo_button_create(chrome, ORB_ICON_PLUS, ORB_SCREEN_D - TOOLBAR_C, TOOLBAR_C, ORB_CONTROL, ORB_GLASS_REGULAR, ORB_GRAY);
    s_primary = orb_kit_demo_button_create(chrome, "Reply", ORB_SCREEN_D / 2, PRIMARY_CY, PRIMARY_W,
                          ORB_GLASS_TINTED, ORB_BLUE);

    orb_parallax_add(s_list, 1.0f, PARALLAX_PX);
    orb_parallax_add(chrome, 0.25f, PARALLAX_PX);

    /* Every row drawn now, not as it first scrolls into view. */
    lv_obj_raster_prepare(screen);
    fade_edges(NULL);
    s_reduced = orb_ui_reduce_motion();
    if (!s_reduced) scroll_start();
    s_phase_timer = lv_timer_create(phase_cb, PHASE_MS, NULL);
    s_press_timer = lv_timer_create(press_cb, PHASE_MS / 16, NULL);
}

void orb_demo_tick(float gx, float gy, float gz, float dt_s)
{
    bool reduced = orb_ui_reduce_motion();
    if (reduced != s_reduced) {
        s_reduced = reduced;
        lv_anim_delete(s_list, scroll_exec);
        lv_anim_delete(&s_glass_phase, light_exec);
        s_glass_phase = false;
        if (s_pressed) {
            s_pressed = false;
            lv_obj_send_event(s_primary, LV_EVENT_RELEASED, NULL);
        }
        if (!reduced) scroll_start();
    }
    /* In the glass phase the demo stages the light itself. */
    if (!s_glass_phase || reduced) {
        orb_ui_set_gravity(gx, gy, gz, dt_s);
    }
}

const char *orb_demo_phase(void)
{
    return s_glass_phase ? "glass" : "scroll";
}
