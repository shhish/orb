#include "agents_internal.h"

/* The inner track's inside radius is 191px. HIG §3 / Design 1.1 §4:
 * every physical line keeps 16px from that circle at BOTH line-box ends. */
#define USAGE_TEXT_MARGIN (ORB_SCREEN_R - 191 + 16)

int usage_line_width(int y, int height)
{
    return orb_chord_width(y, y + height - 1, USAGE_TEXT_MARGIN);
}

void usage_line_place(lv_obj_t *text, int y)
{
    int height = lv_obj_get_style_text_font(text, 0)->line_height;
    int width = usage_line_width(y, height);
    lv_obj_set_style_max_width(text, width, 0);
    lv_obj_set_size(text, width, height);
    lv_obj_set_pos(text, 233 - width / 2, y);
}

lv_obj_t *usage_line(lv_obj_t *parent, orb_text_style_t style, unsigned flags, lv_color_t color)
{
    lv_obj_t *text = lv_label_create(parent);
    orb_text_apply(text, style, flags, 0);
    lv_label_set_text_static(text, "");
    lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(text, color, 0);
    if (lv_color_eq(color, ORB_KIT_SECONDARY)) lv_obj_set_style_text_opa(text, ORB_KIT_SECONDARY_OPA, 0);
    lv_obj_set_event_bubble(text, true);
    return text;
}

void usage_layout(agents_state_t *w, unsigned p, bool known, const char *value_text, const char *reset,
                         const char *week, const char *week_reset, const char *credits)
{
    lv_obj_t *provider = w->usage_providers[p], *value = w->usage_values[p];
    lv_obj_t *resets = w->usage_resets[p], *weekly = w->usage_weeks[p][0];
    /* Title1 is the focal numeric counter. The semantic Title3 missing-data
     * message keeps both words intact at AX3 instead of shrinking the font. */
    orb_text_style_t style = known ? ORB_TEXT_LARGE_TITLE : ORB_TEXT_TITLE3;
    unsigned flags = ORB_TEXT_TABULAR | (known ? ORB_TEXT_EMPHASIZED : ORB_TEXT_REGULAR);
    const lv_font_t *font = orb_font_variant(style, flags);
    if (lv_obj_get_style_text_font(value, 0) != font)
        orb_text_apply(value, style, flags, 0);
    /* Select the final semantic face before reserving its digits. Otherwise
     * the first telemetry tick remeasures the creation font as Large Title. */
    int provider_h = lv_obj_get_style_text_font(provider, 0)->line_height;
    int value_h = lv_obj_get_style_text_font(value, 0)->line_height;
    int reset_h = lv_obj_get_style_text_font(resets, 0)->line_height, week_h = lv_obj_get_style_text_font(weekly, 0)->line_height;
    /* Week, then its reset day or "Using credits" (a third line would meet the footer). Both
     * are reserved even when empty, so digit changes (9% to 100%) never move the hero. */
    const char *lines[3] = {known ? week : "", known ? (credits[0] ? credits : week_reset) : "", ""};
    unsigned count = known ? 2 : 0;
    bool ax = known && orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3;
    int gap = ax ? 0 : 8, reset_gap = ax ? 2 : 8;
    int top = (466 - provider_h - gap - value_h) / 2;
    if (known) {
        int height = provider_h + gap + value_h + reset_gap + reset_h + 6 + (int)count * week_h;
        top = ax ? (416 - height) / 2 : 104;
    }
    usage_line_place(provider, top);
    usage_line_place(value, top + provider_h + gap);
    if (!known || p != w->page_index || !w->usage_bound)
        orb_kit_value_set(value, value_text, known ? "100%" : "Not available");
    int reset_y = top + provider_h + gap + value_h + reset_gap;
    usage_line_place(resets, reset_y);
    orb_kit_value_set_rolling(resets, known ? reset : "", "Week · resets Wed 88:88 PM");
    lv_obj_set_hidden(resets, !known);
    for (unsigned i = 0; i < 3; i++) {
        usage_line_place(w->usage_weeks[p][i], reset_y + reset_h + 6 + (int)i * week_h);
        const char *reserve[] = {"Week 100%", credits[0] ? credits : "resets Wed 88:88 PM", ""}; /* widest of each slot */
        if (i || p != w->page_index || !strstr(week, "%") || !w->usage_bound)
            orb_kit_value_set_rolling(w->usage_weeks[p][i], lines[i], reserve[i]);
        lv_obj_set_hidden(w->usage_weeks[p][i], !lines[i][0]);
    }
    /* Review #34: a legend mark in the ring's own weight before each line
     * it labels: the thick outer ring's window, then the thin weekly ring. */
    lv_obj_update_layout(w->usage_pages[p]);
    lv_obj_t *labelled[2] = {resets, weekly};
    bool shown[2] = {known, known && w->usage_rings[1] && strstr(week, "%") != NULL};
    for (unsigned i = 0; i < 2; i++) {
        lv_obj_t *mark = w->usage_marks[p][i];
        lv_obj_set_hidden(mark, !shown[i]);
        if (!shown[i]) continue;
        const char *text = i ? week : reset;
        int line = lv_obj_get_style_text_font(labelled[i], 0)->line_height;
        int x = 233 - agents_ink(labelled[i], text) / 2 - 8 - lv_obj_get_width(mark);
        lv_obj_set_pos(mark, x, lv_obj_get_y(labelled[i]) + (line - lv_obj_get_height(mark)) / 2);
    }
}

