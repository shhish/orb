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

bool reference_round_source_pack(round_chord_row_t *r)
{
    /* Inter's cached antialiasing and the flat identity fills use a small
     * colour set. An exact per-row palette halves ink storage again; noisy
     * artwork falls back to RGB565 without changing its colours. */
    uint16_t palette[256], keys[512], values[512] = {0};
    unsigned colors = 0;
    bool indexed = true;
    for (int y = 0; y < r->height && indexed; y++) {
        const uint16_t *line = (const uint16_t *)(r->source.data + y * r->source.header.stride);
        int left, right; chord_source_span(r, y, &left, &right);
        for (int x = left; x < right; x++) {
            if (line[x] == r->source_bg) continue;
            unsigned slot = chord_palette_slot(line[x], keys, values);
            if (values[slot]) continue;
            if (colors == 256) { indexed = false; break; }
            palette[colors++] = line[x]; keys[slot] = line[x]; values[slot] = colors;
        }
    }
    if (!indexed) colors = 0;
    size_t index_bytes = (r->height + 1) * sizeof(uint32_t);
    size_t bytes = index_bytes + colors * sizeof(uint16_t);
    for (int y = 0; y < r->height; y++) {
        const uint16_t *line = (const uint16_t *)(r->source.data + y * r->source.header.stride);
        int left, right; chord_source_span(r, y, &left, &right);
        for (int x = left; x < right;) {
            if (line[x] == r->source_bg) { x++; continue; }
            int start = x++;
            while (x < right && line[x] != r->source_bg) x++;
            bytes += 2 * sizeof(uint16_t) + (indexed ? (size_t)(x - start + 1) / 2 * 2 :
                                                      (size_t)(x - start) * 2);
        }
    }
    if (bytes >= r->bytes) return true;
    uint32_t *index = orb_ui_alloc(bytes);
    if (!index) return false;
    uint16_t *retained_palette = (uint16_t *)((uint8_t *)index + index_bytes);
    memcpy(retained_palette, palette, colors * sizeof(uint16_t));
    uint16_t *runs = retained_palette + colors, *out = runs;
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
                for (int px = start; px < x; px++)
                    pixels[px - start] = values[chord_palette_slot(line[px], keys, values)] - 1;
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

