#include "orb_nav_compositor_private.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef __XTENSA__
#include "orb_simd_copy.h"
#endif

#define PI_F 3.14159265358979323846f
#define STEP_SECONDS (1.0f / 240.0f)
#define IRIS_OPEN_MS 280u
#define IRIS_CLOSE_MS 240u
#define LAYER_MS 260u

static float clampf(float value, float low, float high)
{
    return value < low ? low : value > high ? high : value;
}

static uint16_t blend(uint16_t a, uint16_t b, unsigned amount)
{
    unsigned inv = 256 - amount;
    unsigned r = (((a >> 11) * inv + (b >> 11) * amount + 128) >> 8);
    unsigned g = ((((a >> 5) & 63) * inv + ((b >> 5) & 63) * amount + 128) >> 8);
    unsigned bl = (((a & 31) * inv + (b & 31) * amount + 128) >> 8);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static bool prepare_background(orb_nav_compositor_t *c)
{
    unsigned slot = 0;
    while (slot == c->from || slot == c->to) slot++;
    if (!orb_nav_compositor_admit_slot(c, slot)) return false;
    /* HIG §6: depth is prepared once. Drag frames translate two immutable
     * planes; they never rescale a whole screen or rebuild its controls. */
    unsigned source = c->kind == ORB_NAV_TRANSITION_BACK ||
        (c->kind == ORB_NAV_TRANSITION_IRIS && c->reverse) ? c->to : c->from;
    /* Forward/reverse iris share the dimmed plane across descriptor swaps. */
    if (c->prepared_valid && c->prepared == slot && c->prepared_kind == c->kind &&
        c->prepared_source == c->slots[source].pixels &&
        c->prepared_revision == c->slots[source].revision) {
        c->stats.background_hits++;
        return true;
    }
    c->prepared = slot;
    c->prepared_valid = true;
    c->prepared_kind = c->kind;
    c->prepared_source = c->slots[source].pixels;
    c->prepared_revision = c->slots[source].revision;
    c->stats.background_prepares++;
    snapshot_t *s = &c->slots[slot];
    if (c->kind == ORB_NAV_TRANSITION_IRIS) {
        /* HIG §§1/6: a uniform, slight dim separates the launch place from
         * the revealed destination. Prepare it once, without blur or a glow;
         * the aperture itself is the only moving boundary. */
        for (size_t i = 0; i < (size_t)c->width * c->height; i++)
            s->pixels[i] = blend(0, c->slots[source].pixels[i], 205);
        s->valid = false; s->capturing = false;
        return true;
    }
    float scale = c->kind == ORB_NAV_TRANSITION_BACK ? 0.96f : 0.92f;
    /* A sheet arrives over a clearly receded place (polish #4): 40%, so the
     * slab's title and controls lead and the Face's clock reads as depth. */
    unsigned opacity = c->kind == ORB_NAV_TRANSITION_BACK ? 256 : 102;
    float cx = (c->width - 1) * .5f, cy = (c->height - 1) * .5f;
    int16_t source_x[1024];
    for (unsigned x = 0; x < c->width; x++) source_x[x] = (int16_t)lroundf((x - cx) / scale + cx);
    for (unsigned y = 0; y < c->height; y++) {
        int sy = (int)lroundf((y - cy) / scale + cy);
        for (unsigned x = 0; x < c->width; x++) {
            int sx = source_x[x];
            uint16_t value = 0;
            if (sx >= 0 && sx < c->width && sy >= 0 && sy < c->height)
                value = c->slots[source].pixels[(size_t)sy * c->width + sx];
            s->pixels[(size_t)y * c->width + x] = opacity == 256 ? value : blend(0, value, opacity);
        }
    }
    s->valid = false; s->capturing = false;
    return true;
}

bool orb_nav_compositor_begin(orb_nav_compositor_t *c, orb_nav_transition_kind_t kind,
                             unsigned from, unsigned to, orb_nav_direction_t direction,
                             const orb_nav_token_rect_t *token, bool reverse, bool reduce_motion)
{
    if (!slot_exists(c, from) || !slot_exists(c, to) ||
        (from == to && kind != ORB_NAV_TRANSITION_BOUNDARY) ||
        !c->slots[from].valid || !c->slots[to].valid) return false;
    c->active = false;
    c->from = from; c->to = to; c->kind = kind; c->direction = direction;
    c->reverse = reverse; c->reduced_motion = reduce_motion;
    c->glyph_visible = false;
    c->token = token ? *token : (orb_nav_token_rect_t){(int16_t)(c->width / 2), (int16_t)(c->height / 2), 1, 1};
    c->position = c->velocity = c->target = c->accumulator = c->fade_start = 0;
    c->release_start = 0;
    c->fade_elapsed = 0;
    if (!reduce_motion && (kind == ORB_NAV_TRANSITION_SHEET || kind == ORB_NAV_TRANSITION_BACK ||
                          kind == ORB_NAV_TRANSITION_IRIS) &&
        !prepare_background(c)) return false;
    c->active = c->dragging = true;
    return true;
}

float orb_nav_compositor_reverse_iris_round_start(orb_nav_compositor_t *c)
{
    if (!c) return 0;
    c->release_start = 0;
    if (c->kind != ORB_NAV_TRANSITION_IRIS || !c->reverse || c->reduced_motion || c->glyph_visible) return 0;
    float cx = c->token.x + c->token.width * .5f, cy = c->token.y + c->token.height * .5f;
    float far_x = fmaxf(cx, c->width - 1 - cx), far_y = fmaxf(cy, c->height - 1 - cy);
    float maximum = hypotf(far_x, far_y) + 1;
    float initial = fmaxf(c->token.width, c->token.height) * .5f;
    if (maximum <= initial) return 0;
    /* A panel-edge pixel with nonzero coverage has its centre inside R+.5.
     * Include another pixel for the aperture's AA edge and .125px for float
     * reconstruction. A cancelled drag must return to source-identical ink. */
    float panel_radius = fminf(c->width, c->height) * .5f + .5f;
    float distance = hypotf(cx - (c->width - 1) * .5f, cy - (c->height - 1) * .5f) + panel_radius;
    /* With no decorative rim, a geometric aperture can traverse only black
     * canvas for several frames. Skip that source-identical interval too:
     * compare immutable planes once at admission, never during rendering. */
    const uint16_t *source = c->slots[c->from].pixels;
    const uint16_t *backdrop = c->slots[c->prepared].pixels;
    int32_t token_x2 = c->token.x * 2 + c->token.width;
    int32_t token_y2 = c->token.y * 2 + c->token.height;
    int64_t panel_diameter = (c->width < c->height ? c->width : c->height) + 1;
    uint64_t farthest_square = 0;
    bool differs = false;
    for (unsigned y = 0; y < c->height; y++) {
        int64_t panel_y2 = (int64_t)y * 2 - (c->height - 1);
        int64_t token_dy2 = (int64_t)y * 2 - token_y2;
        for (unsigned x = 0; x < c->width; x++) {
            size_t index = (size_t)y * c->width + x;
            if (source[index] == backdrop[index]) continue;
            int64_t panel_x2 = (int64_t)x * 2 - (c->width - 1);
            if (panel_x2 * panel_x2 + panel_y2 * panel_y2 >= panel_diameter * panel_diameter) continue;
            int64_t token_dx2 = (int64_t)x * 2 - token_x2;
            uint64_t square = (uint64_t)(token_dx2 * token_dx2 + token_dy2 * token_dy2);
            if (square > farthest_square) farthest_square = square;
            differs = true;
        }
    }
    if (differs) distance = sqrtf((float)farthest_square) * .5f;
    float invisible_radius = distance + 1.125f;
    if (invisible_radius <= initial) return 0;
    c->release_start = clampf((maximum - invisible_radius) / (maximum - initial), 0, 1);
    return c->release_start;
}

static int luminance(uint16_t pixel)
{
    int r = ((pixel >> 11) * 255 + 15) / 31;
    int g = (((pixel >> 5) & 63) * 255 + 31) / 63;
    int b = ((pixel & 31) * 255 + 15) / 31;
    return (54 * r + 183 * g + 19 * b + 128) >> 8;
}

bool orb_nav_compositor_token_glyph(orb_nav_compositor_t *c,
                                  const orb_nav_token_rect_t *header, uint16_t ink)
{
    if (!c || !c->active || c->kind != ORB_NAV_TRANSITION_IRIS || !header ||
        c->reduced_motion || c->token.width < 8 || c->token.height < 8 ||
        header->width <= 0 || header->height <= 0) return false;
    c->glyph_visible = false;
    int width = c->token.width * 7 / 10, height = c->token.height * 7 / 10;
    int x = c->token.x + (c->token.width - width) / 2;
    int y = c->token.y + (c->token.height - height) / 2;
    if (width > 96 || height > 96 || x < 0 || y < 0 || x + width > c->width || y + height > c->height) return false;
    size_t bytes = (size_t)width * height;
    if (bytes > c->glyph_capacity) {
        uint8_t *glyph = c->allocate(bytes, c->allocator_context);
        if (!glyph) return false;
        if (c->glyph) c->release(c->glyph, c->allocator_context);
        c->glyph = glyph; c->glyph_capacity = bytes; c->stats.sprite_bytes = bytes;
    }
    const uint16_t *source = c->slots[c->reverse ? c->to : c->from].pixels;
    int sample_x = c->token.x + c->token.width * 9 / 10;
    int sample_y = c->token.y + c->token.height / 2;
    if (sample_x < 0 || sample_x >= c->width || sample_y < 0 || sample_y >= c->height) return false;
    int background = luminance(source[(size_t)sample_y * c->width + sample_x]);
    int glyph_luminance = background > 127 ? 0 : 255;
    int left = width, top = height, right = -1, bottom = -1;
    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            int value = luminance(source[(size_t)(y + row) * c->width + x + col]);
            int alpha = (value - background) * 255 / (glyph_luminance - background);
            alpha = alpha < 0 ? 0 : alpha > 255 ? 255 : alpha;
            c->glyph[(size_t)row * width + col] = (uint8_t)alpha;
            if (alpha > 12) {
                if (col < left) left = col;
                if (col > right) right = col;
                if (row < top) top = row;
                if (row > bottom) bottom = row;
            }
        }
    }
    if (right < left || bottom < top) return false;
    int cropped_width = right - left + 1, cropped_height = bottom - top + 1;
    for (int row = 0; row < cropped_height; row++)
        memmove(c->glyph + (size_t)row * cropped_width, c->glyph + (size_t)(row + top) * width + left, cropped_width);
    c->glyph_source = (orb_nav_token_rect_t){x + left, y + top, cropped_width, cropped_height};
    c->glyph_target = *header; c->glyph_ink = ink;
    c->glyph_visible = true;
    return true;
}