/* One provider's windows as the page presents them: the focal value (5 hours,
 * another duration, or the week when that is all the provider reports) and a
 * separate weekly window when one exists. Text and both rings read this one
 * selection, so a ring can never show a number its labels don't. */
static void usage_ring_bind(lv_obj_t *ring, lv_color_t colour, const orb_usage_window_t *win, bool shown, bool animate)
{
    lv_obj_set_style_arc_color(ring, colour, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ring, colour, LV_PART_INDICATOR);
    int percent = win ? (int)lrint(win->used_percent) : 0;
    int32_t target;
    if (!orb_kit_ring_semantic_value(ring, &target) || target != percent * 10)
        orb_kit_ring_set_value(ring, percent, animate && win != NULL);
    /* Staleness is explicit text. Keep the last known ring comparable;
     * missing data alone removes its indicator and leaves the track. */
    lv_opa_t opa = win ? LV_OPA_COVER : LV_OPA_TRANSP;
    if (lv_obj_get_style_arc_opa(ring, LV_PART_INDICATOR) != opa)
        lv_obj_set_style_arc_opa(ring, opa, LV_PART_INDICATOR);
    lv_obj_set_hidden(ring, !shown);
}

static void percent_value(float value, char *out, size_t capacity, void *context)
{
    (void)context;
    snprintf(out, capacity, "%d%%", (int)lrintf(value));
}

static void weekly_value(float value, char *out, size_t capacity, void *context)
{
    (void)context;
    snprintf(out, capacity, "Week %d%%", (int)lrintf(value));
}

/* Review #4: both rings show the visible provider (orb_usage_page_t), in its colour. */
void usage_rings_bind(agents_state_t *w)
{
    orb_usage_page_t page;
    orb_usage_page(&w->model->usage, w->page_index, agents_ready(w), false, &page);
    lv_color_t colour = agents_colour(w->page_index);
    bool animate = w->usage_bound && w->usage_bound_page == w->page_index;
    /* A provider switch is an identity change, not a fabricated telemetry
     * journey from another provider's number (delight §6 never lies). */
    orb_kit_ring_set_value_label(w->usage_rings[0], page.main ? w->usage_values[w->page_index] : NULL, "100%");
    orb_kit_ring_set_value_label(w->usage_rings[1], page.week_ring ? w->usage_weeks[w->page_index][0] : NULL, "Week 100%");
    usage_ring_bind(w->usage_rings[0], colour, page.main, true, animate);
    usage_ring_bind(w->usage_rings[1], colour, page.week_ring, page.week_ring, animate);
    w->usage_bound = true;
    w->usage_bound_page = w->page_index;
}

