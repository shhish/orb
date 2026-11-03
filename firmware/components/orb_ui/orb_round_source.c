#include "orb_round_source.h"
#include "orb_ui_private.h"

#include <string.h>

/* A native snapshot is mostly matte, especially at AX3. Retain only ink runs
 * with a per-scanline offset index. This is lossless RGB565, prepared off the
 * scroll/draw path; dense sources keep the original format when it is smaller.
 * The compositor paints the live matte once, then reads these forward-only
 * runs. A long Sessions list must not retain megabytes of blank row pixels. */
static void chord_source_span(const round_chord_row_t *r, int y, int *left, int *right)
{
    *left = r->width; *right = 0;
    for (unsigned i = 0; i < r->count; i++) {
        const round_chord_child_t *c = &r->children[i];
        if (c->hidden || y < c->y - c->pad || y >= c->y + c->height + c->pad) continue;
        *left = LV_MIN(*left, LV_MAX(0, c->x - c->pad));
        *right = LV_MAX(*right, LV_MIN(r->width, c->x + c->width + c->pad));
    }
}

static unsigned chord_palette_slot(uint16_t color, const uint16_t keys[512], const uint16_t values[512])
{
    unsigned slot = ((uint32_t)color * 2654435761u) >> 23;
    while (values[slot] && keys[slot] != color) slot = (slot + 1) & 511;
    return slot;
}

typedef struct {
    uint16_t palette[256], keys[512], values[512];
} chord_pack_scratch_t;

static bool chord_source_pack_with_scratch(round_chord_row_t *r, chord_pack_scratch_t *scratch)
{
    /* Inter's cached antialiasing and the flat identity fills use a small
     * colour set. An exact per-row palette halves ink storage again; noisy
     * artwork falls back to RGB565 without changing its colours. */
    uint16_t *palette = scratch->palette, *keys = scratch->keys, *values = scratch->values;
    memset(values, 0, sizeof(scratch->values));
    unsigned colors = 0;
    bool indexed = true;
    /* Count both exact encodings while collecting palette entries. Choosing
     * after the complete pass preserves late palette-overflow fallback without
     * a second source scan merely to determine the retained allocation size. */
    size_t indexed_runs = 0, raw_runs = 0;
    /* Glyph stems and flat fills repeat one colour across neighbouring pixels.
     * The background never enters the palette, so it is a safe empty memo; a
     * repeated colour is already present (or already overflowed), leaving the
     * first-occurrence palette order unchanged. */
    uint16_t seen = r->source_bg;
    for (int y = 0; y < r->height; y++) {
        const uint16_t *line = (const uint16_t *)(r->source.data + y * r->source.header.stride);
        int left, right; chord_source_span(r, y, &left, &right);
        for (int x = left; x < right;) {
            if (line[x] == r->source_bg) { x++; continue; }
            int start = x;
            while (x < right && line[x] != r->source_bg) {
                if (indexed && line[x] != seen) {
                    seen = line[x];
                    unsigned slot = chord_palette_slot(line[x], keys, values);
                    if (!values[slot]) {
                        if (colors == 256) indexed = false;
                        else { palette[colors++] = line[x]; keys[slot] = line[x]; values[slot] = colors; }
                    }
                }
                x++;
            }
            raw_runs += 2 * sizeof(uint16_t) + (size_t)(x - start) * 2;
            indexed_runs += 2 * sizeof(uint16_t) + (size_t)(x - start + 1) / 2 * 2;
        }
    }
    if (!indexed) colors = 0;
    size_t index_bytes = (r->height + 1) * sizeof(uint32_t);
    size_t bytes = index_bytes + colors * sizeof(uint16_t) + (indexed ? indexed_runs : raw_runs);
    if (bytes >= r->bytes) return true;
    uint32_t *index = orb_ui_alloc(bytes);
    if (!index) return false;
    uint16_t *retained_palette = (uint16_t *)((uint8_t *)index + index_bytes);
    memcpy(retained_palette, palette, colors * sizeof(uint16_t));
    uint16_t *runs = retained_palette + colors, *out = runs;
    seen = r->source_bg;
    uint8_t seen_index = 0;
    for (int y = 0; y < r->height; y++) {
        index[y] = (uint32_t)(out - runs);
        const uint16_t *line = (const uint16_t *)(r->source.data + y * r->source.header.stride);
        int left, right; chord_source_span(r, y, &left, &right);
        for (int x = left; x < right;) {
            if (line[x] == r->source_bg) { x++; continue; }
            int start = x++;
            while (x < right && line[x] != r->source_bg) x++;
            *out++ = start; *out++ = x - start;
            if (indexed) {
                uint8_t *pixels = (uint8_t *)out;
                for (int px = start; px < x; px++) {
                    if (line[px] != seen) {
                        seen = line[px];
                        seen_index = values[chord_palette_slot(line[px], keys, values)] - 1;
                    }
                    pixels[px - start] = seen_index;
                }
                if ((x - start) & 1) pixels[x - start] = 0;
                out += (x - start + 1) / 2;
            } else {
                memcpy(out, line + start, (x - start) * sizeof(*out));
                out += x - start;
            }
        }
    }
    index[r->height] = (uint32_t)(out - runs);
    orb_ui_free(r->pixels);
    r->pixels = index; r->bytes = bytes; r->sparse = true;
    r->palette = retained_palette; r->palette_count = colors;
    r->source.data = NULL;
    return true;
}