void orb_nav_compositor_drag(orb_nav_compositor_t *c, float progress)
{
    if (!c || !c->active || !isfinite(progress)) return;
    c->dragging = true; c->velocity = 0; c->accumulator = 0;
    /* The first pixels remain 1:1. Only movement beyond either end rubber-bands. */
    if (progress < 0) progress = -.55f * -progress / (1 + .55f * -progress);
    else if (progress > 1) progress = 1 + .55f * (progress - 1) / (1 + .55f * (progress - 1));
    c->position = progress;
}

void orb_nav_compositor_release(orb_nav_compositor_t *c, bool commit, float velocity_per_second)
{
    if (!c || !c->active) return;
    if (c->kind == ORB_NAV_TRANSITION_BOUNDARY) commit = false;
    c->dragging = false; c->target = commit ? 1 : c->release_start;
    c->velocity = isfinite(velocity_per_second) ? clampf(velocity_per_second, -8, 8) : 0;
    c->fade_elapsed = 0; c->fade_start = clampf(c->position, 0, 1);
}

void orb_nav_compositor_retarget(orb_nav_compositor_t *c, bool commit)
{
    if (!c || !c->active) return;
    if (c->kind == ORB_NAV_TRANSITION_BOUNDARY) commit = false;
    c->dragging = false; c->target = commit ? 1 : c->release_start;
    c->fade_elapsed = 0; c->fade_start = clampf(c->position, 0, 1);
}