void agents_usage_page(agents_state_t *w)
{
    w->page_index = LV_MIN(w->page_index, 1u);

    if (w->meter) orb_kit_page_dots_set_active(w->meter, w->page_index,
        w->page_index ? ORB_KIT_CODEX : ORB_KIT_CLAUDE);
    for (unsigned p = 0; p < 2; p++)
        if (w->usage_pages[p]) lv_obj_set_hidden(w->usage_pages[p], p != w->page_index);
    if (w->usage_rings[0]) usage_rings_bind(w);
    unsigned working = 0;
    for (unsigned i = 0; i < w->model->session_count; i++)
        if (w->model->prefs.sources[w->model->sessions[i].agent] &&
            w->model->sessions[i].state == ORB_SESSION_BUSY) working++;
    char caption[96] = "";
    orb_usage_page_t page;
    orb_usage_page(&w->model->usage, w->page_index, agents_ready(w), false, &page);
    if (page.stale) snprintf(caption, sizeof(caption), "Last known");
    else if (working) snprintf(caption, sizeof(caption), "%u working", working);
    orb_kit_value_set_rolling(w->usage_footer, caption, "888 working");
}

void agents_usage_update(agents_state_t *w)
{
    for (unsigned p = 0; p < 2; p++) {
        orb_usage_page_t page;
        orb_usage_page(&w->model->usage, p, agents_ready(w), orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3, &page);
        usage_layout(w, p, page.main != NULL, page.value, page.reset, page.weekly, page.week_reset, page.credits);
    }
    agents_usage_page(w);
}

void agents_usage(agents_state_t *w)
{
    agents_chrome_clear(w);
    /* The arc helper's size is 2*radius+width: outer radii 223/197, an
     * exact 10px bezel inset and 16px clear gap between the two tracks. */
    w->usage_rings[0] = orb_kit_ring_create(w->page, 218, 10, ORB_KIT_CLAUDE);
    w->usage_rings[1] = orb_kit_ring_create(w->page, 194, 6, ORB_KIT_CLAUDE);
    orb_kit_ring_set_formatter(w->usage_rings[0], percent_value, NULL);
    orb_kit_ring_set_formatter(w->usage_rings[1], weekly_value, NULL);

    w->meter = orb_kit_page_dots_create(w->page, 2);
    /* HIG §§1/7: both full-size centre pages are retained. Crown and vertical
     * paging switch visibility and rebind the two retained rings, never rebuild them. */
    for (unsigned p = 0; p < 2; p++) {
        lv_obj_t *page = agents_plain(w->page); w->usage_pages[p] = page;
        lv_obj_set_size(page, 466, 466);
        w->usage_providers[p] = usage_line(page, ORB_TEXT_HEADLINE, ORB_TEXT_EMPHASIZED,
            agents_colour(p));
        lv_label_set_text_static(w->usage_providers[p], agents_provider(p));
        w->usage_values[p] = usage_line(page, ORB_TEXT_TITLE1, ORB_TEXT_REGULAR | ORB_TEXT_TABULAR, ORB_LABEL);
        w->usage_resets[p] = usage_line(page, ORB_TEXT_FOOTNOTE2, ORB_TEXT_TABULAR, ORB_KIT_SECONDARY);
        for (unsigned i = 0; i < 3; i++)
            w->usage_weeks[p][i] = usage_line(page, ORB_TEXT_FOOTNOTE2, ORB_TEXT_TABULAR, ORB_KIT_SECONDARY);
        for (unsigned i = 0; i < 2; i++) {
            lv_obj_t *mark = agents_plain(page); w->usage_marks[p][i] = mark;
            lv_obj_set_size(mark, 16, i ? 6 : 10);
            lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(mark, agents_colour(p), 0);
            lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
        }
    }
    /* The working count is also inside the inner track, rather than crossing
     * either ring at six o'clock. Its box obeys the same 16px chord clearance. */
    w->usage_footer = usage_line(w->page, ORB_TEXT_FOOTNOTE2, ORB_TEXT_EMPHASIZED, ORB_KIT_SECONDARY);
    usage_line_place(w->usage_footer, orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 ? 352 : 336);
    agents_usage_update(w);
    orb_usage_page_t page;
    orb_usage_page(&w->model->usage, w->page_index, agents_ready(w), false, &page);
    if (page.main) orb_kit_ring_set_arrival(w->usage_rings[0], (int)lrint(page.main->used_percent));
    if (page.week_ring) orb_kit_ring_set_arrival(w->usage_rings[1], (int)lrint(page.week_ring->used_percent));
}

