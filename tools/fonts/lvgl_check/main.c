/*
 * Host check for orb_fonts: Orb's LVGL fork running on the Mac with no display.
 *
 *   orb_fonts_check <pack> <records.tsv> <jobs.tsv> <status.ppm> <screen-face.ppm>
 *
 * 1. Checks every pack record against the C font it was made from (records.tsv: font id,
 *    record file): metrics, every glyph's descriptor and bitmap, kerning.
 * 2. Decodes RLE glyphs on two threads at once, as the firmware's two draw units do.
 * 3. Runs the self-test (test_app/main/orb_fonts_selftest.c) with the font pack.
 * 4. Renders each job into a PGM so tools/fonts/lvgl_check.py can compare LVGL's pixels
 *    with the Pillow previews. Job lines are tab-separated: out.pgm, content size, Bold
 *    Text, kind (text, icons, clock), style or clock index, variant flags, UTF-8 text.
 * 5. Checks live status widgets through every font setting and renders their gallery
 *    and the 466x466 face directly through LVGL (RGB PPM, losslessly encoded to PNG).
 *
 * It also provides orb_fonts' platform hooks: the pack is a file and zlib inflates it.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#include "lvgl.h"
#include "orb_fonts.h"
#include "orb_fonts_private.h"
#include "orb_fonts_selftest.h"
#include "status_check.h"

#define W 1400
#define H 480

static uint8_t frame[W * H];   /* the red channel: white text on black is its coverage */
static uint8_t buf[W * H * 4]; /* XRGB8888, one band covering the whole screen */
static FILE *pack_file;
static bool fail_font_allocations;
static unsigned font_allocation_failures;

/* ---- orb_fonts' platform hooks ---- */

bool orb_fonts_port_read(uint32_t offset, void *dst, size_t size)
{
    return pack_file && fseek(pack_file, (long)offset, SEEK_SET) == 0 && fread(dst, 1, size, pack_file) == size;
}

bool orb_fonts_port_inflate(void *dst, size_t dst_size, const void *src, size_t src_size)
{
    uLongf out = dst_size;
    return uncompress(dst, &out, src, src_size) == Z_OK && out == dst_size;
}

void *orb_fonts_port_alloc(size_t size)
{
    if (fail_font_allocations) {
        font_allocation_failures++;
        return NULL;
    }
    return malloc(size);
}

void orb_host_fail_font_allocations(bool fail)
{
    fail_font_allocations = fail;
    if (fail) font_allocation_failures = 0;
}

unsigned orb_host_font_allocation_failures(void)
{
    return font_allocation_failures;
}

void orb_fonts_port_free(void *ptr)
{
    free(ptr);
}

/* ---- helpers ---- */

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static uint32_t tick_ms(void)
{
    return (uint32_t)(now_us() / 1000);
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)n);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return data;
}

/* A glyph's A8 bitmap as LVGL decodes it, packed to box_w per row; returns its size. */
static size_t decode(const lv_font_t *font, uint32_t cp, uint32_t next, lv_font_glyph_dsc_t *g, uint8_t *out,
                     size_t cap)
{
    memset(g, 0, sizeof(*g));
    if (!lv_font_get_glyph_dsc(font, g, cp, next) || g->resolved_font != font) {
        return 0;
    }
    size_t n = (size_t)g->box_w * g->box_h;
    if (!n || n > cap) {
        return 0;
    }
    lv_draw_buf_t *db = lv_draw_buf_create(g->box_w, g->box_h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    lv_font_get_glyph_bitmap(g, db);
    for (int y = 0; y < g->box_h; y++) {
        memcpy(out + (size_t)y * g->box_w, db->data + (size_t)y * db->header.stride, g->box_w);
    }
    lv_draw_buf_destroy(db);
    return n;
}

/* ---- 1. records ---- */

static int compare_font(const char *name, const lv_font_t *c, const lv_font_t *r)
{
    static uint8_t a[256 * 256], b[256 * 256];
    int bad = 0;
    if (c->line_height != r->line_height || c->base_line != r->base_line ||
        c->underline_position != r->underline_position || c->underline_thickness != r->underline_thickness) {
        printf("  %s: metrics differ\n", name);
        bad++;
    }
    /* Every codepoint either font has, then kerning against a sample of followers. */
    static const uint32_t ranges[][2] = {{0x20, 0x2FF}, {0x2000, 0x27FF}, {0xE000, 0xE1FF}};
    static const uint32_t next[] = {0, 'A', 'T', 'V', 'Y', 'a', 'o', 'y', '.', ',', '1', '7', 0x2019};
    int glyphs = 0;
    for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
        for (uint32_t cp = ranges[i][0]; cp <= ranges[i][1]; cp++) {
            for (size_t k = 0; k < sizeof(next) / sizeof(next[0]); k++) {
                lv_font_glyph_dsc_t gc, gr;
                memset(&gc, 0, sizeof(gc));
                memset(&gr, 0, sizeof(gr));
                bool hc = lv_font_get_glyph_dsc(c, &gc, cp, next[k]) && gc.resolved_font == c;
                bool hr = lv_font_get_glyph_dsc(r, &gr, cp, next[k]) && gr.resolved_font == r;
                if (hc != hr || (hc && (gc.adv_w != gr.adv_w || gc.box_w != gr.box_w || gc.box_h != gr.box_h ||
                                        gc.ofs_x != gr.ofs_x || gc.ofs_y != gr.ofs_y))) {
                    if (bad++ < 5) {
                        printf("  %s: U+%04X before U+%04X differs\n", name, (unsigned)cp, (unsigned)next[k]);
                    }
                }
                if (!hc || k) {
                    continue;
                }
                glyphs++;
                size_t na = decode(c, cp, 0, &gc, a, sizeof(a)), nb = decode(r, cp, 0, &gr, b, sizeof(b));
                if (na != nb || memcmp(a, b, na)) {
                    if (bad++ < 5) {
                        printf("  %s: U+%04X bitmap differs\n", name, (unsigned)cp);
                    }
                }
            }
        }
    }
    return glyphs ? bad : bad + 1;
}