bool orb_nav_compositor_tick(orb_nav_compositor_t *c, uint32_t elapsed_ms)
{
    if (!c || !c->active || c->dragging || !elapsed_ms) return false;
    float before = c->position;
    if (c->reduced_motion || c->kind == ORB_NAV_TRANSITION_FADE) {
        /* HIG §6: a 150ms cross-fade for the whole change. A release partway
         * through a tracked fade finishes the remaining share of it. */
        c->fade_elapsed += elapsed_ms;
        float duration = fmaxf(16.f, 150.f * fabsf(c->target - c->fade_start));
        float t = fminf(1, c->fade_elapsed / duration);
        c->position = c->fade_start + (c->target - c->fade_start) * t;
        if (t == 1) c->active = false;
    } else {
        /* Main.dc.html: bounded 280/240 ms iris and 260 ms layer motion.
         * Keep HIG §6's velocity-preserving spring and 240Hz integration;
         * its terminal tail settles at the table's perceptual deadline. */
        uint32_t duration = c->kind == ORB_NAV_TRANSITION_IRIS ?
            (c->reverse ? IRIS_CLOSE_MS : IRIS_OPEN_MS) :
            c->kind == ORB_NAV_TRANSITION_SHEET || c->kind == ORB_NAV_TRANSITION_BACK ? LAYER_MS : 0;
        float response = duration ? duration / 1000.f : .45f;
        float omega = 2 * PI_F / response;
        c->fade_elapsed += elapsed_ms;
        c->accumulator += fminf(elapsed_ms / 1000.f, .25f);
        while (c->accumulator >= STEP_SECONDS) {
            float acceleration = omega * omega * (c->target - c->position) - 1.8f * omega * c->velocity;
            c->velocity += acceleration * STEP_SECONDS;
            c->position += c->velocity * STEP_SECONDS;
            c->accumulator -= STEP_SECONDS;
        }
        if ((duration && c->fade_elapsed >= duration) ||
            (fabsf(c->position - c->target) < .0008f && fabsf(c->velocity) < .008f)) {
            c->position = c->target; c->velocity = 0; c->active = false;
        }
    }
    return before != c->position;
}

