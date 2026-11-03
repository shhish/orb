/* Reuse the permanent parity catalogue's actual presenters and frozen models.
 * This does not change its renderer, baseline frames, or production input. */
#define main font_parity_original_main
#include "../../../../../sim/parity_main.c"
#undef main
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

void font_surfaces_prepare(void);
void font_surfaces_switch(unsigned pair, bool retained);
void font_surfaces_complete(bool failed);

static unsigned notice_count(lv_obj_t *root)
{
    unsigned count = 0;
    if (lv_obj_is_hidden(root)) return 0;
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), "Not saved")) count++;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++)
        count += notice_count(lv_obj_get_child(root, i));
    return count;
}

static void choose_text(const char *value)
{
    lv_obj_t *label = find_text(parity_root(), value);
    parity_check(label != NULL, value);
    lv_obj_scroll_to_view_recursive(label, LV_ANIM_OFF);
    parity_advance(64);
    parity_check(parity_tap_text(value), value);
    parity_advance(320);
}

static void notice_lifetime(void)
{
    font_surfaces_switch(0, false);
    parity_navigate(ORB_SHELL_ACCESSIBILITY); parity_advance(720);
    choose_text("Text size");
    choose_text("Larger"); font_surfaces_complete(true); parity_advance(64);
    assert(notice_count(parity_root()) == 1 && orb_fonts_content_size() == ORB_CONTENT_SIZE_DEFAULT);
    choose_text("Default"); /* successful no-op clears the previous failure */
    assert(!orb_fonts_pending() && notice_count(parity_root()) == 0);
    choose_text("Larger"); font_surfaces_complete(true); parity_advance(64);
    assert(notice_count(parity_root()) == 1);
    orb_shell_back(parity_shell); parity_advance(400);
    choose_text("Reduce motion"); /* unrelated successful setting clears the notice */
    assert(notice_count(parity_root()) == 0);
    choose_text("Text size");
    choose_text("Larger"); font_surfaces_complete(true); parity_advance(64);
    assert(notice_count(parity_root()) == 1);
    choose_text("Larger"); choose_text("Default");
    font_surfaces_complete(false); parity_advance(64);
    assert(notice_count(parity_root()) == 0 && orb_fonts_content_size() == ORB_CONTENT_SIZE_DEFAULT);
    choose_text("Larger");
    orb_shell_back(parity_shell); parity_advance(400); /* leave detail before completion */
    font_surfaces_complete(true); parity_advance(64);
    assert(notice_count(parity_root()) == 0);
    choose_text("Text size");
    choose_text("Larger");
    parity_navigate(ORB_SHELL_APPS); parity_advance(400);
    parity_navigate(ORB_SHELL_ACCESSIBILITY); parity_advance(400);
    if (find_text(parity_root(), "Larger")) { orb_shell_back(parity_shell); parity_advance(400); }
    choose_text("Reduce motion"); /* must not reattach an abandoned font request */
    font_surfaces_complete(true); parity_advance(64);
    assert(notice_count(parity_root()) == 0);
    if (!find_text(parity_root(), "Larger")) choose_text("Text size");
    choose_text("Larger"); font_surfaces_complete(true); parity_advance(64);
    assert(notice_count(parity_root()) == 1);
    choose_text("Larger"); font_surfaces_complete(false); parity_advance(64);
    assert(notice_count(parity_root()) == 0 && orb_fonts_content_size() == ORB_CONTENT_SIZE_LARGER);
    puts("font notices: once, no-op/coalesced success, local/outer leave and retry success passed");
}

int font_surfaces_main(const char *scenario)
{
    unsigned count = parity_shell_count + parity_app_count +
        sizeof(s_transitions) / sizeof(s_transitions[0]) +
        sizeof(s_privacy) / sizeof(s_privacy[0]) + sizeof(s_time_layers) / sizeof(s_time_layers[0]);
    s_id = scenario; s_profile = "font-lifetime";
    for (unsigned i = 0; i < count; i++) if (!strcmp(s_id, scenario_at(i)->id)) s_scenario = scenario_at(i);
    bool navigation = !strcmp(scenario, "navigation");
    parity_check(navigation || s_scenario != NULL, "font lifetime scenario exists");
    setenv("TZ", "UTC", 1); tzset(); setlocale(LC_ALL, "C"); srand(1330790992);
    font_surfaces_prepare();
    lv_tick_set_cb(tick); lv_rand_set_seed(1330790992);
    parity_display = sim_display_create(); orb_ui_init(NULL);
    parity_check(orb_model_init() && orb_home_init(), "font lifetime models");
    orb_time_init(); parity_check(orb_sound_init(), "font lifetime sound");
    s_pointer = lv_indev_create(); lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_pointer, read_pointer); lv_indev_set_display(s_pointer, parity_display);
    /* Screens are constructed before the first retirement, then survive all
     * 36 pairs. That includes private/public Face hosts above the shell and
     * the navigation compositor's suspended, off-tree screen objects. */
    if (navigation) {
        parity_shell_create(true);
        for (unsigned surface = 0; surface <= ORB_SHELL_SURFACE_LAST; surface++) {
            parity_navigate((orb_shell_surface_t)surface); parity_advance(400);
        }
    } else s_scenario->setup(s_id);
    for (unsigned from = 0; from < 6; from++) {
        for (unsigned to = 0; to < 6; to++) {
            font_surfaces_switch(from, false);
            font_surfaces_switch(to, true);
            s_event_count = 0;
            if (navigation) {
                for (unsigned surface = 0; surface <= ORB_SHELL_SURFACE_LAST; surface++) {
                    parity_navigate((orb_shell_surface_t)surface); parity_advance(400);
                }
            }
            lv_obj_invalidate_raster(lv_screen_active());
            lv_obj_invalidate(lv_screen_active());
            parity_advance(64);
        }
    }
    if (navigation) notice_lifetime();
    printf("font lifetime %s: 36 pairs, pre-existing presenters, retired heap/mappings, held glyphs and queued draws\n", scenario);
    return 0;
}
