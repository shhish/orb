/* Real-LVGL integration checks and previews for Orb's live status widgets. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "orb_fonts.h"
#include "orb_status.h"
#include "status_check.h"

#define WIDTH 1400
#define HEIGHT 480

static uint8_t pixels[WIDTH * HEIGHT * 3];
static unsigned checks, failures, flushes;
static uint32_t flushed_pixels;

static void expect(bool condition, const char *message)
{
    checks++;
    if (!condition) {
        failures++;
        printf("  status FAIL: %s\n", message);
    }
}

static void expected_allocation_log(lv_log_level_t level, const char *message)
{
    (void)level;
    if (!strstr(message, "no memory for pack font")) fputs(message, stderr);
}

static void capture(lv_display_t *display, const lv_area_t *area, uint8_t *map)
{
    const int width = lv_area_get_width(area);
    const int stride = lv_display_get_horizontal_resolution(display);
    flushes++;
    flushed_pixels += (uint32_t)width * lv_area_get_height(area);
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            size_t src = ((size_t)(y - area->y1) * width + x - area->x1) * 4;
            size_t dst = ((size_t)y * stride + x) * 3;
            pixels[dst] = map[src + 2];
            pixels[dst + 1] = map[src + 1];
            pixels[dst + 2] = map[src];
        }
    }
    lv_display_flush_ready(display);
}

static lv_obj_t *reset_screen(lv_display_t *display, int width, int height)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_display_set_resolution(display, width, height);
    lv_display_set_flush_cb(display, capture);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    lv_obj_set_scrollable(screen, false);
    memset(pixels, 0, sizeof(pixels));
    return screen;
}

static void refresh(lv_display_t *display)
{
    flushes = 0;
    flushed_pixels = 0;
    lv_refr_now(display);
}

static uint64_t ink(void)
{
    uint64_t result = 0;
    for (size_t i = 0; i < sizeof(pixels); i += 3) {
        uint8_t value = pixels[i] > pixels[i + 1] ? pixels[i] : pixels[i + 1];
        if (pixels[i + 2] > value) value = pixels[i + 2];
        result += value;
    }
    return result;
}

static uint64_t fingerprint(void)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < sizeof(pixels); i++) {
        hash = (hash ^ pixels[i]) * UINT64_C(1099511628211);
    }
    return hash;
}

static unsigned color_pixels(uint32_t color)
{
    unsigned count = 0;
    for (size_t i = 0; i < sizeof(pixels); i += 3) {
        if (pixels[i] == (uint8_t)(color >> 16) && pixels[i + 1] == (uint8_t)(color >> 8) &&
            pixels[i + 2] == (uint8_t)color) count++;
    }
    return count;
}

static int write_ppm(lv_display_t *display, const char *path)
{
    lv_obj_invalidate(lv_screen_active());
    refresh(display);
    FILE *file = fopen(path, "wb");
    if (!file) {
        perror(path);
        return 1;
    }
    int width = lv_display_get_horizontal_resolution(display);
    int height = lv_display_get_vertical_resolution(display);
    fprintf(file, "P6\n%d %d\n255\n", width, height);
    size_t size = (size_t)width * height * 3;
    bool ok = fwrite(pixels, 1, size, file) == size;
    if (fclose(file)) ok = false;
    return ok ? 0 : 1;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, orb_text_style_t style,
                       orb_text_variant_t variant, int x, int y, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    orb_text_apply(obj, style, variant, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_label_set_text(obj, text);
    lv_obj_set_pos(obj, x, y);
    return obj;
}

static lv_obj_t *centered_label(lv_obj_t *parent, const char *text, orb_text_style_t style,
                               orb_text_variant_t variant, int center, int baseline, uint32_t color)
{
    lv_obj_t *obj = label(parent, text, style, variant, 0, 0, color);
    const lv_font_t *font = orb_font_variant(style, variant);
    lv_obj_update_layout(obj);
    lv_obj_set_pos(obj, center - lv_obj_get_width(obj) / 2,
                   baseline - (font->line_height - font->base_line));
    return obj;
}

static void status_sheet(lv_display_t *display)
{
    lv_obj_t *screen = reset_screen(display, WIDTH, HEIGHT);
    static const char *names[] = {"100%", "80%", "50%", "20%", "5%", "Charging", "Low Power"};
    static const int levels[] = {100, 80, 50, 20, 5, 80, 50};
    const orb_text_style_t styles[] = {ORB_TEXT_BODY, ORB_TEXT_HEADLINE};
    for (unsigned s = 0; s < 2; s++) {
        int top = (int)s * 240;
        label(screen, s ? "Headline" : "Body", styles[s], ORB_TEXT_REGULAR, 24, top + 5, 0xFFFFFF);
        label(screen, "Level", ORB_TEXT_FOOTNOTE2, 0, 24, top + 88, 0x98989D);
        label(screen, "Percentage", ORB_TEXT_FOOTNOTE2, 0, 24, top + 131, 0x98989D);
        label(screen, "Wi-Fi", ORB_TEXT_FOOTNOTE2, 0, 24, top + 194, 0x98989D);
        for (unsigned i = 0; i < 7; i++) {
            int center = 220 + (int)i * 172;
            centered_label(screen, names[i], ORB_TEXT_FOOTNOTE2, 0, center, top + 70, 0x98989D);
            for (unsigned percentage = 0; percentage < 2; percentage++) {
                lv_obj_t *battery = orb_battery_create(screen, styles[s], ORB_TEXT_REGULAR);
                orb_battery_set_state(battery, levels[i], i == 5, i == 6);
                orb_battery_set_percentage(battery, percentage != 0);
                lv_obj_update_layout(battery);
                lv_obj_set_pos(battery, center - lv_obj_get_width(battery) / 2,
                               top + 108 + percentage * 43 - orb_status_baseline(battery));
            }
        }
        static const char *wifi_names[] = {"0 bars", "1 bar", "2 bars", "3 bars", "No connection"};
        for (unsigned i = 0; i < 5; i++) {
            int center = 220 + (int)i * 222;
            lv_obj_t *wifi = orb_wifi_create(screen, styles[s], ORB_TEXT_REGULAR);
            orb_wifi_set_bars(wifi, i < 4 ? (int)i : 0, i < 4);
            lv_obj_update_layout(wifi);
            lv_obj_set_pos(wifi, center - 64, top + 215 - orb_status_baseline(wifi));
            label(screen, wifi_names[i], ORB_TEXT_FOOTNOTE2, 0, center - 14, top + 194, 0x98989D);
        }
    }
}

static void face_screen(lv_display_t *display)
{
    lv_obj_t *screen = reset_screen(display, 466, 466);
    lv_obj_t *face = lv_obj_create(screen);
    lv_obj_remove_style_all(face);
    lv_obj_set_pos(face, 0, 0);
    lv_obj_set_size(face, 466, 466);
    lv_obj_set_style_bg_color(face, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(face, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(face, true, 0);
    lv_obj_set_scrollable(face, false);
    centered_label(face, "MON 26", ORB_TEXT_FOOTNOTE1, ORB_TEXT_EMPHASIZED, 233, 110, 0xFF9230);
    lv_obj_t *clock = lv_label_create(face);
    orb_clock_apply(clock, ORB_CLOCK_LARGE, 0);
    lv_obj_set_style_text_color(clock, lv_color_white(), 0);
    lv_label_set_text(clock, "10:09");
    lv_obj_update_layout(clock);
    lv_obj_set_pos(clock, 233 - lv_obj_get_width(clock) / 2, 128);
    centered_label(face, ORB_ICON_TERMINAL " 2 need you", ORB_TEXT_TITLE3, 0, 233, 330, 0xFFFFFF);

    lv_obj_t *battery = orb_battery_create(face, ORB_TEXT_CAPTION1, ORB_TEXT_REGULAR);
    orb_battery_set_state(battery, 82, false, false);
    lv_obj_set_style_text_color(battery, lv_color_hex(0x98989D), 0);
    lv_obj_t *percentage = label(face, "82%", ORB_TEXT_CAPTION1, ORB_TEXT_TABULAR, 0, 0, 0x98989D);
    lv_obj_t *wifi = orb_wifi_create(face, ORB_TEXT_CAPTION1, ORB_TEXT_REGULAR);
    orb_wifi_set_rssi(wifi, -48, true);
    lv_obj_set_style_text_color(wifi, lv_color_hex(0x98989D), 0);
    lv_obj_update_layout(face);
    int width = lv_obj_get_width(battery) + lv_obj_get_width(percentage) + lv_obj_get_width(wifi) + 24;
    int x = 233 - width / 2;
    lv_obj_set_pos(battery, x, 385 - orb_status_baseline(battery));
    x += lv_obj_get_width(battery) + 12;
    const lv_font_t *font = orb_font_variant(ORB_TEXT_CAPTION1, ORB_TEXT_TABULAR);
    lv_obj_set_pos(percentage, x, 385 - (font->line_height - font->base_line));
    x += lv_obj_get_width(percentage) + 12;
    lv_obj_set_pos(wifi, x, 385 - orb_status_baseline(wifi));
}

int orb_status_host_previews(lv_display_t *display, const char *sheet_path, const char *face_path)
{
    if (orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) != ORB_FONTS_OK ||
        orb_fonts_set_bold_text(false) != ORB_FONTS_OK) return 1;
    status_sheet(display);
    int result = write_ppm(display, sheet_path);
    face_screen(display);
    result += write_ppm(display, face_path);
    lv_obj_clean(lv_screen_active());
    printf("rendered real-LVGL live status sheet and 466x466 face\n");
    return result;
}

/* Match raw status drawing against the same ink opacity supplied through
 * text_opa. This exercises all primitives, including percentage/bolt knockouts
 * and the disconnected slash, without relying on a particular rasterizer's
 * antialiasing or RGB rounding. */