void orb_nav_compositor_cancel(orb_nav_compositor_t *c)
{
    if (c) { c->active = c->dragging = false; c->position = c->target = c->velocity = 0; }
}

bool orb_nav_compositor_active(const orb_nav_compositor_t *c) { return c && c->active; }
float orb_nav_compositor_progress(const orb_nav_compositor_t *c) { return c ? c->position : 0; }
bool orb_nav_compositor_committed(const orb_nav_compositor_t *c)
{
    return c && c->kind != ORB_NAV_TRANSITION_BOUNDARY && c->position >= .999f;
}

static void copy_span(uint8_t *dst, const uint16_t *src, int count, bool swapped)
{
    if (count <= 0) return;
#ifdef __XTENSA__
    orb_simd_copy_rgb565(dst, src, count);
#else
    /* The simulator's vectorizing compiler can fuse the ordinary host swap.
     * S3 instead needs the explicit PSRAM SIMD load/internal-word path below. */
    if (!swapped) memcpy(dst, src, (size_t)count * 2);
    else for (int i = 0; i < count; i++) { dst[i * 2] = src[i] >> 8; dst[i * 2 + 1] = src[i] & 255; }
    return;
#endif
    if (!swapped) return;
    /* IDF disables builtin memcpy. Read PSRAM once in SIMD bursts, then
     * swap alias-safe words in internal RAM without per-pixel calls. */
    if ((uintptr_t)dst & 1) {
        /* A two-byte peel cannot align an odd capture address. */
        while (count--) {
            uint8_t byte = dst[0]; dst[0] = dst[1]; dst[1] = byte;
            dst += 2;
        }
        return;
    }
    if (count && ((uintptr_t)dst & 2)) {
        uint8_t byte = dst[0]; dst[0] = dst[1]; dst[1] = byte;
        dst += 2; count--;
    }
    typedef uint32_t __attribute__((__may_alias__)) swap_word_t;
    swap_word_t *words = (swap_word_t *)dst;
    while (count >= 2) {
        uint32_t pair = *words;
        pair = ((pair & 0xFF00FF00u) >> 8) | ((pair & 0x00FF00FFu) << 8);
        *words++ = pair;
        count -= 2;
    }
    dst = (uint8_t *)words;
    if (count) { uint8_t byte = dst[0]; dst[0] = dst[1]; dst[1] = byte; }
}

static void put_pixel(uint8_t *dst, uint16_t pixel, bool swapped)
{
    if (!swapped) memcpy(dst, &pixel, 2);
    else { dst[0] = pixel >> 8; dst[1] = pixel & 255; }
}