static int check_records(const char *list)
{
    FILE *f = fopen(list, "r");
    if (!f) {
        perror(list);
        return 1;
    }
    char line[1024];
    int fonts = 0, bad = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        char *id = strtok(line, "\t"), *name = strtok(NULL, "\t"), *path = strtok(NULL, "\t");
        if (!id || !name || !path) {
            continue;
        }
        size_t size;
        uint8_t *rec = read_file(path, &size);
        const lv_font_t *builtin = orb_fonts_get((orb_font_id_t)atoi(id));
        lv_font_t *font = rec ? orb_fonts_record_font(rec, size, NULL) : NULL;
        if (!font || !builtin) {
            printf("  %s: record doesn't load\n", name);
            bad++;
        } else {
            bad += compare_font(name, builtin, font);
        }
        orb_fonts_record_font_free(font);
        free(rec);
        fonts++;
    }
    fclose(f);
    printf("pack records vs C fonts: %d fonts, %s\n", fonts, bad ? "DIFFERENT" : "identical");
    return bad;
}

/* ---- 2. two draw threads ---- */

typedef struct {
    const lv_font_t *font;
    uint32_t cp;
    uint8_t ref[256 * 256];
    uint8_t out[256 * 256];
    size_t n;
    int bad;
} race_t;

static void *race(void *arg)
{
    race_t *r = arg;
    lv_font_glyph_dsc_t g;
    for (int i = 0; i < 20000; i++) {
        size_t n = decode(r->font, r->cp, 0, &g, r->out, sizeof(r->out));
        r->bad += n != r->n || memcmp(r->out, r->ref, n) != 0;
    }
    return NULL;
}

static int check_concurrent_decoding(void)
{
    static race_t a, b;
    a.font = orb_clock_font(ORB_CLOCK_LARGE);
    a.cp = '8';
    b.font = orb_icon_font(ORB_TEXT_LARGE_TITLE);
    b.cp = 0xE04C; /* ORB_ICON_BATTERY_FULL */
    lv_font_glyph_dsc_t g;
    a.n = decode(a.font, a.cp, 0, &g, a.ref, sizeof(a.ref));
    b.n = decode(b.font, b.cp, 0, &g, b.ref, sizeof(b.ref));
    if (!a.n || !b.n || !((lv_font_fmt_txt_dsc_t *)a.font->dsc)->bitmap_format ||
        !((lv_font_fmt_txt_dsc_t *)b.font->dsc)->bitmap_format) {
        printf("concurrent RLE decoding: test glyphs missing or not compressed\n");
        return 1;
    }
    pthread_t ta, tb;
    pthread_create(&ta, NULL, race, &a);
    pthread_create(&tb, NULL, race, &b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    printf("concurrent RLE decoding on two threads: %d and %d of 20000 bitmaps wrong\n", a.bad, b.bad);
    return a.bad + b.bad;
}

/* ---- 4. renders ---- */

static void grab(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int32_t w = lv_area_get_width(area);
    for (int32_t y = area->y1; y <= area->y2; y++) {
        for (int32_t x = area->x1; x <= area->x2; x++) {
            frame[y * W + x] = px_map[((y - area->y1) * w + (x - area->x1)) * 4 + 2];
        }
    }
    lv_display_flush_ready(disp);
}

static int render_jobs(lv_display_t *disp, const char *path)
{
    FILE *jobs = fopen(path, "r");
    if (!jobs) {
        perror(path);
        return 1;
    }
    lv_display_set_flush_cb(disp, grab);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_text_color(scr, lv_color_white(), 0);
    char line[4096];
    int n = 0;
    while (fgets(line, sizeof(line), jobs)) {
        line[strcspn(line, "\n")] = 0;
        char *out = strtok(line, "\t"), *size = strtok(NULL, "\t"), *bold = strtok(NULL, "\t");
        char *kind = strtok(NULL, "\t"), *index = strtok(NULL, "\t");
        char *variant = strtok(NULL, "\t"), *text = strtok(NULL, "");
        if (!out || !size || !bold || !kind || !index || !variant || !text) {
            continue;
        }
        if (orb_fonts_set_content_size((orb_content_size_t)atoi(size)) != ORB_FONTS_OK ||
            orb_fonts_set_bold_text(atoi(bold)) != ORB_FONTS_OK) {
            printf("can't select content size %s, Bold Text %s\n", size, bold);
            return 1;
        }
        lv_obj_clean(scr);
        lv_obj_t *label = lv_label_create(scr);
        int i = atoi(index), v = atoi(variant);
        if (!strcmp(kind, "clock")) {
            orb_clock_apply(label, (orb_clock_t)i, 0);
        } else if (!strcmp(kind, "icons")) {
            orb_icon_apply(label, (orb_text_style_t)i, (orb_text_variant_t)v, 0);
        } else {
            orb_text_apply(label, (orb_text_style_t)i, (orb_text_variant_t)v, 0);
        }
        lv_label_set_text(label, text);
        lv_obj_set_pos(label, 10, 10);
        lv_obj_invalidate(scr);
        memset(frame, 0, sizeof(frame));
        lv_refr_now(disp);
        FILE *f = fopen(out, "wb");
        if (!f) {
            perror(out);
            return 1;
        }
        fprintf(f, "P5\n%d %d\n255\n", W, H);
        fwrite(frame, 1, sizeof(frame), f);
        fclose(f);
        n++;
    }
    fclose(jobs);
    orb_fonts_set_bold_text(false);
    orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT);
    printf("rendered %d jobs\n", n);
    return 0;
}

