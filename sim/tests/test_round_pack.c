#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_round_source.h"
#include "orb_ui_private.h"

bool reference_round_source_pack(round_chord_row_t *row);
static unsigned s_checks, s_calls, s_fail_on;
static bool s_failed;
static size_t s_live, s_compared;
static struct { void *pointer; size_t bytes; } s_blocks[16];
#define CHECK(value) do { ++s_checks; if (!(value)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); exit(1); } } while (0)

static void *allocate(size_t size)
{
    if (++s_calls == s_fail_on) { s_failed = true; return NULL; }
    void *pointer = malloc(size);
    CHECK(pointer);
    for (unsigned i = 0; i < 16; ++i) if (!s_blocks[i].pointer) {
        s_blocks[i].pointer = pointer;
        s_blocks[i].bytes = size;
        s_live += size;
        return pointer;
    }
    abort();
}
static void release(void *pointer)
{
    if (!pointer) return;
    for (unsigned i = 0; i < 16; ++i) if (s_blocks[i].pointer == pointer) {
        s_live -= s_blocks[i].bytes;
        s_blocks[i].pointer = NULL;
        free(pointer);
        return;
    }
    abort();
}

static round_chord_row_t *source(unsigned width, unsigned height, unsigned mode, unsigned seed)
{
    round_chord_row_t *row = calloc(1, sizeof(*row) + 4 * sizeof(row->children[0]));
    CHECK(row);
    row->width = width;
    row->height = height;
    row->count = 4;
    row->source_bg = 1;
    row->children[0] = (round_chord_child_t){.x = -2, .y = 1, .width = width / 2,
        .height = height - 2, .pad = 3};
    row->children[1] = (round_chord_child_t){.x = width / 2 + 1, .width = width / 2 - 1, .height = height};
    row->children[2] = (round_chord_child_t){.width = width, .height = height, .hidden = true};
    row->children[3] = (round_chord_child_t){.x = width / 3, .y = height / 2,
        .width = width / 3, .height = height / 2, .pad = 1};
    unsigned stride = (width * 2 + 3) & ~3u;
    row->bytes = stride * height;
    row->pixels = allocate(row->bytes);
    memset(row->pixels, 0x5a, row->bytes);
    lv_draw_buf_init(&row->source, width, height, LV_COLOR_FORMAT_RGB565, stride, row->pixels, row->bytes);
    for (unsigned y = 0; y < height; ++y) {
        uint16_t *line = (uint16_t *)(row->source.data + y * stride);
        for (unsigned x = 0; x < width; ++x) {
            unsigned n = y * width + x + seed * 31;
            bool ink = mode == 4 || ((n * 17 + seed) % 11 < 5);
            unsigned colors = mode == 1 ? 64 : mode == 2 ? 256 : 257;
            line[x] = mode == 0 || !ink ? 1 : 2 + n % colors;
        }
    }
    return row;
}
static void dispose(round_chord_row_t *row) { release(row->pixels); free(row); }

static void exact_case(unsigned width, unsigned height, unsigned mode, unsigned seed)
{
    CHECK(!s_live);
    round_chord_row_t *reference = source(width, height, mode, seed);
    round_chord_row_t *candidate = source(width, height, mode, seed);
    s_fail_on = 0;
    CHECK(reference_round_source_pack(reference));
    CHECK(orb_round_source_pack(candidate));
    CHECK(reference->bytes == candidate->bytes && reference->sparse == candidate->sparse);
    CHECK(reference->palette_count == candidate->palette_count);
    CHECK(!memcmp(reference->pixels, candidate->pixels, reference->bytes));
    s_compared += reference->bytes;
    dispose(reference);
    dispose(candidate);
    CHECK(!s_live);
}

static void allocation_failures(void)
{
    for (unsigned failure = 1; failure <= 2; ++failure) {
        round_chord_row_t *row = source(100, 20, 0, 0);
        uint16_t *pixels = row->pixels;
        pixels[31] = 2;
        size_t bytes = row->bytes;
        s_calls = 0;
        s_fail_on = failure;
        s_failed = false;
        bool result = orb_round_source_pack(row);
        CHECK(result == (failure == 1)); /* Scratch OOM keeps dense; final retained allocation keeps old failure. */
        CHECK(s_failed && !row->sparse && row->pixels == pixels && row->source.data == (uint8_t *)pixels);
        CHECK(row->bytes == bytes && s_live == bytes && pixels[31] == 2);
        s_fail_on = 0;
        dispose(row);
        CHECK(!s_live);
    }
}

int main(void)
{
    lv_init();
    orb_ui_init(&(orb_ui_config_t){.alloc = allocate, .free = release});
    const unsigned widths[] = {5, 17, 64, 127, 418};
    const unsigned heights[] = {3, 9, 72};
    for (unsigned w = 0; w < sizeof(widths) / sizeof(widths[0]); ++w)
        for (unsigned h = 0; h < sizeof(heights) / sizeof(heights[0]); ++h)
            for (unsigned mode = 0; mode < 5; ++mode)
                for (unsigned seed = 0; seed < 8; ++seed) exact_case(widths[w], heights[h], mode, seed);
    allocation_failures();
    printf("round pack: %u checks; %zu exact packed/dense bytes compared\n", s_checks, s_compared);
    lv_deinit();
    return 0;
}