static void glyph_row_scalar(const orb_nav_compositor_t *c, float progress, int x, int y,
                      int width, uint8_t *dst, bool swapped)
{
    if (!c->glyph_visible || progress <= 0 || progress >= 1) return;
    float t = c->reverse ? 1 - progress : progress;
    /* HIG §§1/6: establish the header before the aperture reveals readable
     * rows. A full-duration flight puts the token on the agent's last step
     * and headline; early arrival preserves identity without covering them. */
    t = fminf(1, t * 3);
    const orb_nav_token_rect_t *a = &c->glyph_source, *b = &c->glyph_target;
    float gx = a->x + (b->x - a->x) * t, gy = a->y + (b->y - a->y) * t;
    float gw = a->width + (b->width - a->width) * t;
    float gh = a->height + (b->height - a->height) * t;
    if (y + 1 <= gy || y >= gy + gh) return;
    int left = (int)floorf(gx), right = (int)ceilf(gx + gw) - 1;
    if (left < x) left = x;
    if (right >= x + width) right = x + width - 1;
    float sy = clampf((y - gy + .5f) * a->height / gh - .5f, 0, a->height - 1);
    int y0 = (int)sy, y1 = y0 + 1 < a->height ? y0 + 1 : y0;
    float fy = sy - y0;
    float cover_y = fminf(y + 1, gy + gh) - fmaxf(y, gy);
    /* The flat launch disc gives way to black as soon as the iris starts.
     * Use the header's contrast-safe ink throughout that flight, so a black
     * launcher glyph does not disappear against the destination canvas. */
    uint16_t ink = c->glyph_ink;
    for (int col = left; col <= right; col++) {
        float sx = clampf((col - gx + .5f) * a->width / gw - .5f, 0, a->width - 1);
        int x0 = (int)sx, x1 = x0 + 1 < a->width ? x0 + 1 : x0;
        float fx = sx - x0;
        float top = c->glyph[(size_t)y0 * a->width + x0] * (1 - fx) +
                    c->glyph[(size_t)y0 * a->width + x1] * fx;
        float bottom = c->glyph[(size_t)y1 * a->width + x0] * (1 - fx) +
                       c->glyph[(size_t)y1 * a->width + x1] * fx;
        float cover_x = fminf(col + 1, gx + gw) - fmaxf(col, gx);
        unsigned alpha = (unsigned)lroundf((top * (1 - fy) + bottom * fy) * cover_x * cover_y * 256 / 255);
        if (!alpha) continue;
        uint8_t *pixel = dst + (size_t)(col - x) * 2;
        uint16_t background;
        if (swapped) background = (uint16_t)((pixel[0] << 8) | pixel[1]);
        else memcpy(&background, pixel, 2);
        put_pixel(pixel, blend(background, ink, alpha), swapped);
    }
}

#define GLYPH_COLUMNS 96
#define GLYPH_ONE 32768
#define GLYPH_ROUND_GUARD 1311 /* .0401 alpha units, exceeds Q15 interpolation error. */
typedef struct {
    float fraction, coverage;
    uint16_t first, second, fraction_q15, coverage_q15;
} glyph_column_t;

typedef struct {
    glyph_column_t columns[GLYPH_COLUMNS];
    float y, height;
    int left, right;
    bool ready;
} glyph_frame_t;

static void glyph_prepare(const orb_nav_compositor_t *c, float progress,
                          int y, int height, glyph_frame_t *frame)
{
    frame->ready = false;
    if (!c->glyph_visible || progress <= 0 || progress >= 1) return;
    float t = fminf(1, (c->reverse ? 1 - progress : progress) * 3);
    const orb_nav_token_rect_t *a = &c->glyph_source, *b = &c->glyph_target;
    float gx = a->x + (b->x - a->x) * t;
    float width = a->width + (b->width - a->width) * t;
    frame->y = a->y + (b->y - a->y) * t;
    frame->height = a->height + (b->height - a->height) * t;
    frame->left = (int)floorf(gx); frame->right = (int)ceilf(gx + width) - 1;
    if (frame->right - frame->left + 1 > GLYPH_COLUMNS) return;
    frame->ready = true;
    if (y + height <= frame->y || y >= frame->y + frame->height) return;
    for (int col = frame->left; col <= frame->right; col++) {
        glyph_column_t *column = &frame->columns[col - frame->left];
        float sx = clampf((col - gx + .5f) * a->width / width - .5f, 0, a->width - 1);
        column->first = (int)sx;
        column->second = column->first + 1 < a->width ? column->first + 1 : column->first;
        column->fraction = sx - column->first;
        column->coverage = fminf(col + 1, gx + width) - fmaxf(col, gx);
        column->fraction_q15 = (unsigned)lroundf(column->fraction * GLYPH_ONE);
        column->coverage_q15 = (unsigned)lroundf(column->coverage * GLYPH_ONE);
    }
}

