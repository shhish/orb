/* Fused-material invariants, independent of the compatibility blur oracle:
 * full halo input, immutable maps across bands, native/swapped equivalence,
 * clipped writes/guards, admission failure and bounded internal ownership. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "orb_ui.h"
#include "../../firmware/components/orb_ui/orb_glass_fused.h"
#include "../../firmware/components/orb_ui/orb_glass_sdf.h"
#include "src/draw/lv_draw_private.h"
#include "sim_display.h"

enum {W = 72, H = 72, PAD = 8, STRIDE = W * 2 + 6, GUARD = 32};
static unsigned s_checks;
static uint8_t s_full[GUARD + H * STRIDE + GUARD], s_split[sizeof(s_full)], s_native[sizeof(s_full)];
static uint8_t s_dark[(W + PAD * 2) * (H + PAD * 2)], s_rim[W * H];
static orb_rim_px_t s_rim_px[6 * (W + H)];
static lv_area_t s_shape = {64, 64, 64 + W - 1, 64 + H - 1};
#define CHECK(c) do { s_checks++; if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    return obj;
}

static void unknown_paint(lv_event_t *e) { LV_UNUSED(e); }

static uint32_t hash(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

static void initialize(uint8_t *pixels, bool swapped)
{
    memset(pixels, 0xA7, sizeof(s_full));
    for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++) {
        uint16_t value = 0x3186;
        if (swapped) value = lv_color_swap_16(value);
        memcpy(pixels + GUARD + y * STRIDE + x * 2, &value, 2);
    }
}

static void draw(orb_glass_fused_t *map, uint8_t *pixels, bool swapped, lv_area_t clip)
{
    lv_draw_buf_t buffer, dark, rim;
    lv_color_format_t format = swapped ? LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565;
    lv_area_t band = {s_shape.x1, clip.y1, s_shape.x2, clip.y2};
    int32_t rows = lv_area_get_height(&band);
    CHECK(lv_draw_buf_init(&buffer, W, rows, format, STRIDE,
        pixels + GUARD + (clip.y1 - s_shape.y1) * STRIDE, rows * STRIDE) == LV_RESULT_OK);
    CHECK(lv_draw_buf_init(&dark, W + PAD * 2, H + PAD * 2, LV_COLOR_FORMAT_A8, W + PAD * 2,
                            s_dark, sizeof(s_dark)) == LV_RESULT_OK);
    CHECK(lv_draw_buf_init(&rim, W, H, LV_COLOR_FORMAT_A8, W, s_rim, sizeof(s_rim)) == LV_RESULT_OK);
    lv_layer_t layer = {.draw_buf = &buffer, .color_format = format, .buf_area = band};
    lv_draw_task_t task = {.target_layer = &layer, .clip_area = clip};
    lv_draw_blur_dsc_t dsc;
    lv_draw_blur_dsc_init(&dsc);
    dsc.blur_radius = 26;
    dsc.corner_radius = W / 2;
    dsc.tint = lv_color_hex(0x0091FF);
    dsc.tint_opa = 178;
    CHECK(orb_glass_fused_draw(map, &task, &dsc, &s_shape, &dark, &rim, PAD, 255));
}

static void guards(const uint8_t *pixels)
{
    for (unsigned i = 0; i < GUARD; i++) {
        CHECK(pixels[i] == 0xA7);
        CHECK(pixels[sizeof(s_full) - 1 - i] == 0xA7);
    }
    for (unsigned y = 0; y < H; y++) for (unsigned x = W * 2; x < STRIDE; x++)
        CHECK(pixels[GUARD + y * STRIDE + x] == 0xA7);
}

int main(void)
{
    lv_init();
    sim_display_create();
    orb_ui_init(NULL);
    lv_obj_t *root = lv_screen_active();
    lv_obj_remove_style_all(root);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    /* High contrast source lies entirely outside the control, inside its halo.
     * A band-local/shape-only sampler cannot see it. */
    lv_obj_t *stripe = box(root, 64, 49, W, 9, lv_color_white());
    lv_obj_set_raster_matte(stripe, lv_color_black());
    lv_obj_t *stop = box(root, 64, 64, W, H, lv_color_black());
    lv_obj_update_layout(root);
    lv_obj_raster_prepare(root);
    orb_glass_fused_t map = {0};
    CHECK(orb_glass_fused_prepare(&map, stop, &s_shape, 26));
    CHECK(map.sample.x1 == s_shape.x1 - 16 && map.sample.y1 == s_shape.y1 - 16);
    uint32_t halo_hash = hash(map.pixels, map.width * map.height * 3);
    unsigned lit = 0;
    for (unsigned i = 0; i < map.width * map.height * 3; i++) lit += map.pixels[i];
    CHECK(lit > 0);
    orb_capsule_t capsule = {PAD, PAD, W, H};
    int n = orb_sdf_masks(s_dark, W + PAD * 2, W + PAD * 2, H + PAD * 2,
        s_rim_px, 6 * (W + H), capsule, capsule, 2, 3, 46, 46);
    CHECK(n > 0);
    orb_sdf_rim_light(s_rim, W, s_rim_px, n, 0, -1, .6f);
    for (unsigned swapped = 0; swapped < 2; swapped++) {
        initialize(s_full, swapped);
        initialize(s_split, swapped);
        draw(&map, s_full, swapped, s_shape);
        lv_area_t top = s_shape, bottom = s_shape;
        top.y2 = bottom.y1 + 32;
        bottom.y1 = top.y2 + 1;
        draw(&map, s_split, swapped, top);
        draw(&map, s_split, swapped, bottom);
        CHECK(!memcmp(s_full, s_split, sizeof(s_full)));
        CHECK(hash(map.pixels, map.width * map.height * 3) == halo_hash);
        guards(s_full); guards(s_split);
        if (!swapped) memcpy(s_native, s_full, sizeof(s_full));
        else for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++) {
            size_t i = GUARD + y * STRIDE + x * 2;
            CHECK(s_native[i] == s_full[i + 1] && s_native[i + 1] == s_full[i]);
        }
        initialize(s_split, swapped);
        lv_area_t partial = {s_shape.x1 + 3, s_shape.y1 + 4, s_shape.x2 - 5, s_shape.y2 - 6};
        draw(&map, s_split, swapped, partial);
        for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++) {
            if (x >= 3 && x < W - 5 && y >= 4 && y < H - 6) continue;
            uint16_t value;
            memcpy(&value, s_split + GUARD + y * STRIDE + x * 2, 2);
            CHECK((swapped ? lv_color_swap_16(value) : value) == 0x3186);
        }
        guards(s_split);
    }
    /* The box's corner is outside capsule coverage, but its soft shadow still
     * shades real content there. Fusing only nonzero AA pixels loses it. */
    CHECK(s_dark[(8 + PAD) * (W + PAD * 2) + 8 + PAD] > 0);
    uint16_t corner;
    memcpy(&corner, s_native + GUARD + 8 * STRIDE + 8 * 2, 2);
    CHECK(corner != 0x3186);
    uint8_t unsupported[W * H * 4];
    lv_draw_buf_t argb;
    CHECK(lv_draw_buf_init(&argb, W, H, LV_COLOR_FORMAT_ARGB8888, W * 4, unsupported, sizeof(unsupported)) == LV_RESULT_OK);
    lv_layer_t argb_layer = {.draw_buf = &argb, .color_format = LV_COLOR_FORMAT_ARGB8888, .buf_area = s_shape};
    lv_draw_task_t argb_task = {.target_layer = &argb_layer, .clip_area = s_shape};
    lv_draw_blur_dsc_t unsupported_dsc;
    lv_draw_blur_dsc_init(&unsupported_dsc);
    CHECK(!orb_glass_fused_draw(&map, &argb_task, &unsupported_dsc, &s_shape, NULL, NULL, PAD, 255));
    /* Moving halo source changes the next prepared map, never a prior band. */
    lv_obj_set_y(stripe, 40);
    lv_obj_update_layout(root);
    lv_obj_raster_prepare(root);
    CHECK(orb_glass_fused_prepare(&map, stop, &s_shape, 26));
    CHECK(hash(map.pixels, map.width * map.height * 3) != halo_hash);
    lv_obj_add_event_cb(stripe, unknown_paint, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_set_raster(stripe, false);
    CHECK(!orb_glass_fused_prepare(&map, stop, &s_shape, 26));
    lv_obj_remove_event_cb(stripe, unknown_paint);
    lv_obj_set_style_clip_corner(root, true, 0);
    CHECK(!orb_glass_fused_prepare(&map, stop, &s_shape, 26));
    lv_obj_set_style_clip_corner(root, false, 0);
    /* Dirty/unprepared source is a failed proof, not transparent content. */
    lv_obj_set_raster_matte(stripe, lv_color_black());
    CHECK(!orb_glass_fused_prepare(&map, stop, &s_shape, 26));
    orb_glass_fused_stats_t stats;
    orb_glass_fused_get_stats(&stats);
    CHECK(stats.maps == 1 && stats.bytes == map.capacity && stats.bytes <= stats.budget_bytes);
    CHECK(stats.fallbacks >= 3 && stats.draws >= 8 && stats.prepares == 2);
    lv_obj_raster_prepare(root);
    lv_obj_t *overlay = box(root, 64, 40, W, 20, lv_color_black());
    lv_obj_move_to_index(overlay, 1);
    lv_obj_update_layout(root);
    CHECK(!orb_glass_fused_prepare(&map, stop, &s_shape, 26));
    lv_obj_delete(overlay);
    orb_glass_fused_t leases[12] = {0};
    unsigned admitted = 0;
    while (admitted < 12 && orb_glass_fused_prepare(&leases[admitted], stop, &s_shape, 26)) admitted++;
    CHECK(admitted > 0 && admitted < 12);
    orb_glass_fused_get_stats(&stats);
    CHECK(stats.bytes <= stats.budget_bytes && stats.maps == admitted + 1);
    for (unsigned i = 0; i < 12; i++) orb_glass_fused_release(&leases[i]);
    orb_glass_fused_release(&map);
    orb_glass_fused_get_stats(&stats);
    CHECK(stats.maps == 0 && stats.bytes == 0);
    printf("fused glass: %u checks\n", s_checks);
    lv_deinit();
    return 0;
}