static void check_status_opacity(lv_display_t *display)
{
    lv_obj_t *screen = reset_screen(display, WIDTH, HEIGHT);
    lv_obj_t *parent = lv_obj_create(screen);
    lv_obj_remove_style_all(parent);
    lv_obj_set_size(parent, 300, 120);
    lv_obj_set_scrollable(parent, false);
    lv_obj_t *battery = orb_battery_create(parent, ORB_TEXT_BODY, 0);
    lv_obj_t *wifi = orb_wifi_create(parent, ORB_TEXT_BODY, 0);
    lv_obj_set_pos(battery, 40, 40);
    lv_obj_set_pos(wifi, 160, 40);
    static const struct { lv_opa_t text, widget, parent; } cases[] = {
        {128, LV_OPA_COVER, LV_OPA_COVER},
        {LV_OPA_COVER, 128, LV_OPA_COVER},
        {LV_OPA_COVER, LV_OPA_COVER, 128},
        {128, 128, 128},
    };
    for (unsigned state = 0; state < 3; state++) {
        orb_battery_set_state(battery, state == 2 ? 20 : 100, state == 1, false);
        orb_battery_set_percentage(battery, state != 0);
        orb_wifi_set_bars(wifi, state == 0 ? 3 : 1, state != 2);
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            lv_opa_t layer = LV_OPA_COVER;
            if (cases[i].parent < LV_OPA_MAX) layer = LV_OPA_MIX2(layer, cases[i].parent);
            if (cases[i].widget < LV_OPA_MAX) layer = LV_OPA_MIX2(layer, cases[i].widget);
            lv_opa_t ink_opa = cases[i].text;
            if (layer < LV_OPA_MAX) ink_opa = LV_OPA_MIX2(ink_opa, layer);
            lv_obj_set_style_opa(parent, LV_OPA_COVER, 0);
            lv_obj_set_style_opa(battery, LV_OPA_COVER, 0);
            lv_obj_set_style_opa(wifi, LV_OPA_COVER, 0);
            lv_obj_set_style_text_opa(parent, ink_opa, 0);
            lv_obj_invalidate(screen);
            refresh(display);
            uint64_t reference = fingerprint();
            lv_obj_set_style_text_opa(parent, cases[i].text, 0);
            lv_obj_set_style_opa(parent, cases[i].parent, 0);
            lv_obj_set_style_opa(battery, cases[i].widget, 0);
            lv_obj_set_style_opa(wifi, cases[i].widget, 0);
            lv_obj_invalidate(screen);
            refresh(display);
            expect(fingerprint() == reference,
                   "status ink honors text, widget and ancestor opacity exactly once");
        }
    }
    lv_obj_delete(parent);
}

