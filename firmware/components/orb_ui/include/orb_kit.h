#pragma once

/* Design 2.0 components. UI thread / LVGL lock, like orb_ui. All dimensions
 * are device pixels. Creation/content changes may allocate; drawing and
 * scrolling reuse prepared storage. Callers retain lv_obj user_data. */
#include "orb_ui.h"
#include "orb_kit_motion.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ORB_KIT_DISC 80
#define ORB_KIT_DISC_MINI 56
#define ORB_KIT_DISC_ROW 40
#define ORB_KIT_ART 180
#define ORB_KIT_PRIMARY_H 64
#define ORB_KIT_SECONDARY_H 52
#define ORB_KIT_KNOB 30
#define ORB_KIT_FOCUS_WIDTH 3
#define ORB_KIT_FOCUS_OFFSET 4
#define ORB_KIT_TITLE_RADIUS 200
#define ORB_KIT_TITLE_INSET_RADIUS 176 /* Ink stays inside a full rim gauge. */
#define ORB_KIT_CAPTION_RADIUS 212
#define ORB_KIT_SCROLL_RADIUS 224 /* Outside the widest chord-fitted rows. */
#define ORB_KIT_ROW_H 72
#define ORB_KIT_TITLE_MAX_DEGREES 120 /* Wider titles reach the side content. */
#define ORB_KIT_PRESS_SCALE 240 /* LVGL 256 = 100%, rounded 94%. */
#define ORB_KIT_PRESS_MS 80
#define ORB_KIT_RELEASE_MS 160
#define ORB_KIT_VALUE_MS 280

#define ORB_KIT_SECONDARY ORB_LABEL_SECONDARY
#define ORB_KIT_SECONDARY_OPA ORB_LABEL_SECONDARY_OPA
#define ORB_KIT_GREY ORB_GRAY
#define ORB_KIT_CLAUDE ORB_ORANGE
#define ORB_KIT_CODEX ORB_BLUE
#define ORB_KIT_DOT ORB_INDIGO
/* Increased-contrast indigo (hig.md §5): Dot/Orbit text and tints on dark glass. */
#define ORB_KIT_DOT_CONTRAST lv_color_hex(0xA7AAFF)
#define ORB_KIT_HOME ORB_YELLOW
#define ORB_KIT_TIME lv_color_hex(0x64D2FF)
#define ORB_KIT_PLAY ORB_GREEN
#define ORB_KIT_SOUND ORB_PINK
#define ORB_KIT_DATE ORB_RED
#define ORB_KIT_MINT lv_color_hex(0x63E6E2)

typedef enum {
    ORB_KIT_DISC_STANDARD, ORB_KIT_DISC_KIND_MINI,
    ORB_KIT_DISC_KIND_ROW, ORB_KIT_DISC_ART,
} orb_kit_disc_kind_t;
typedef enum {
    ORB_KIT_ACCENT_FILL, ORB_KIT_NEUTRAL_GLASS,
    ORB_KIT_TINTED_GLASS, ORB_KIT_TRACK,
} orb_kit_disc_style_t;

/* Linear-light luminance threshold: only light accents get black ink. */
bool orb_kit_accent_uses_black(lv_color_t accent);
lv_color_t orb_kit_accent_ink(lv_color_t accent);
lv_obj_t *orb_kit_disc_create(lv_obj_t *parent, orb_kit_disc_kind_t kind,
                              orb_kit_disc_style_t style, lv_color_t accent, const char *glyph);
/* HIG §7 sheet dismissal: one shared mini X on the top-leading diagonal.
 * The caller supplies its reciprocal close action. */
lv_obj_t *orb_kit_close_create(lv_obj_t *parent);
/* Focus transfers glide on a velocity-preserving spring; Reduce Motion uses
 * a 150ms endpoint crossfade. Layout and hit rectangles never move. */