int main(int argc, char **argv)
{
    bool status_only = argc > 1 && strcmp(argv[1], "--status-only") == 0;
    bool status_tests = argc > 1 && strcmp(argv[1], "--status-tests") == 0;
    bool rescue_tests = argc > 1 && strcmp(argv[1], "--rescue-tests") == 0;
    if ((status_only && argc != 5) || ((status_tests || rescue_tests) && argc != 3) ||
        (!status_only && !status_tests && !rescue_tests && argc != 6)) {
        fprintf(stderr, "usage: %s pack records.tsv jobs.tsv status.ppm screen-face.ppm\n"
                        "       %s --status-only pack status.ppm screen-face.ppm\n"
                        "       %s --status-tests pack\n"
                        "       %s --rescue-tests missing-or-corrupt-pack\n", argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }
    const char *pack_path = argv[status_only || status_tests || rescue_tests ? 2 : 1];
    pack_file = fopen(pack_path, "rb");
    if (!pack_file && !rescue_tests) {
        perror(pack_path);
        return 2;
    }
    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, grab);

    if (rescue_tests) {
        LV_FONT_DECLARE(orb_inter_28_regular)
        LV_FONT_DECLARE(orb_icons_28_regular)
        int checks = 0, failures = 0;
        for (int style = 0; style < ORB_TEXT_STYLE_COUNT; style++) {
            for (int variant = 0; variant < 4; variant++) {
                const lv_font_t *text = orb_font_variant(style, variant);
                const lv_font_t *icon = orb_icon_font_variant(style, variant);
                lv_font_glyph_dsc_t glyph;
                failures += text != &orb_inter_28_regular;
                /* The rescue icons carry the app-owned ellipsis fallback (orb_fonts_builtin_ellipsis). */
                failures += orb_fonts_builtin_source(icon) != &orb_icons_28_regular;
                failures += !lv_font_get_glyph_dsc(text, &glyph, 'H', 0);
                failures += !lv_font_get_glyph_dsc(icon, &glyph, 0xE010, 0); /* gear */
                checks += 4;
            }
        }
        for (int clock = 0; clock < ORB_CLOCK_COUNT; clock++) {
            failures += orb_clock_font(clock) != &orb_inter_28_regular;
            checks++;
        }
        failures += orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK;
        failures += orb_fonts_content_size() != ORB_CONTENT_SIZE_DEFAULT;
        checks += 2;
        printf("rescue fonts: %d checks, %d failures\n", checks, failures);
        return failures ? 1 : 0;
    }

    if (status_only) {
        return orb_status_host_previews(disp, argv[3], argv[4]) ? 1 : 0;
    }
    if (status_tests) {
        return orb_status_host_check(disp) ? 1 : 0;
    }

    int failures = check_records(argv[2]);
    failures += check_concurrent_decoding();
    failures += orb_fonts_selftest(disp, now_us, true);
    if (render_jobs(disp, argv[3])) {
        return 2;
    }
    failures += orb_status_host_check(disp);
    failures += orb_status_host_previews(disp, argv[4], argv[5]);
    return failures ? 1 : 0;
}