static void glyph_row(const orb_nav_compositor_t *c, const glyph_frame_t *frame,
                      float progress, int x, int y, int width, uint8_t *dst, bool swapped)
{
    if (!frame->ready) {
        glyph_row_scalar(c, progress, x, y, width, dst, swapped);
        return;
    }
    float gy = frame->y, gh = frame->height;
    if (y + 1 <= gy || y >= gy + gh) return;
    const orb_nav_token_rect_t *a = &c->glyph_source;
    int left = frame->left > x ? frame->left : x;
    int right = frame->right < x + width - 1 ? frame->right : x + width - 1;
    float sy = clampf((y - gy + .5f) * a->height / gh - .5f, 0, a->height - 1);
    int y0 = (int)sy, y1 = y0 + 1 < a->height ? y0 + 1 : y0;
    float fy = sy - y0, cover_y = fminf(y + 1, gy + gh) - fmaxf(y, gy);
    unsigned fy_q15 = (unsigned)lroundf(fy * GLYPH_ONE);
    unsigned cover_y_q15 = (unsigned)lroundf(cover_y * GLYPH_ONE);
    const uint8_t *top_row = c->glyph + (size_t)y0 * a->width;
    const uint8_t *bottom_row = c->glyph + (size_t)y1 * a->width;
    for (int col = left; col <= right; col++) {
        const glyph_column_t *column = &frame->columns[col - frame->left];
        int x0 = column->first, x1 = column->second, fx = column->fraction_q15;
        int top = top_row[x0] * (GLYPH_ONE - fx) + top_row[x1] * fx;
        int bottom = bottom_row[x0] * (GLYPH_ONE - fx) + bottom_row[x1] * fx;
        int value = top + (int)(((int64_t)(bottom - top) * fy_q15 + GLYPH_ONE / 2) >> 15);
        value = (int)(((int64_t)value * column->coverage_q15 + GLYPH_ONE / 2) >> 15);
        value = (int)(((int64_t)value * cover_y_q15 + GLYPH_ONE / 2) >> 15);
        unsigned alpha_q15 = (unsigned)(value * 256 / 255);
        unsigned alpha = (alpha_q15 + GLYPH_ONE / 2) >> 15;
        /* Preserve the existing float oracle only where fixed-point sampling
         * could cross lroundf's half-way point. The ordinary glyph interior
         * uses integer interpolation and precomputed column maps. */
        if (abs((int)(alpha_q15 & (GLYPH_ONE - 1)) - GLYPH_ONE / 2) <= GLYPH_ROUND_GUARD) {
            float fraction = column->fraction;
            float top_f = top_row[x0] * (1 - fraction) + top_row[x1] * fraction;
            float bottom_f = bottom_row[x0] * (1 - fraction) + bottom_row[x1] * fraction;
            alpha = (unsigned)lroundf((top_f * (1 - fy) + bottom_f * fy) *
                column->coverage * cover_y * 256 / 255);
        }
        if (!alpha) continue;
        uint8_t *pixel = dst + (size_t)(col - x) * 2;
        uint16_t background;
        if (swapped) background = (uint16_t)((pixel[0] << 8) | pixel[1]);
        else memcpy(&background, pixel, 2);
        put_pixel(pixel, blend(background, c->glyph_ink, alpha), swapped);
    }
}

static void translated_row(const orb_nav_compositor_t *c, unsigned slot, int dx, int dy,
                           int x, int y, int width, uint8_t *dst, bool swapped)
{
    int sy = y - dy;
    if (sy < 0 || sy >= c->height) return;
    int left = x > dx ? x : dx;
    int right = x + width < dx + c->width ? x + width : dx + c->width;
    if (left >= right) return;
    copy_span(dst + (size_t)(left - x) * 2,
              c->slots[slot].pixels + (size_t)sy * c->width + left - dx, right - left, swapped);
}

/* HIG §6 / polish #3-#4: translate the complete rectangular plane. The
 * hardware bezel supplies the only circular clip; a second moving disc mask
 * cuts controls into wedges and reveals the underlying place through corners. */
static void overlay_row(const orb_nav_compositor_t *c, unsigned slot, int dx, int dy,
                        int x, int y, int width, uint8_t *dst, bool swapped)
{
    const uint16_t *background = c->slots[c->prepared].pixels + (size_t)y * c->width + x;
    int sy = y - dy;
    int left = x > dx ? x : dx;
    int right = x + width < dx + c->width ? x + width : dx + c->width;
    if (sy < 0 || sy >= c->height || left >= right) {
        copy_span(dst, background, width, swapped);
        return;
    }
    /* Immutable planes permit disjoint spans: one PSRAM read per pixel. */
    copy_span(dst, background, left - x, swapped);
    translated_row(c, slot, dx, dy, x, y, width, dst, swapped);
    copy_span(dst + (size_t)(right - x) * 2, background + right - x,
              x + width - right, swapped);
}

/* Plan only the row about to be copied; no mask, extra plane or arena.
 * Polish #34: the aperture is its own antialiased edge. The former 1.75px
 * grey hairline outlined the iris without meaning and cost a blend per rim
 * pixel; the dimmed launch place already separates the two layers. */
typedef struct {
    int left, right, edge_left, edge_right;
    uint16_t alpha_left, alpha_right;
    unsigned source; /* 0: boundary, 1: whole outer, 2: whole inner. */
} iris_row_t;

