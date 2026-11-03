/* Inject only the storage status into the real workspace. The host sound
 * backend saves synchronously; no production fault switch is needed. */
#include "orb_home.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include "orb_fonts.h"
#include "orb_settings_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim_display.h"
#include "src/misc/lv_text_private.h"

extern void orb_sound_preferences(orb_sound_preferences_t *preferences);
static bool s_failed, s_saving;
void orb_settings_sound_preferences(orb_sound_preferences_t *preferences)
{
    orb_sound_preferences(preferences);
    preferences->save_error = s_failed;
    preferences->saving = s_saving;
}
static uint32_t s_now;
static orb_shell_t *s_shell;
static lv_display_t *s_display;
static uint32_t tick(void) { return s_now; }
static void advance(void)
{
    for (unsigned i = 0; i < 60; i++) {
        s_now += 20; orb_shell_poll(s_shell); lv_timer_handler();
    }
    lv_refr_now(s_display);
}
static lv_obj_t *find(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}
#define has(root, text) (find(root, text) != NULL)
#define CHECK(v) do { if (!(v)) { fprintf(stderr, "sound save:%d: %s\n", __LINE__, #v); exit(1); } } while (0)
/* Keep the reported copy as a regression even if Sound's normal subtitle is
 * shortened. A semantic label check alone missed the blank first line: the
 * immutable source still contains both lines when a chord cut drops its ink. */
static void wrapped_subtitle(void)
{
    lv_obj_t *title = find(orb_shell_root(s_shell), "Silent"); CHECK(title);
    lv_obj_t *row = lv_obj_get_parent(title), *list = lv_obj_get_parent(row);
    orb_kit_row_set_text(row, ORB_KIT_ROW_TEXT_SUBTITLE, "Alarms still ring");
    lv_obj_update_layout(list);
    orb_round_list_update(list);
    advance();
    lv_obj_t *subtitle = find(row, "Alarms still ring"); CHECK(subtitle);
    CHECK(lv_label_get_max_lines(subtitle) == 2);
    static uint8_t partial[466 * 466 * 3], full[sizeof(partial)];
    const int positions[] = {0, 100, 0};
    for (unsigned position = 0; position < 3; position++) {
        lv_obj_scroll_to_y(list, positions[position], LV_ANIM_OFF);
        advance();
        lv_area_t bounds; CHECK(orb_round_list_child_bounds(row, subtitle, &bounds));
        sim_display_rgb(partial);
        const lv_font_t *font = lv_obj_get_style_text_font(subtitle, 0);
        int pitch = font->line_height + lv_obj_get_style_text_line_space(subtitle, 0);
        lv_font_glyph_dsc_t glyph; CHECK(lv_font_get_glyph_dsc(font, &glyph, 'A', 'l'));
        int top = font->line_height - font->base_line - glyph.box_h - glyph.ofs_y;
        unsigned ink[2] = {0};
        for (unsigned line = 0; line < 2; line++) {
            /* Count the actual cap-height band, excluding adjacent title ink
             * in the subtitle's blank ascent and the other line's leading. */
            int first = bounds.y1 + (top + line * pitch) * lv_area_get_height(&bounds) /
                lv_obj_get_height(subtitle);
            int last = bounds.y1 + (top + glyph.box_h + line * pitch) *
                lv_area_get_height(&bounds) / lv_obj_get_height(subtitle) - 1;
            for (int y = first; y <= last; y++) {
                for (int x = bounds.x1; x <= bounds.x2; x++) {
                    const uint8_t *pixel = partial + ((size_t)y * 466 + x) * 3;
                    if (pixel[0] > 64 && pixel[1] > 64 && pixel[2] > 64) ink[line]++;
                }
            }
        }
        printf("wrapped subtitle scroll=%d lane=%dx%d ink=%u/%u\n", positions[position],
            lv_area_get_width(&bounds), lv_area_get_height(&bounds), ink[0], ink[1]);
        CHECK(ink[0] > 10 && ink[1] > 10);
        lv_obj_invalidate(orb_shell_root(s_shell)); lv_refr_now(s_display);
        sim_display_rgb(full);
        CHECK(!memcmp(partial, full, sizeof(full)));
    }
}
static void compact_subtitle(void)
{
    for (unsigned size = 0; size < ORB_CONTENT_SIZE_COUNT; size++) {
        CHECK(orb_fonts_set_content_size(size) == ORB_FONTS_OK);
        for (unsigned bold = 0; bold < 2; bold++) {
            CHECK(orb_fonts_set_bold_text(bold) == ORB_FONTS_OK);
            advance();
            lv_obj_t *subtitle = find(orb_shell_root(s_shell), "Alarms ring"); CHECK(subtitle);
            lv_obj_t *row = lv_obj_get_parent(subtitle), *list = lv_obj_get_parent(row);
            CHECK(lv_label_get_max_lines(subtitle) == 1);
            int width = lv_text_get_width("Alarms ring", strlen("Alarms ring"),
                lv_obj_get_style_text_font(subtitle, 0), &(lv_text_attributes_t){
                    .letter_space = lv_obj_get_style_text_letter_space(subtitle, 0),
                });
            const int positions[] = {0, 100};
            for (unsigned position = 0; position < 2; position++) {
                lv_obj_scroll_to_y(list, positions[position], LV_ANIM_OFF); advance();
                lv_area_t bounds, row_bounds;
                CHECK(orb_round_list_child_bounds(row, subtitle, &bounds));
                CHECK(orb_round_list_row_bounds(row, &row_bounds));
                int shown = width * lv_area_get_height(&row_bounds) / lv_obj_get_height(row);
                printf("compact subtitle size=%u bold=%u scroll=%d ink-width=%d lane=%d\n",
                    size, bold, positions[position], shown, lv_area_get_width(&bounds));
                if (size == ORB_CONTENT_SIZE_DEFAULT) CHECK(shown <= lv_area_get_width(&bounds));
            }
        }
    }
}
int main(void)
{
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); CHECK(orb_model_init()); CHECK(orb_home_init());
    orb_model_set_connection(true, true);
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL); CHECK(s_shell);
    orb_shell_snapshot_t status = {.net=ORB_SHELL_NET_CONNECTED, .link=ORB_SHELL_LINK_ONLINE,
        .brightness=70, .battery_pct=80};
    orb_shell_update(s_shell, &status); advance();
    orb_shell_navigate(s_shell, ORB_SHELL_SOUND); advance();
    CHECK(has(orb_shell_root(s_shell), "Alarms ring"));
    wrapped_subtitle();
    s_failed = true; advance(); CHECK(has(orb_shell_root(s_shell), ORB_ICON_WARNING " Not saved"));
    s_failed = false; s_saving = true; advance();
    CHECK(!has(orb_shell_root(s_shell), ORB_ICON_WARNING " Not saved"));
    s_saving = false; advance(); CHECK(has(orb_shell_root(s_shell), "Alarms ring"));
    compact_subtitle();
    orb_shell_destroy(s_shell);
    puts("sound save: failure, retry and recovery retain truthful root feedback");
    return 0;
}
