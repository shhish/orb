#pragma once

/* A shoulder preview needs four meaningful characters across its label;
 * wrapped lines remain one semantic block rather than orphaning a suffix. */
#define ORB_ROUND_CUT_HIDDEN (UINT16_MAX - 1)

#include "orb_ui.h"

#define ROUND_CHORD_WIDTH_STEP 8
#define CHORD_TEXT_LINES 6
#define CHORD_TEXT_BUCKETS (ORB_SCREEN_D / ROUND_CHORD_WIDTH_STEP + 2)

/* Private immutable row-source contract shared by cold packing and the painter. */
typedef struct round_chord_row round_chord_row_t;

typedef struct {
    lv_obj_t *obj;
    int16_t x, y, width, height, pad, ink_top, ink_bottom;
    bool trailing, flexible, hidden, bounded_ink;
    uint16_t *cut;
    const uint16_t *source_runs;
    int16_t draw_width;
    uint16_t draw_bucket;
    uint16_t line_height, lines, line_box;
    lv_draw_buf_t *dots;
} round_chord_child_t;

/* The text line owning source scanline sy. Inter's line box is taller than
 * its pitch (negative line space), so a descender of line k reaches into line
 * k+1's slot: those rows belong to the line above, and rows below the last
 * pitch belong to the last line. Treating them as an uncut extra line copied
 * the clipped glyph's descender under the ellipsis ("Livin…" over a "g"). */
static inline unsigned chord_text_line(const round_chord_child_t *c, int sy)
{
    if (!c->line_height || c->lines <= 1) return 0;
    int row = sy - c->y - LV_MAX(0, (int)c->line_box - (int)c->line_height);
    return row < 0 ? 0 : LV_MIN((unsigned)row / c->line_height, c->lines - 1u);
}

/* HIG §§3/6: retain exactly the reference row pixels while hoisting division,
 * opacity and byte order out of the pixel loops. The adjusted palette belongs to
 * its immutable row; only the synchronous painter updates this keyed cache. */
typedef struct {
    uint16_t palette[256];
    uint16_t red[32], green[64], blue[32];
    uint16_t background, live_bg;
    unsigned opacity;
    bool swapped, valid;
} chord_565_t;

struct round_chord_row {
    round_chord_row_t *next;
    lv_obj_t *obj;
    struct round_shape *shape;
    lv_draw_buf_t source;
    void *pixels;
    size_t bytes;
    uint16_t *source_run_index;
    size_t source_run_index_bytes;
    round_chord_row_t *draw_pose, *lease_source;
    size_t draw_pose_bytes;
    const uint16_t *draw_map;
    unsigned draw_leases;
    uint16_t draw_fill, draw_map_scale;
    bool draw_pose_busy, retired;
    int32_t width, height, current_width;
    uint16_t scale;
    uint16_t minimum_scale;
    uint16_t source_bg, palette_count;
    const uint16_t *palette;
    lv_opa_t opa;
    unsigned count;
    bool capturing, sparse;
    uint32_t signature;
    lv_area_t presented;
    chord_565_t paint;
    uint16_t *insets;
    uint16_t inset_scale;
    int32_t inset_radius;
    round_chord_child_t children[];
};


bool orb_round_source_pack(round_chord_row_t *row);

void orb_round_source_cuts(round_chord_child_t *child);
/* Cold, preparation-time row-source helpers shared with the painter. */
void orb_round_source_index(round_chord_row_t *row);
/* Retained bytes of one prepared row, matching orb_round_list_cache_stats. */
size_t orb_round_source_bytes(const round_chord_row_t *row);
void orb_round_text_ink(round_chord_child_t *child);
/* One shared ellipsis raster per font, ink and tracking; refcounted. */
lv_draw_buf_t *orb_round_text_dots(lv_obj_t *label);
void orb_round_text_dots_release(lv_draw_buf_t *dots);

/* Idle lookahead admission: the display has rendered nothing for a few panel
 * periods, so no fling, spring, transition or animation waits on the UI. */
#define ORB_ROUND_IDLE_MS 50u
void orb_round_idle_watch(lv_obj_t *obj);
bool orb_round_idle(void);