typedef struct {
    float cx, cy, radius;
    float nearest_square, farthest_square, deep_square, clear_square;
} iris_frame_t;

static iris_frame_t iris_frame(float cx, float cy, float radius, int x, int width)
{
    float nearest = cx - clampf(cx, x, x + width - 1);
    float farthest = fmaxf(fabsf(cx - x), fabsf(cx - (x + width - 1)));
    return (iris_frame_t){.cx = cx, .cy = cy, .radius = radius,
        .nearest_square = nearest * nearest, .farthest_square = farthest * farthest,
        .deep_square = radius >= 4 ? (radius - 4) * (radius - 4) : -1,
        .clear_square = (radius + 4) * (radius + 4)};
}

static void iris_plan_row(const iris_frame_t *frame, int y, iris_row_t *plan)
{
    float radius = frame->radius, cx = frame->cx, delta_y = y - frame->cy;
    float square_y = delta_y * delta_y;
    *plan = (iris_row_t){.left = 1, .right = 0, .edge_left = -1, .edge_right = -1};
    /* Four pixels of clearance conservatively cover both AA endpoints,
     * including float rounding. Interior/exterior rows need one stream
     * copy, with no square roots or rounded edge preparation. */
    if (frame->farthest_square + square_y < frame->deep_square) plan->source = 2;
    else if (radius >= 0 && frame->nearest_square + square_y > frame->clear_square) plan->source = 1;
    if (plan->source) return;
    float square = radius * radius - square_y;
    if (square > 0) {
        float half = sqrtf(square), left = cx - half, right = cx + half;
        plan->left = (int)ceilf(left); plan->right = (int)floorf(right);
        plan->edge_left = (int)floorf(left); plan->edge_right = (int)ceilf(right);
        plan->alpha_left = (unsigned)lroundf((1 - (left - floorf(left))) * 256);
        plan->alpha_right = (unsigned)lroundf((1 - (ceilf(right) - right)) * 256);
    }
}

static void iris_row(const orb_nav_compositor_t *c, const iris_row_t *plan,
                     int x, int y, int width, uint8_t *dst, bool swapped)
{
    unsigned inside = c->reverse ? c->from : c->to;
    const uint16_t *inner = c->slots[inside].pixels + (size_t)y * c->width;
    const uint16_t *outer = c->slots[c->prepared].pixels + (size_t)y * c->width;
    if (plan->source) {
        copy_span(dst, (plan->source == 2 ? inner : outer) + x, width, swapped);
        return;
    }
    int left = plan->left > x ? plan->left : x;
    int right = plan->right < x + width - 1 ? plan->right : x + width - 1;
    /* Disjoint immutable spans avoid rereading the covered PSRAM plane. */
    if (left <= right) {
        copy_span(dst, outer + x, left - x, swapped);
        copy_span(dst + (size_t)(left - x) * 2, inner + left, right - left + 1, swapped);
        copy_span(dst + (size_t)(right + 1 - x) * 2, outer + right + 1, x + width - right - 1, swapped);
    } else copy_span(dst, outer + x, width, swapped);
    if (plan->edge_left >= x && plan->edge_left < x + width)
        put_pixel(dst + (size_t)(plan->edge_left - x) * 2,
            blend(outer[plan->edge_left], inner[plan->edge_left], plan->alpha_left), swapped);
    if (plan->edge_right >= x && plan->edge_right < x + width)
        put_pixel(dst + (size_t)(plan->edge_right - x) * 2,
            blend(outer[plan->edge_right], inner[plan->edge_right], plan->alpha_right), swapped);
}