void orb_kit_disc_set_focus(lv_obj_t *obj, bool focused);
/* Settle focus under this parent before freezing a navigation snapshot. */
void orb_kit_focus_finish(lv_obj_t *parent);
/* Selector ownership: kit focus outline plus LV_STATE_CHECKED, no extra tint. */
void orb_kit_chip_set_selected(lv_obj_t *obj, bool selected);
lv_obj_t *orb_kit_button_primary_create(lv_obj_t *parent, const char *text, lv_color_t accent);
lv_obj_t *orb_kit_button_secondary_create(lv_obj_t *parent, const char *text);
/* Kit children are private. Static text changes rebuild presentation while
 * idle; values reserve their template once and remain tabular while changing. */
void orb_kit_button_set_text(lv_obj_t *obj, const char *text);
const char *orb_kit_button_get_text(lv_obj_t *obj);
/* Polish review #27: equal-status capsules share the wider one's width. The
 * floor survives Dynamic Type re-measures; the bottom chord still caps it. */
void orb_kit_button_set_min_width(lv_obj_t *obj, int32_t width);
void orb_kit_button_set_value(lv_obj_t *obj, const char *text, const char *reserve_template);
void orb_kit_chip_set_value(lv_obj_t *obj, const char *text, const char *reserve_template);
void orb_kit_disc_set_glyph(lv_obj_t *obj, const char *glyph);
void orb_kit_disc_set_ink(lv_obj_t *obj, lv_color_t colour);
void orb_kit_disc_set_ink_opa(lv_obj_t *obj, lv_opa_t opacity);
void orb_kit_disc_set_text_style(lv_obj_t *obj, orb_text_style_t style, orb_text_variant_t variant);
void orb_kit_disc_set_glyph_hidden(lv_obj_t *obj, bool hidden);

typedef struct {
    int32_t width;
    unsigned template_measures, updates, raster_prepares;
    bool tabular, live;
} orb_kit_value_info_t;
/* Existing labels keep their text style/clock family and any larger caller
 * lane. Set after applying typography. NULL template retains the reservation.
 * A repeated value is a no-op. Neither a value tick nor its idle timer typesets
 * a raster: live content remains native, including when it is at rest. */
void orb_kit_value_set(lv_obj_t *label, const char *text, const char *reserve_template);
void orb_kit_hero_set_value(lv_obj_t *label, const char *text, const char *reserve_template);
bool orb_kit_value_info(lv_obj_t *obj, orb_kit_value_info_t *out);
lv_obj_t *orb_kit_glyph_button_create(lv_obj_t *parent, const char *glyph, bool primary, lv_color_t accent);
void orb_kit_place_bottom_arc(lv_obj_t *obj);
void orb_kit_place_arc_pair(lv_obj_t *a, lv_obj_t *b);
/* Idempotent shared response. PRESS_LOST/scroll retire the contact until the
 * next PRESSED; even manually delivered CLICKED is stopped after cancellation. */
void orb_kit_press_apply(lv_obj_t *obj);
void orb_kit_press_cancel(lv_obj_t *obj);
bool orb_kit_press_is_cancelled(lv_obj_t *obj);
/* HIG §8: one unavailable-state treatment for every surface (polish review
 * #12). A disabled control keeps its place and size, dims its ink to
 * ORB_KIT_DISABLED_OPA, loses any pending press and takes no input. Rows use
 * orb_kit_row_set_enabled. A status caption says why, in these words. */
#define ORB_KIT_DISABLED_OPA 102
#define ORB_KIT_OFFLINE_CAPTION "Mac offline · last known"
#define ORB_KIT_SYNCING_CAPTION "Syncing · last known"
#define ORB_KIT_READ_ONLY_CAPTION "Read-only"
void orb_kit_control_set_enabled(lv_obj_t *obj, bool enabled);
/* Centred secondary Footnote 2 line whose box fits its circle chord at y. */
lv_obj_t *orb_kit_status_caption_create(lv_obj_t *parent, const char *text, int32_t y);
/* Semantic role for diagnostics/tests; identity-colour content discs are false. */
bool orb_kit_is_primary(lv_obj_t *obj);

typedef struct {
    orb_text_style_t style;
    float angle_degrees;
    bool ellipsised;
} orb_kit_rim_text_info_t;
lv_obj_t *orb_kit_rim_title_create(lv_obj_t *parent, const char *text, lv_color_t colour);
lv_obj_t *orb_kit_rim_title_inset_create(lv_obj_t *parent, const char *text, lv_color_t colour);
/* Use on a title (including a two-colour title) when adding/removing a full
 * rim ring. Rebuilds the cached text while idle; never call from draw/scroll. */
