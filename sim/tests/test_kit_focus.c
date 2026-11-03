/* Shared focus/selection contract, using the production band renderer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_kit.h"
#include "sim_display.h"

#if defined(__APPLE__)
#define OPTIONAL_API __attribute__((weak_import))
#else
#define OPTIONAL_API __attribute__((weak))
#endif
extern void orb_kit_chip_set_selected(lv_obj_t *, bool) OPTIONAL_API;

extern size_t sim_memory_allocation_calls(void);
static uint32_t s_now;
static unsigned s_checks;
static uint8_t s_pixels[SIM_D * SIM_D * 3];
static uint32_t tick(void) { return s_now; }
static void require(bool valid, const char *message)
{
    s_checks++;
    if (!valid) { fprintf(stderr, "kit focus: %s\n", message); exit(EXIT_FAILURE); }
}
static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 8) { s_now += 8; lv_timer_handler(); }
    lv_refr_now(lv_display_get_default());
}
static unsigned white_between(void)
{
    sim_display_rgb(s_pixels);
    unsigned count = 0;
    for (unsigned y = 150; y < 310; y++) for (unsigned x = 190; x < 270; x++) {
        unsigned offset = (y * SIM_D + x) * 3;
        if (s_pixels[offset] > 180 && s_pixels[offset + 1] > 180 && s_pixels[offset + 2] > 180) count++;
    }
    return count;
}
static void snapshot(const char *directory, const char *name)
{
    if (!directory) return;
    char path[512]; snprintf(path, sizeof(path), "%s/%s.png", directory, name);
    require(sim_display_save_png(path), "capture failed");
}
static void allocation_free_focus(lv_obj_t *root)
{
    lv_obj_t *discs[6];
    for (unsigned i = 0; i < 6; i++) {
        discs[i] = orb_kit_disc_create(root, ORB_KIT_DISC_STANDARD, ORB_KIT_ACCENT_FILL, ORB_KIT_HOME, "");
        lv_obj_set_pos(discs[i], 30 + i * 60, 193);
    }
    lv_obj_t *summary = lv_label_create(root);
    lv_obj_set_size(summary, 240, 32); lv_label_set_text_static(summary, "First");
    orb_kit_disc_set_focus(discs[0], true); advance(1000);
    static const char *names[] = {"First", "Second", "Third", "Fourth", "Fifth", "Sixth"};
    /* Enqueue the sibling's one-shot refresh before the measurement. Its
     * registration is unrelated widget work; focus must leave it pending. */
    lv_label_set_text_static(summary, "Second");
    size_t before = sim_memory_allocation_calls();
    unsigned previous = 0;
    for (unsigned i = 1; i <= 80; i++) {
        unsigned next = i % 6;
        if (i % 2) { orb_kit_disc_set_focus(discs[previous], false); orb_kit_disc_set_focus(discs[next], true); }
        else { orb_kit_disc_set_focus(discs[next], true); orb_kit_disc_set_focus(discs[previous], false); }
        previous = next;
        /* A focus user's retained sibling may queue its own text layout.
         * Focus changes must not flush unrelated work under a crown sample. */
        lv_label_set_text_static(summary, names[next]);
    }
    size_t allocated = sim_memory_allocation_calls() - before;
    printf("kit focus: 80 synchronous transfers allocated %zu times\n", allocated);
    require(allocated == 0, "retained focus transfers allocate on first use");
    for (unsigned i = 0; i < 6; i++) lv_obj_delete(discs[i]);
    lv_obj_delete(summary);
}
static void reparented_focus(lv_obj_t *root)
{
    lv_obj_t *old_parent = lv_obj_create(root), *new_parent = lv_obj_create(root);
    lv_obj_remove_style_all(old_parent); lv_obj_remove_style_all(new_parent);
    lv_obj_set_size(old_parent, SIM_D, SIM_D); lv_obj_set_size(new_parent, SIM_D, SIM_D);
    lv_obj_t *a = orb_kit_disc_create(old_parent, ORB_KIT_DISC_STANDARD, ORB_KIT_ACCENT_FILL, ORB_KIT_HOME, "");
    lv_obj_t *b = orb_kit_disc_create(old_parent, ORB_KIT_DISC_STANDARD, ORB_KIT_ACCENT_FILL, ORB_KIT_HOME, "");
    lv_obj_set_pos(a, 80, 193); lv_obj_set_pos(b, 306, 193);
    orb_kit_disc_set_focus(a, true); advance(1000);
    lv_obj_set_parent(a, new_parent); lv_obj_set_parent(b, new_parent);
    /* Reparenting is an explicit preparation boundary, including unchanged focus. */
    orb_kit_disc_set_focus(a, true); orb_kit_disc_set_focus(b, false);
    lv_obj_delete(old_parent);
    size_t before = sim_memory_allocation_calls();
    orb_kit_disc_set_focus(a, false); orb_kit_disc_set_focus(b, true);
    require(sim_memory_allocation_calls() == before, "prepared reparented focus allocated on transfer");
    advance(120); require(white_between() > 10, "reparented controls lost their shared focus glide");
    lv_obj_delete(new_parent); advance(1000);
}
static void selector(lv_obj_t *root, const char *directory)
{
    lv_obj_t *a = orb_kit_button_secondary_create(root, ORB_ICON_SUN " 70%");
    lv_obj_t *b = orb_kit_button_secondary_create(root, ORB_ICON_SPEAKER_LOW " 50%");
    orb_kit_chip_set_value(a, ORB_ICON_SUN " 70%", ORB_ICON_SUN " 100%");
    orb_kit_chip_set_value(b, ORB_ICON_SPEAKER_LOW " 50%", ORB_ICON_SPEAKER_LOW " 100%");
    lv_obj_update_layout(root);
    lv_obj_set_pos(a, 143 - lv_obj_get_width(a)/2, 200);
    lv_obj_set_pos(b, 323 - lv_obj_get_width(b)/2, 200);
    require(orb_kit_chip_set_selected != NULL, "kit selector has no selected-state API");
    orb_kit_chip_set_selected(a, true); orb_kit_chip_set_selected(b, false);
    advance(1000);
    require(lv_obj_get_style_outline_width(a, 0) == ORB_KIT_FOCUS_WIDTH &&
            lv_obj_get_style_outline_pad(a, 0) == ORB_KIT_FOCUS_OFFSET,
            "selected chip lacks kit outline geometry");
    require(lv_obj_get_style_outline_width(b, 0) == 0, "unselected chip is outlined");
    require(lv_color_eq(lv_obj_get_style_outline_color(a, 0), ORB_LABEL), "selected chip outline is not white");
    require(lv_obj_has_state(a, LV_STATE_CHECKED) && !lv_obj_has_state(b, LV_STATE_CHECKED),
            "selector semantic state does not match the visible owner");
    snapshot(directory, "selector-brightness");
    orb_kit_chip_set_selected(b, true); orb_kit_chip_set_selected(a, false);
    advance(1000);
    require(lv_obj_get_style_outline_width(a, 0) == 0 &&
            lv_obj_get_style_outline_width(b, 0) == ORB_KIT_FOCUS_WIDTH,
            "selected chip did not follow the owner change");
    snapshot(directory, "selector-volume");
    lv_obj_delete(a); lv_obj_delete(b);
}
static void glide(lv_obj_t *root, bool reverse, const char *directory)
{
    lv_obj_t *a = orb_kit_disc_create(root, ORB_KIT_DISC_STANDARD, ORB_KIT_ACCENT_FILL, ORB_KIT_HOME, "");
    lv_obj_t *b = orb_kit_disc_create(root, ORB_KIT_DISC_STANDARD, ORB_KIT_ACCENT_FILL, ORB_KIT_HOME, "");
    lv_obj_set_pos(a, 80, 193); lv_obj_set_pos(b, 306, 193);
    orb_kit_disc_set_focus(a, true); advance(1000);
    require(white_between() == 0, "fixture begins with ink in the travel corridor");
    if (reverse) { orb_kit_disc_set_focus(b, true); orb_kit_disc_set_focus(a, false); }
    else { orb_kit_disc_set_focus(a, false); orb_kit_disc_set_focus(b, true); }
    advance(120);
    require(white_between() > 10, "focus jumped directly instead of gliding between discs");
    snapshot(directory, reverse ? "focus-glide-reverse-order" : "focus-glide");
    require(lv_obj_get_x(a) == 80 && lv_obj_get_x(b) == 306 &&
            lv_obj_get_width(a) == ORB_KIT_DISC && lv_obj_get_width(b) == ORB_KIT_DISC,
            "glide changed the controls' geometry");
    /* Retarget while moving; deletion during the second glide must retire it. */
    orb_kit_disc_set_focus(b, false); orb_kit_disc_set_focus(a, true);
    advance(120); require(white_between() > 0, "retargeting lost the moving focus ring");
    advance(1200);
    require(white_between() == 0 && lv_obj_get_style_outline_opa(a, 0) == LV_OPA_COVER,
            "focus did not settle onto the destination outline");
    snapshot(directory, "focus-settled");
    orb_kit_disc_set_focus(a, false); orb_kit_disc_set_focus(b, true);
    advance(48); lv_obj_delete(b); advance(1000);
    require(white_between() == 0, "deleted destination left a moving focus ring");
    lv_obj_delete(a);
}
static void row_glide(lv_obj_t *root, const char *directory)
{
    lv_obj_t *list = lv_obj_create(root); lv_obj_remove_style_all(list);
    lv_obj_set_size(list, SIM_D, SIM_D); lv_obj_set_scrollable(list, false);
    lv_obj_t *a = orb_kit_row_create(list, &(orb_kit_row_config_t){.title = "First", .glyph = ORB_ICON_GEAR, .accent = ORB_KIT_GREY});
    lv_obj_t *b = orb_kit_row_create(list, &(orb_kit_row_config_t){.title = "Second", .glyph = ORB_ICON_GEAR, .accent = ORB_KIT_GREY});
    lv_obj_set_pos(a, 24, 150); lv_obj_set_pos(b, 24, 278);
    /* Production lists can establish focus before registering their compositor. */
    orb_kit_row_set_focus(a, true); require(orb_kit_list_apply(list), "list helper unavailable");
    advance(1000);
    orb_kit_row_set_focus(a, false); orb_kit_row_set_focus(b, true);
    advance(120); sim_display_rgb(s_pixels);
    uint8_t *intermediate = malloc(sizeof(s_pixels)); require(intermediate != NULL, "pixel reservation failed");
    memcpy(intermediate, s_pixels, sizeof(s_pixels)); snapshot(directory, "focus-row-glide");
    advance(1200); sim_display_rgb(s_pixels);
    require(memcmp(intermediate, s_pixels, sizeof(s_pixels)) != 0, "row leading focus did not glide across the cached list");
    free(intermediate); lv_obj_delete(list);
}
static void reduced(lv_obj_t *root, const char *directory)
{
    lv_obj_t *a = orb_kit_disc_create(root, ORB_KIT_DISC_STANDARD, ORB_KIT_ACCENT_FILL, ORB_KIT_HOME, "");
    lv_obj_t *b = orb_kit_disc_create(root, ORB_KIT_DISC_STANDARD, ORB_KIT_ACCENT_FILL, ORB_KIT_HOME, "");
    lv_obj_set_pos(a, 80, 193); lv_obj_set_pos(b, 306, 193);
    orb_ui_set_reduce_motion(true);
    orb_kit_disc_set_focus(a, true); advance(1000);
    orb_kit_disc_set_focus(a, false); orb_kit_disc_set_focus(b, true);
    advance(72); require(white_between() == 0, "Reduce Motion slid focus across the content");
    sim_display_rgb(s_pixels);
    unsigned source = s_pixels[(187 * SIM_D + 120) * 3 + 2];
    unsigned destination = s_pixels[(187 * SIM_D + 346) * 3 + 2];
    require(source > 20 && source < 235 && destination > 20 && destination < 235,
            "Reduce Motion did not crossfade both fixed endpoint outlines");
    snapshot(directory, "focus-reduced-crossfade");
    advance(160);
    require(lv_obj_get_style_outline_opa(b, 0) == LV_OPA_COVER,
            "Reduce Motion did not finish its 150ms focus crossfade");
    orb_ui_set_reduce_motion(false); lv_obj_delete(a); lv_obj_delete(b);
}
int main(int argc, char **argv)
{
    lv_init(); lv_tick_set_cb(tick); require(sim_display_create() != NULL, "display unavailable");
    orb_ui_init(NULL);
    const char *kind = argc > 1 ? argv[1] : "all";
    const char *directory = argc > 2 ? argv[2] : NULL;
    if (argc > 3 && !strcmp(argv[3], "ax3-bold-rt")) {
        require(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK, "AX3 unavailable");
        require(orb_fonts_set_bold_text(true) == ORB_FONTS_OK, "Bold unavailable");
        orb_ui_set_reduce_transparency(true);
    }
    lv_obj_t *root = lv_screen_active(); lv_obj_set_scrollable(root, false);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0); lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    if (!strcmp(kind, "selector") || !strcmp(kind, "all")) selector(root, directory);
    if (!strcmp(kind, "glide") || !strcmp(kind, "all")) { allocation_free_focus(root); reparented_focus(root); glide(root, false, directory); glide(root, true, directory); row_glide(root, directory); reduced(root, directory); }
    lv_obj_t *temporary = lv_obj_create(root);
    lv_obj_t *a = orb_kit_disc_create(temporary, ORB_KIT_DISC_STANDARD, ORB_KIT_ACCENT_FILL, ORB_KIT_HOME, "");
    lv_obj_t *b = orb_kit_disc_create(temporary, ORB_KIT_DISC_STANDARD, ORB_KIT_ACCENT_FILL, ORB_KIT_HOME, "");
    lv_obj_set_pos(a, 80, 193); lv_obj_set_pos(b, 306, 193);
    orb_kit_disc_set_focus(a, true); orb_kit_disc_set_focus(a, false); orb_kit_disc_set_focus(b, true);
    lv_obj_delete(temporary); advance(1000);
    printf("kit focus: %u checks passed\n", s_checks);
    return EXIT_SUCCESS;
}