bool orb_nav_compositor_render_band(orb_nav_compositor_t *c, int x, int y,
                                   int width, int height, void *pixels,
                                   size_t stride, bool swapped)
{
    if (!c || !pixels || x < 0 || y < 0 || width <= 0 || height <= 0 || x + width > c->width ||
        y + height > c->height || stride < (size_t)width * 2 ||
        !c->slots[c->from].valid || !c->slots[c->to].valid) return false;
    /* DMA ownership must never alias an immutable snapshot, including an
     * interior pointer or the sheet's prepared plane. */
    uintptr_t output_begin = (uintptr_t)pixels;
    uintptr_t output_end = output_begin + (size_t)(height - 1) * stride + (size_t)width * 2;
    size_t snapshot_bytes = (size_t)c->width * c->height * 2;
    for (unsigned slot = 0; slot < ORB_NAV_SNAPSHOT_SLOTS; slot++) {
        uintptr_t source_begin = (uintptr_t)c->slots[slot].pixels;
        if (source_begin && output_begin < source_begin + snapshot_bytes && output_end > source_begin) return false;
    }
    float p = clampf(c->position, 0, 1);
    bool fade = c->reduced_motion || c->kind == ORB_NAV_TRANSITION_FADE;
    int dx = 0, dy = 0, tx = 0, ty = 0;
    int extent = c->direction == ORB_NAV_DIRECTION_LEFT || c->direction == ORB_NAV_DIRECTION_RIGHT ? c->width : c->height;
    int sign = c->direction == ORB_NAV_DIRECTION_LEFT || c->direction == ORB_NAV_DIRECTION_UP ? -1 : 1;
    int displacement = (int)lroundf(c->position * extent * sign);
    int peek = (int)lroundf(.06f * extent * sinf(PI_F * p));
    int neighbour = displacement - sign * (extent + peek);
    if (c->direction == ORB_NAV_DIRECTION_LEFT || c->direction == ORB_NAV_DIRECTION_RIGHT) { dx = displacement; tx = neighbour; }
    else { dy = displacement; ty = neighbour; }
    float cx = c->token.x + c->token.width * .5f, cy = c->token.y + c->token.height * .5f;
    float initial_radius = fmaxf(c->token.width, c->token.height) * .5f;
    float far_x = fmaxf(cx, c->width - 1 - cx), far_y = fmaxf(cy, c->height - 1 - cy);
    float max_radius = hypotf(far_x, far_y) + 1;
    float iris_progress = c->reverse ? 1 - p : p;
    float radius = initial_radius + (max_radius - initial_radius) * iris_progress;
    bool draw_iris = !fade && c->kind == ORB_NAV_TRANSITION_IRIS && c->position != 0 && c->position != 1;
    iris_frame_t iris = {0};
    if (draw_iris) iris = iris_frame(cx, cy, radius, x, width);
    iris_row_t plan;
    glyph_frame_t glyph = {.ready = false};
    if (draw_iris) glyph_prepare(c, p, y, height, &glyph);
    for (int row = 0; row < height; row++) {
        int sy = y + row;
        uint8_t *dst = (uint8_t *)pixels + (size_t)row * stride;
        const uint16_t *a = c->slots[c->from].pixels + (size_t)sy * c->width + x;
        const uint16_t *b = c->slots[c->to].pixels + (size_t)sy * c->width + x;
        if (c->position == 0 || (c->position == 1 && c->kind != ORB_NAV_TRANSITION_BOUNDARY)) {
            copy_span(dst, c->position == 0 ? a : b, width, swapped);
        } else if (fade) {
            /* Polish #33: the outgoing layer fades out first, then the incoming
             * one fades in, so the two never double-expose. Each pixel reads
             * one plane, half of the former two-plane blend. */
            const uint16_t *source = p < .5f ? a : b;
            unsigned alpha = (unsigned)lroundf((p < .5f ? 1 - p * 2 : p * 2 - 1) * 256);
            for (int col = 0; col < width; col++) put_pixel(dst + col * 2, blend(0, source[col], alpha), swapped);
        } else if (c->kind == ORB_NAV_TRANSITION_IRIS) {
            iris_plan_row(&iris, sy, &plan);
            iris_row(c, &plan, x, sy, width, dst, swapped);
        } else if (c->kind == ORB_NAV_TRANSITION_SHEET) {
            /* A sheet travels in from the selected arc while its place stays
             * cached at 92% / 40%. No blur or depth animation is performed. */
            overlay_row(c, c->to, tx, ty, x, sy, width, dst, swapped);
        } else if (c->kind == ORB_NAV_TRANSITION_BACK) {
            overlay_row(c, c->from, dx, dy, x, sy, width, dst, swapped);
        } else if (c->kind == ORB_NAV_TRANSITION_BOUNDARY) {
            memset(dst, 0, (size_t)width * 2);
            translated_row(c, c->from, dx, dy, x, sy, width, dst, swapped);
        } else {
            memset(dst, 0, (size_t)width * 2);
            translated_row(c, c->from, dx, dy, x, sy, width, dst, swapped);
            translated_row(c, c->to, tx, ty, x, sy, width, dst, swapped);
        }
        if (!fade && c->kind == ORB_NAV_TRANSITION_IRIS) glyph_row(c, &glyph, p, x, sy, width, dst, swapped);
    }
    c->stats.bands++; c->stats.pixels += (uint64_t)width * height;
    if (y == 0) c->stats.frames++;
    return true;
}

void orb_nav_compositor_stats(const orb_nav_compositor_t *c, orb_nav_compositor_stats_t *stats)
{
    if (c && stats) *stats = c->stats;
}