int orb_status_host_check(lv_display_t *display)
{
    checks = failures = 0;
    static const int rssi[] = {-200, -81, -80, -68, -67, -56, -55, 0};
    static const int bars[] = {0, 0, 1, 1, 2, 2, 3, 3};
    for (unsigned i = 0; i < sizeof(rssi) / sizeof(rssi[0]); i++) {
        expect(orb_wifi_bars_for_rssi(rssi[i]) == bars[i], "RSSI boundary mapping");
    }
    expect(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK, "default size");
    expect(orb_fonts_set_bold_text(false) == ORB_FONTS_OK, "regular weight");
    lv_obj_t *screen = reset_screen(display, WIDTH, HEIGHT);
    lv_obj_t *battery = orb_battery_create(screen, ORB_TEXT_BODY, 0);
    lv_obj_set_pos(battery, 40, 40);
    lv_obj_update_layout(battery);
    const int width = lv_obj_get_width(battery), height = lv_obj_get_height(battery);
    expect(width > height && height > 10, "battery proportions");
    expect(orb_status_baseline(battery) > 0 && orb_status_baseline(battery) <= height,
           "baseline is inside line box");
    orb_battery_set_state(battery, -10, false, false);
    expect(orb_battery_get_state(battery).level == 0, "negative level clamps to zero");
    orb_battery_set_state(battery, 150, false, false);
    expect(orb_battery_get_state(battery).level == 100, "level clamps to 100");
    uint64_t previous = 0;
    static const int levels[] = {0, 5, 20, 50, 80, 100};
    for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        orb_battery_set_state(battery, levels[i], false, false);
        refresh(display);
        uint64_t current = ink();
        expect(current > previous, "visible battery fill increases with level");
        previous = current;
        expect(lv_obj_get_width(battery) == width && lv_obj_get_height(battery) == height,
               "battery level keeps layout stable");
    }
    refresh(display);
    orb_battery_set_state(battery, 100, false, false);
    refresh(display);
    expect(flushes == 0, "same battery state does not redraw");
    orb_battery_set_state(battery, 80, false, false);
    refresh(display);
    expect(flushes > 0 && flushed_pixels <= (uint32_t)(width + 8) * (height + 8),
           "battery change invalidates only its small bounds");
    static const struct { int level; bool charging, low; uint32_t color; } states[] = {
        {80, false, false, 0xFFFFFF}, {20, false, false, 0xFF4245},
        {5, false, false, 0xFF4245}, {80, true, false, 0x30D158},
        {5, true, false, 0x30D158}, {80, false, true, 0xFFD600},
        {5, true, true, 0xFFD600},
    };
    for (unsigned i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        orb_battery_set_state(battery, states[i].level, states[i].charging, states[i].low);
        refresh(display);
        expect(color_pixels(states[i].color) > 0, "battery semantic color and precedence");
    }
    lv_obj_set_style_text_color(battery, lv_color_hex(0x30D158), 0);
    orb_battery_set_state(battery, 80, false, false);
    refresh(display);
    unsigned green_fill = color_pixels(0x30D158);
    orb_battery_set_state(battery, 80, true, false);
    refresh(display);
    expect(color_pixels(0x30D158) < green_fill, "charging bolt knocks out the active fill");
    lv_obj_set_style_text_color(battery, lv_color_white(), 0);
    lv_obj_set_style_text_opa(battery, 128, 0);
    orb_battery_set_state(battery, 100, false, false);
    refresh(display);
    unsigned empty_pixels = color_pixels(0x000000);
    orb_battery_set_percentage(battery, true);
    refresh(display);
    expect(color_pixels(0x000000) > empty_pixels + 5,
           "percentage knockout stays background-colored at inherited half opacity");
    orb_battery_set_percentage(battery, true);
    refresh(display);
    expect(flushes == 0, "same percentage setting does not redraw");
    orb_battery_set_percentage(battery, false);
    lv_obj_set_style_text_opa(battery, LV_OPA_COVER, 0);
    orb_battery_set_state(battery, 80, false, false);
    refresh(display);
    uint64_t no_percentage = fingerprint();
    orb_battery_set_percentage(battery, true);
    refresh(display);
    expect(orb_battery_get_state(battery).percentage, "percentage state retained");
    expect(fingerprint() != no_percentage, "percentage draws inside body");
    expect(lv_obj_get_width(battery) == width, "percentage keeps layout stable");
    const int baseline = orb_status_baseline(battery);
    expect(orb_fonts_set_content_size(ORB_CONTENT_SIZE_LARGER) == ORB_FONTS_OK, "Larger font set");
    lv_obj_update_layout(battery);
    expect(lv_obj_get_width(battery) > width && orb_status_baseline(battery) > baseline,
           "live widget follows Dynamic Type");
    expect(orb_fonts_set_bold_text(true) == ORB_FONTS_OK, "Bold Text font set");
    refresh(display);
    expect(ink() > 0, "widget renders after its old pack fonts are freed");
    lv_obj_delete(battery);
    expect(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK,
           "font observer removed when widget deleted");
    expect(orb_fonts_set_bold_text(false) == ORB_FONTS_OK, "restore weight");

    screen = reset_screen(display, WIDTH, HEIGHT);
    lv_obj_t *wifi = orb_wifi_create(screen, ORB_TEXT_HEADLINE, 0);
    lv_obj_set_pos(wifi, 40, 40);
    lv_obj_update_layout(wifi);
    const int wifi_width = lv_obj_get_width(wifi);
    orb_wifi_set_bars(wifi, -2, true);
    expect(orb_wifi_get_state(wifi).bars == 0, "negative bars clamp to zero");
    orb_wifi_set_bars(wifi, 10, true);
    expect(orb_wifi_get_state(wifi).bars == 3, "bars clamp to three");
    previous = 0;
    uint64_t connected_zero = 0;
    for (int i = 0; i < 4; i++) {
        orb_wifi_set_bars(wifi, i, true);
        refresh(display);
        uint64_t current = ink();
        expect(current > previous, "active Wi-Fi bars increase visible ink");
        previous = current;
        expect(lv_obj_get_width(wifi) == wifi_width, "Wi-Fi bars keep layout stable");
        if (i == 0) {
            connected_zero = fingerprint();
            unsigned dim_pixels = 0;
            /* LVGL's mask, primitive and object opacity multiplication can
             * round 77/255 down by two levels before the final RGB blend. */
            for (uint32_t gray = 75; gray <= 77; gray++) {
                dim_pixels += color_pixels(gray * 0x010101);
            }
            expect(dim_pixels > 0,
                   "inactive Wi-Fi bars are thirty percent opaque");
        }
    }
    unsigned softened_edges = 0;
    for (size_t i = 0; i < sizeof(pixels); i += 3) {
        if (pixels[i] > 0 && pixels[i] < 255 && pixels[i] != 77) softened_edges++;
    }
    expect(softened_edges > 0, "Wi-Fi vector edges are anti-aliased");
    orb_wifi_set_bars(wifi, 0, false);
    refresh(display);
    expect(!orb_wifi_get_state(wifi).connected, "disconnected state retained");
    expect(fingerprint() != connected_zero, "disconnected distinct from weak signal");
    orb_wifi_set_rssi(wifi, -60, true);
    refresh(display);
    expect(orb_wifi_get_state(wifi).bars == 2 && orb_wifi_get_state(wifi).connected,
           "RSSI setter updates live state");
    orb_wifi_set_rssi(wifi, -62, true);
    refresh(display);
    expect(flushes == 0, "RSSI changes within one bar do not redraw");
    lv_obj_delete(wifi);

    /* Keep objects alive across every pack swap: stale font pointers, missed
     * observers and size-specific percentage clipping are easy to hide at Body. */
    battery = orb_battery_create(screen, ORB_TEXT_BODY, 0);
    wifi = orb_wifi_create(screen, ORB_TEXT_BODY, 0);
    lv_obj_set_pos(battery, 40, 40);
    lv_obj_set_pos(wifi, 220, 40);
    orb_battery_set_state(battery, 100, true, false);
    orb_battery_set_percentage(battery, true);
    orb_wifi_set_bars(wifi, 3, true);
    for (int size = 0; size < ORB_CONTENT_SIZE_COUNT; size++) {
        expect(orb_fonts_set_content_size((orb_content_size_t)size) == ORB_FONTS_OK,
               "select every Dynamic Type set with live widgets");
        for (int bold = 0; bold < 2; bold++) {
            expect(orb_fonts_set_bold_text(bold != 0) == ORB_FONTS_OK,
                   "select every Bold Text set with live widgets");
            for (int style = 0; style < ORB_TEXT_STYLE_COUNT; style++) {
                for (unsigned variant = 0; variant <= ORB_TEXT_EMPHASIZED; variant++) {
                    orb_status_set_text_style(battery, (orb_text_style_t)style, variant);
                    orb_status_set_text_style(wifi, (orb_text_style_t)style, variant);
                    lv_obj_update_layout(screen);
                    const lv_font_t *text = orb_font_variant((orb_text_style_t)style, variant);
                    const lv_font_t *compact = orb_status_font((orb_text_style_t)style, variant);
                    const orb_text_style_info_t *info = orb_text_style_info((orb_text_style_t)style);
                    lv_font_glyph_dsc_t cap, zero, bolt;
                    expect(lv_font_get_glyph_dsc(text, &cap, 'H', 0), "capital metric exists");
                    expect(orb_status_baseline(battery) == text->line_height - text->base_line &&
                           orb_status_baseline(wifi) == text->line_height - text->base_line,
                           "all live styles align to their text baseline");
                    expect(lv_font_get_glyph_dsc(compact, &zero, '0', 0), "compact zero exists");
                    expect(lv_font_get_glyph_dsc(compact, &bolt, 0xE153, 0), "compact bolt exists");
                    for (uint32_t digit = '1'; digit <= '9'; digit++) {
                        lv_font_glyph_dsc_t d;
                        expect(lv_font_get_glyph_dsc(compact, &d, digit, 0) && d.adv_w == zero.adv_w,
                               "percentage figures are tabular in every style and weight");
                    }
                    int weight = variant ? info->emphasized_weight : info->weight;
                    int body_h = ((int)cap.box_h * 9 + 5) / 10;
                    int body_w = (body_h * 22 + 5) / 10;
                    int stroke = LV_MAX(1, ((int)cap.box_h * (weight >= 600 ? 9 : 6) + 50) / 100);
                    int gap = LV_MAX(1, ((int)cap.box_h * 7 + 50) / 100);
                    int inner_h = body_h - 2 * (stroke + gap);
                    int inner_w = body_w - 2 * (stroke + gap);
                    lv_point_t text_size;
                    lv_text_get_size(&text_size, "100" ORB_ICON_LIGHTNING_FILL, compact, 0, 0,
                                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                    text_size.x++; /* The widget separates the digits and bolt by one pixel. */
                    if (text_size.x > inner_w || (int)zero.box_h > inner_h || (int)bolt.box_h > inner_h) {
                        printf("  fit: size=%d bold=%d style=%d variant=%u; content=%dx%d/%d inner=%dx%d\n",
                               size, bold, style, variant, (int)text_size.x, (int)zero.box_h,
                               (int)bolt.box_h, inner_w, inner_h);
                    }
                    expect(text_size.x <= inner_w, "100 plus bolt fits inside battery fill");
                    expect((int)zero.box_h <= inner_h && (int)bolt.box_h <= inner_h,
                           "percentage and bolt height fit inside battery fill");
                    int content_baseline = (body_h + (int)zero.box_h) / 2 + zero.ofs_y;
                    int ink_top = LV_MIN(content_baseline - zero.ofs_y - (int)zero.box_h,
                                         content_baseline - bolt.ofs_y - (int)bolt.box_h);
                    int ink_bottom = LV_MAX(content_baseline - zero.ofs_y - 1,
                                            content_baseline - bolt.ofs_y - 1);
                    expect(ink_top >= stroke + gap && ink_bottom < body_h - stroke - gap,
                           "digits and bolt stay within the fill at their shared baseline");
                    refresh(display);
                    expect(ink() > 0, "every style renders after Dynamic Type changes");
                }
            }
        }
    }
    lv_obj_delete(battery);
    lv_obj_delete(wifi);
    expect(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK,
           "restore size after deleting observers");
    expect(orb_fonts_set_bold_text(false) == ORB_FONTS_OK, "restore normal weight");

    /* A cold AX3 font can fail to load, leaving Default-size geometry. Drawing
     * must use that measured cap height, and must not retry a flash allocation.
     * Even if a different label later loads AX3 successfully, this widget keeps
     * its already-measured geometry until the next font-setting notification. */
    screen = reset_screen(display, WIDTH, HEIGHT);
    const lv_font_t *default_body = orb_font(ORB_TEXT_BODY);
    lv_log_register_print_cb(expected_allocation_log);
    orb_host_fail_font_allocations(true);
    expect(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK,
           "AX3 setting remains available when an individual font allocation fails");
    battery = orb_battery_create(screen, ORB_TEXT_BODY, 0);
    lv_obj_set_pos(battery, 40, 40);
    orb_battery_set_state(battery, 100, true, false);
    orb_battery_set_percentage(battery, true);
    lv_obj_update_layout(battery);
    expect(orb_host_font_allocation_failures() > 0, "cold font allocation failure was injected");
    expect(orb_status_baseline(battery) == default_body->line_height - default_body->base_line,
           "failed pack font uses Default baseline geometry");
    unsigned allocation_failures = orb_host_font_allocation_failures();
    refresh(display);
    expect(orb_host_font_allocation_failures() == allocation_failures,
           "drawing a status widget never retries a pack allocation");
    uint64_t fallback_render = fingerprint();
    int fallback_width = lv_obj_get_width(battery);
    orb_host_fail_font_allocations(false);
    lv_log_register_print_cb(NULL);
    expect(orb_font(ORB_TEXT_BODY) != default_body, "pack font loads after memory recovery");
    lv_obj_invalidate(battery);
    refresh(display);
    expect(fingerprint() == fallback_render && lv_obj_get_width(battery) == fallback_width,
           "background font recovery cannot enlarge digits inside cached fallback geometry");
    expect(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK,
           "restore Default after injected allocation failure");
    expect(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK,
           "remeasure recovered AX3 after setting notification");
    lv_obj_update_layout(battery);
    expect(lv_obj_get_width(battery) > fallback_width, "recovered widget grows with AX3 notification");
    lv_obj_delete(battery);
    expect(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK,
           "restore size after allocation failure test");
    check_status_opacity(display);
    printf("orb_status: %u checks, %u failures\n", checks, failures);
    return (int)failures;
}