bool orb_kit_rim_title_set_inset(lv_obj_t *obj, bool inset);
/* Narrower arc budget for sibling-heavy apps (Home caps names at ~90°).
 * Longer text steps down to Footnote 2, then ends in one U+2026. */
bool orb_kit_rim_title_set_max_degrees(lv_obj_t *obj, float degrees);
lv_obj_t *orb_kit_rim_title_two_colour_create(lv_obj_t *parent, const char *text,
                                             lv_color_t colour, const char *suffix);
lv_obj_t *orb_kit_rim_date_create(lv_obj_t *parent, const char *weekday, const char *day);
lv_obj_t *orb_kit_rim_caption_create(lv_obj_t *parent, const char *text);
/* Apps uses Caption 2 emphasized white; hints default to secondary Footnote 2.
 * Styling rebuilds the prepared caption while idle, retaining radius 212. */
bool orb_kit_rim_caption_set_style(lv_obj_t *obj, orb_text_style_t style,
                                  orb_text_variant_t variant, lv_color_t colour, lv_opa_t opacity);
void orb_kit_rim_text_set(lv_obj_t *obj, const char *text, const char *suffix);
/* Borrowed semantic text before fitting/ellipsis; valid until set/delete. */
const char *orb_kit_rim_text_get(lv_obj_t *obj);
float orb_kit_rim_text_measure(const char *text, orb_text_style_t style, int32_t radius);
bool orb_kit_rim_text_info(lv_obj_t *obj, orb_kit_rim_text_info_t *out);

lv_obj_t *orb_kit_page_dots_create(lv_obj_t *parent, unsigned count);
void orb_kit_page_dots_set_active(lv_obj_t *obj, unsigned index, lv_color_t accent);
/* Owned by list, sibling of the list. Position follows scroll/layout events;
 * breathing uses the shared paused-at-rest clock. Either may delete safely. */
lv_obj_t *orb_kit_scroll_arc_attach(lv_obj_t *list);
/* Retained readers call after explicit visibility changes (LVGL emits no
 * hide/show event). The indicator follows its reader without an idle timer. */
static inline void orb_kit_scroll_arc_refresh(lv_obj_t *list)
{
    if (list) lv_obj_send_event(list, LV_EVENT_REFRESH, NULL);
}
/* Same right-rim indicator for a finite page stack, zero-based position. */
lv_obj_t *orb_kit_scroll_arc_create(lv_obj_t *parent, unsigned position, unsigned count);
void orb_kit_scroll_arc_set_position(lv_obj_t *obj, unsigned position, unsigned count);
lv_obj_t *orb_kit_ring_create(lv_obj_t *parent, int32_t radius, int32_t width, lv_color_t accent);
void orb_kit_ring_set_value(lv_obj_t *obj, int32_t percent, bool animated);
void orb_kit_ring_set_text(lv_obj_t *obj, const char *text, const char *reserve_template);
/* Stopwatch content: outer tick tips at radius, 12 major divisions or 60
 * divisions with every fifth emphasized. Position accepts any shared time
 * units (e.g. elapsed milliseconds / 60000 for one minute). No input owner. */
lv_obj_t *orb_kit_tick_ring_create(lv_obj_t *parent, int32_t radius,
                                  unsigned tick_count, lv_color_t accent);
void orb_kit_tick_ring_set_position(lv_obj_t *obj, uint32_t elapsed, uint32_t period);

typedef void (*orb_kit_detent_cb_t)(lv_obj_t *slider, int32_t value, void *user_data);
lv_obj_t *orb_kit_rim_slider_create(lv_obj_t *parent, int32_t radius, int32_t width,
                                   lv_color_t accent, int32_t min, int32_t max,
                                   int32_t detent_step, orb_kit_detent_cb_t cb, void *user_data);
