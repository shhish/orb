#include "orb_ui.h"
#include "orb_ui_private.h"

/* Content masks live with their reader/list viewport. They never create a
 * glass surface, and draw uses cached line bounds rather than measuring text. */
typedef struct {
    int32_t top, bottom;
    bool follow_scroll;
} scroll_edge_t;

static void scroll_edge_event(lv_event_t *e);

static scroll_edge_t *edge_find(lv_obj_t *viewport)
{
    for (unsigned i = 0; i < lv_obj_get_event_count(viewport); i++) {
        lv_event_dsc_t *descriptor = lv_obj_get_event_dsc(viewport, i);
        if (lv_event_dsc_get_cb(descriptor) == scroll_edge_event)
            return lv_event_dsc_get_user_data(descriptor);
    }
    return NULL;
}

static void draw_edge(lv_layer_t *layer, lv_obj_t *viewport, lv_area_t area, bool top)
{
    /* HIG §2.1: dissolve real content under floating controls. This is an
     * alpha mask over content, never a decorative gradient background. */
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.base.obj = viewport;
    d.bg_color = lv_color_black();
    d.bg_opa = LV_OPA_COVER;
    d.bg_grad.dir = LV_GRAD_DIR_VER;
    d.bg_grad.stops_count = 2;
    d.bg_grad.stops[0] = (lv_grad_stop_t){lv_color_black(), top ? LV_OPA_COVER : LV_OPA_TRANSP, 0};
    d.bg_grad.stops[1] = (lv_grad_stop_t){lv_color_black(), top ? LV_OPA_TRANSP : LV_OPA_COVER, 255};
    /* LVGL clips these two alpha ramps to the current band. No backdrop copy,
     * blur scratch, or full-screen mask is needed for the black content canvas. */
    lv_draw_rect(layer, &d, &area);
}

static void scroll_edge_event(lv_event_t *e)
{
    scroll_edge_t *edge = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        orb_ui_free(edge);
        return;
    }
    if (code != LV_EVENT_DRAW_POST) {
        return;
    }
    lv_obj_t *viewport = lv_event_get_current_target_obj(e);
    lv_area_t area;
    lv_obj_get_coords(viewport, &area);
    int32_t half = lv_area_get_height(&area) / 2;
    /* HIG §§2.1/7: leave the first resting row at full contrast; only content
     * leaving the viewport dissolves. This reads the actual scroll position,
     * including an interrupted fling, without a second animation or timer. */
    int32_t top = LV_MIN(edge->top, half), bottom = LV_MIN(edge->bottom, half);
    if (edge->follow_scroll) {
        /* Grow the dissolve with the clipped distance: a one-pixel drag
         * must not suddenly dim a full line of the first/last resting row. */
        top = LV_MIN(top, LV_MAX(0, lv_obj_get_scroll_y(viewport)));
        bottom = LV_MIN(bottom, LV_MAX(0, lv_obj_get_scroll_bottom(viewport)));
    }
    orb_kit_reader_fades(viewport, &top, &bottom);
    if (top > 0) {
        lv_area_t a = area;
        a.y2 = a.y1 + top - 1;
        draw_edge(lv_event_get_layer(e), viewport, a, true);
    }
    if (bottom > 0) {
        lv_area_t a = area;
        a.y1 = a.y2 - bottom + 1;
        draw_edge(lv_event_get_layer(e), viewport, a, false);
    }
}

static bool scroll_edge_apply(lv_obj_t *viewport, int32_t top_px, int32_t bottom_px,
                              bool follow_scroll)
{
    if (!viewport) {
        return false;
    }
    scroll_edge_t *edge = edge_find(viewport);
    bool created = !edge;
    if (created) {
        edge = orb_ui_alloc(sizeof(*edge));
        if (!edge) {
            return false;
        }
        if (!lv_obj_add_event_cb(viewport, scroll_edge_event, LV_EVENT_ALL, edge)) {
            orb_ui_free(edge);
            return false;
        }
    }
    int32_t top = LV_MAX(0, top_px), bottom = LV_MAX(0, bottom_px);
    if (!created && edge->top == top && edge->bottom == bottom &&
        edge->follow_scroll == follow_scroll) {
        return true;
    }
    edge->top = top;
    edge->bottom = bottom;
    edge->follow_scroll = follow_scroll;
    lv_obj_invalidate(viewport);
    return true;
}

bool orb_scroll_edge_apply(lv_obj_t *viewport, int32_t top_px, int32_t bottom_px)
{
    return scroll_edge_apply(viewport, top_px, bottom_px, false);
}

bool orb_scroll_edge_follow(lv_obj_t *viewport, int32_t top_px, int32_t bottom_px)
{
    return scroll_edge_apply(viewport, top_px, bottom_px, true);
}