bool orb_round_source_pack(round_chord_row_t *r)
{
    /* Cold capture is nested beneath scene construction on the UI task. Keep
     * its 2.5 KiB palette/hash scratch in on-demand PSRAM with no internal or
     * stack fallback. OOM retains the exact dense source already captured. */
    chord_pack_scratch_t *scratch = orb_ui_alloc(sizeof(*scratch));
    if (!scratch) return true;
    bool result = chord_source_pack_with_scratch(r, scratch);
    orb_ui_free(scratch);
    return result;
}

/* HIG §§3/7: sources and their child copy lanes stay fixed while scrolling.
 * Seed each lane at its first possible ink run rather than decoding all earlier
 * children again for every band. Only preparation allocates this optional
 * index; an allocation failure retains the exact unindexed painter. */
void orb_round_source_index(round_chord_row_t *r)
{
    if (!r->sparse) return;
    const uint32_t *lines = r->pixels;
    const uint16_t *runs = (const uint16_t *)(lines + r->height + 1) + r->palette_count;
    /* Offsets are relative to one scanline. Preserve the general source
     * fallback rather than truncating an unusually wide caller's offset. */
    for (int y = 0; y < r->height; ++y)
        if (lines[y + 1] - lines[y] > UINT16_MAX) return;
    size_t entries = 0;
    for (unsigned i = 0; i < r->count; ++i) {
        const round_chord_child_t *c = &r->children[i];
        if (c->hidden) continue;
        int first_y = LV_MAX(0, c->y - c->pad);
        int last_y = LV_MIN(r->height, c->y + c->height + c->pad);
        if (first_y < last_y) entries += last_y - first_y;
    }
    if (!entries) return;
    uint16_t *index = orb_ui_alloc(entries * sizeof(*index));
    if (!index) return;
    uint16_t *out = index;
    for (unsigned i = 0; i < r->count; ++i) {
        round_chord_child_t *c = &r->children[i];
        if (c->hidden) continue;
        int first_y = LV_MAX(0, c->y - c->pad);
        int last_y = LV_MIN(r->height, c->y + c->height + c->pad);
        if (first_y >= last_y) continue;
        c->source_runs = out;
        int first_x = LV_MAX(0, c->x - c->pad);
        for (int y = first_y; y < last_y; ++y) {
            const uint16_t *first = runs + lines[y], *next = first, *end = runs + lines[y + 1];
            while (next < end) {
                unsigned start = next[0], length = next[1];
                if ((int)(start + length) > first_x) break;
                next += 2 + (r->palette_count ? (length + 1) / 2 : length);
            }
            *out++ = (uint16_t)(next - first);
        }
    }
    r->source_run_index = index;
    r->source_run_index_bytes = entries * sizeof(*index);
}

/* Retained bytes of one prepared row, matching orb_round_list_cache_stats. */
size_t orb_round_source_bytes(const round_chord_row_t *r)
{
    size_t bytes = r->bytes + sizeof(*r) + r->source_run_index_bytes +
        r->count * sizeof(r->children[0]) + (size_t)r->height * sizeof(uint16_t);
    for (unsigned i = 0; i < r->count; i++)
        if (r->children[i].cut) bytes += CHORD_TEXT_BUCKETS * r->children[i].lines * sizeof(uint16_t);
    return bytes;
}

/* Lookahead is admitted only after the display has rendered nothing for a few
 * panel periods: no fling, settle spring, navigation transition or animation
 * is waiting for the UI task. Moving frames therefore never pay for it. */
static lv_display_t *s_chord_render_display;
static uint32_t s_chord_render_tick;

static void chord_render_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RENDER_START) s_chord_render_tick = lv_tick_get();
    else if (code == LV_EVENT_DELETE) s_chord_render_display = NULL;
}

void orb_round_idle_watch(lv_obj_t *obj)
{
    lv_display_t *display = lv_obj_get_display(obj);
    if (!display || display == s_chord_render_display) return;
    if (s_chord_render_display)
        lv_display_remove_event_cb_with_user_data(s_chord_render_display, chord_render_event, NULL);
    s_chord_render_display = lv_display_add_event_cb(display, chord_render_event, LV_EVENT_ALL, NULL) ?
        display : NULL;
    s_chord_render_tick = lv_tick_get();
}

bool orb_round_idle(void)
{
    return s_chord_render_display && lv_tick_elaps(s_chord_render_tick) >= ORB_ROUND_IDLE_MS;
}