void orb_kit_rim_slider_set_value(lv_obj_t *obj, int32_t value, bool animated);
void orb_kit_rim_slider_set_angles(lv_obj_t *obj, int32_t start, int32_t sweep);
int32_t orb_kit_rim_slider_get_value(lv_obj_t *obj);

/* One level control: radius 213, width 14, start 145°, sweep −110°, 30 px
 * knob, accent tabular percentage. Returns the actual slider, so the existing
 * rim-slider setters also update its live centre. Optional inset title/icon
 * are owned by the slider and deleted with it. Default value centre is y380;
 * standalone pages can use y240 to leave room for context above the arc. */
lv_obj_t *orb_kit_level_create(lv_obj_t *parent, lv_color_t accent, int32_t min, int32_t max,
                               int32_t step, orb_kit_detent_cb_t cb, void *user_data);
void orb_kit_level_set_value(lv_obj_t *obj, int32_t value, bool animated);
void orb_kit_level_set_text(lv_obj_t *obj, const char *text, const char *reserve_template);
void orb_kit_level_set_value_y(lv_obj_t *obj, int32_t y);
void orb_kit_level_set_value_center(lv_obj_t *obj, int32_t x, int32_t y);
void orb_kit_level_set_text_style(lv_obj_t *obj, orb_text_style_t style, orb_text_variant_t variant);
void orb_kit_level_set_icon(lv_obj_t *obj, const char *glyph);
void orb_kit_level_set_title(lv_obj_t *obj, const char *text);
/* Track/fill colour and value ink for the current state. Unknown hides the
 * fill and knob; ORB_KIT_SECONDARY ink uses the secondary label opacity. */
void orb_kit_level_set_state(lv_obj_t *obj, lv_color_t fill, lv_color_t value, bool known);
/* A screen whose own control already shows the value (Control Center's chip). */
void orb_kit_level_set_value_hidden(lv_obj_t *obj, bool hidden);

lv_obj_t *orb_kit_switch_create(lv_obj_t *parent, bool checked, lv_color_t accent);
void orb_kit_switch_set_checked(lv_obj_t *obj, bool checked, bool animated);
bool orb_kit_switch_get_checked(lv_obj_t *obj);

typedef struct {
    const char *title;
    const char *subtitle;
    const char *value;
    const char *glyph;
    lv_color_t accent;
    bool show_switch;
    bool checked;
} orb_kit_row_config_t;
lv_obj_t *orb_kit_row_create(lv_obj_t *list, const orb_kit_row_config_t *config);
void orb_kit_row_set_value(lv_obj_t *row, const char *value);
void orb_kit_row_set_live_value(lv_obj_t *row, const char *value, const char *reserve_template);
/* Prepare native leading ink once during construction; later state changes
 * update its fill and glyph without rebuilding the immutable chord source. */
void orb_kit_row_set_live_accent(lv_obj_t *row, lv_color_t fill, lv_color_t ink);
/* Recompute floating row ink after an admitted hidden page becomes visible. */
void orb_kit_list_refresh(lv_obj_t *list);
typedef enum {
    ORB_KIT_ROW_TEXT_TITLE, ORB_KIT_ROW_TEXT_SUBTITLE, ORB_KIT_ROW_TEXT_VALUE,
} orb_kit_row_text_role_t;
void orb_kit_row_set_text(lv_obj_t *row, orb_kit_row_text_role_t role, const char *text);
/* A live lane retains its transparent geometry anchor when its colour changes. */
void orb_kit_row_set_text_colour(lv_obj_t *row, orb_kit_row_text_role_t role, lv_color_t colour);
void orb_kit_row_set_live_text(lv_obj_t *row, orb_kit_row_text_role_t role,
                              const char *text, const char *reserve_template);
void orb_kit_row_set_focus(lv_obj_t *row, bool focused);
/* Covers the prepared row and its floating switch. Disabling retires a
 * current press so a later release cannot act when the row is re-enabled. */
void orb_kit_row_set_enabled(lv_obj_t *row, bool enabled);
/* Durations, dates and counts use ORB_TEXT_TABULAR; roles remain independent.
 * Published type styles step down before a single-line ellipsis is necessary. */
bool orb_kit_row_set_text_role(lv_obj_t *row, orb_kit_row_text_role_t role,
                                orb_text_variant_t variant);
/* Opt into the same two-line row growth at ordinary content sizes, too.
 * AX3 rows use this mechanism automatically; repeated opt-ins are idempotent. */
void orb_kit_row_wrap_title(lv_obj_t *row, orb_text_variant_t variant);
/* Reserve enough wrapped lines for a complete identity, stepping through
 * published title styles when needed to keep the whole row in its viewport. */
void orb_kit_row_show_full_title(lv_obj_t *row, orb_text_variant_t variant);

typedef struct {
    orb_text_style_t style;
    unsigned lines;
    bool complete;
} orb_kit_text_fit_info_t;
/* Fits full semantic text through Title3, Headline, Caption2, never ellipsis.
 * max_lines is 2 or 3. If even Caption2 needs more lines, returns false and keeps
 * the complete semantic text at Caption2; the caller must expand its budget.
 * Re-fits on Dynamic Type/Bold changes. Call again to change text or budget. */
bool orb_kit_hero_fit(lv_obj_t *label, const char *text, int32_t width,
                       unsigned max_lines, orb_kit_text_fit_info_t *out);
bool orb_kit_hero_fit_info(lv_obj_t *label, orb_kit_text_fit_info_t *out);
/* Static identity in an existing fixed lane. Middle elision preserves UTF-8
 * prefix/suffix and published type; call again when text or fonts change. */
bool orb_kit_label_middle_ellipsis(lv_obj_t *label, const char *text);
/* Polish review #1: the largest reader height <= available that ends on a
 * whole wrapped line's ink at rest (all of available if the content fits or
 * nothing straddles it). Font descent without ink never hides a fitting line.
 * Interactive rows retain normal list clipping. */
int32_t orb_kit_reader_whole_lines(lv_obj_t *reader, int32_t available);
/* Keeps a scroll reader's resting viewport on whole lines from now on: on the
 * display's next refresh after its content or requested height changes, its
 * height becomes orb_kit_reader_whole_lines() of the height the caller set.
 * orb_kit_detail_page() readers do this already; other readers opt in. */
void orb_kit_reader_snap(lv_obj_t *reader);

/* Glossary §1.3: the text, or its longest prefix of whole code points that
 * fits `width` with one U+2026 appended (no space or separator before it).
 * Returns the bytes written; capacity must exceed strlen(text) + 3. */
size_t orb_kit_text_truncate(const char *text, const lv_font_t *font, int32_t letter_space,
                             int32_t width, char *out, size_t capacity);
/* Balanced wrap for headings and captions with LV_TEXT_FLAG_AVOID_WIDOW:
 * the wrap width for `text` in a `width` lane. When wrapping ends on a widow
 * (one word, or under half the widest line) it is the narrowest width keeping the line count;
 * otherwise `width` itself, so wraps that already read well never move. */
int32_t orb_kit_text_balance_width(const char *text, const lv_font_t *font, int32_t letter_space,
                                   int32_t width);
/* Balances a wrapped label inside its own lane with alignment-side padding,
 * keeping its geometry and semantic text; re-balances after every Dynamic
 * Type/Bold Text or width change. Call again after changing its text. */
void orb_kit_label_balance(lv_obj_t *label);
/* Command text: breaks only at spaces and after '/' (never at "-j4"'s
 * hyphen), through the LVGL fork's path-break flag. */
void orb_kit_label_set_command(lv_obj_t *label, const char *command);


/* Live floating sibling of row; owned by row/list. Do not reparent. */
lv_obj_t *orb_kit_row_switch(lv_obj_t *row);
bool orb_kit_list_apply(lv_obj_t *list);

/* Face artwork/AOD presentation: geometry/opacity only, sources unchanged. */
void orb_kit_rim_text_set_presentation(lv_obj_t *obj, int32_t scale, lv_opa_t opacity);
void orb_kit_rim_text_set_opa(lv_obj_t *obj, lv_opa_t opacity);

#ifdef __cplusplus
}
#endif

#include "orb_kit_controls_motion.h"
#include "orb_kit_effects.h"
