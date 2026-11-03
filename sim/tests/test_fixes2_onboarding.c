/* Exercise the real view with only its asynchronous storage boundary faked. */
#include "../../firmware/components/orb_shell/system/system_views.h"
static orb_system_views_t *create_views(bool preview,
    bool (*event)(void *, orb_system_view_event_t), void *context);
#define orb_system_views_create create_views
#include "../../firmware/components/orb_shell/orb_workspace.c"
#undef orb_system_views_create
#include "sim_display.h"

static bool s_needed, s_saving, s_error, s_finish_during_read;
static unsigned s_requests;
bool fixes2_needed(void) { return s_needed; }
bool fixes2_saving(void)
{
    bool observed = s_saving;
    if (s_finish_during_read && s_saving) {
        s_finish_during_read = s_saving = s_needed = false;
    }
    return observed;
}
bool fixes2_error(void) { return s_error; }
bool fixes2_dismiss(void)
{
    if (s_saving) return false;
    ++s_requests; s_saving = true; s_error = false;
    return true;
}
static orb_system_views_t *create_views(bool preview,
    bool (*event)(void *, orb_system_view_event_t), void *context)
{
    orb_system_views_t *views = orb_system_views_create(preview, event, context);
    orb_system_views_onboarding_port(views, &(orb_onboarding_port_t){
        fixes2_needed, fixes2_saving, fixes2_error, fixes2_dismiss});
    return views;
}
static uint32_t s_now;
static lv_indev_data_t s_input;
static lv_indev_t *s_pointer;
static lv_display_t *s_display;
static orb_shell_t *s_shell;
#define CHECK(v) do { if (!(v)) { fprintf(stderr, "onboarding UI:%d %s\n", __LINE__, #v); exit(1); } } while (0)
static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *pointer, lv_indev_data_t *out) { (void)pointer; *out = s_input; }
static void step(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 20) { s_now += 20; orb_shell_poll(s_shell); lv_timer_handler(); }
    lv_refr_now(s_display);
}
static lv_obj_t *find(lv_obj_t *obj, const char *text)
{
    if (lv_obj_is_hidden(obj)) return NULL;
    const char *button = orb_kit_button_get_text(obj);
    if ((button && !strcmp(button, text)) ||
        (lv_obj_check_type(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), text))) return obj;
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *found = find(lv_obj_get_child(obj, i), text); if (found) return found;
    }
    return NULL;
}
static void tap(const char *text)
{
    lv_obj_t *obj = find(orb_shell_root(s_shell), text); CHECK(obj);
    while (!lv_obj_is_clickable(obj)) obj = lv_obj_get_parent(obj);
    /* Settings destinations can lie below the viewport at large text sizes.
     * Reach the real row before sending its physical pointer contact. */
    lv_obj_scroll_to_view(obj, LV_ANIM_OFF); step(100);
    CHECK(lv_obj_is_visible(obj));
    lv_area_t area; lv_obj_get_coords(obj, &area);
    s_input = (lv_indev_data_t){.point={(area.x1+area.x2)/2,(area.y1+area.y2)/2}, .state=LV_INDEV_STATE_PRESSED};
    lv_indev_read(s_pointer); step(40);
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); step(100);
}
static void maintenance(void)
{
    orb_shell_navigate(s_shell, ORB_SHELL_SETTINGS); step(1200);
    tap("Mac"); tap("Maintenance"); step(1200);
    CHECK(find(orb_shell_root(s_shell), "Unpair Mac"));
}

static void shot(const char *dir, const char *name)
{
    if (!dir) return;
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.png", dir, name);
    CHECK(sim_display_save_png(path));
}
int main(int argc, char **argv)
{
    lv_init(); lv_tick_set_cb(tick); orb_ui_init(NULL); CHECK(orb_model_init() && orb_home_init());
    if (argc > 1 && !strcmp(argv[1], "ax3-bold-rt")) {
        CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK); orb_fonts_set_bold_text(true);
        orb_ui_set_reduce_transparency(true);
    }
    s_display = sim_display_create(); CHECK(s_display);
    /* Polling before the first async render must observe a saving transition;
     * completion then leaves even if no tutorial tree has been built yet. */
    orb_workspace_poll(NULL);
    orb_workspace_t *probe = orb_workspace_create(lv_screen_active(), NULL, NULL, NULL); CHECK(probe);
    orb_workspace_navigate(probe, ORB_SHELL_ONBOARDING);
    s_needed = s_saving = true; orb_workspace_poll(probe);
    s_needed = s_saving = false; orb_workspace_poll(probe);
    CHECK(orb_workspace_surface(probe) == ORB_SHELL_FACE);
    orb_workspace_destroy(probe);
    s_pointer = lv_indev_create(); lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_pointer, read_pointer); lv_indev_set_display(s_pointer, s_display);
    orb_model_set_connection(true, true);
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL); CHECK(s_shell);
    orb_shell_snapshot_t status = {.net=ORB_SHELL_NET_CONNECTED, .link=ORB_SHELL_LINK_ONLINE,
        .brightness=70, .battery_pct=80};
    orb_shell_update(s_shell, &status); step(1200);
    uint8_t *material_before = malloc(SIM_D * SIM_D * 3), *material_after = malloc(SIM_D * SIM_D * 3);
    CHECK(material_before && material_after);
    maintenance(); sim_display_rgb(material_before);
    shot(argc > 2 ? argv[2] : NULL, "maintenance-before-notice");
    orb_shell_back(s_shell); step(1200); orb_shell_back(s_shell); step(1200);
    orb_shell_navigate(s_shell, ORB_SHELL_ONBOARDING); step(1200);
    s_needed = true;
    /* A system notice retires an existing physical contact. Its later UP
     * cannot acknowledge a tutorial that the owner can no longer see. */
    lv_obj_t *got = find(orb_shell_root(s_shell), "Got it"); CHECK(got);
    lv_area_t held_area; lv_obj_get_coords(got, &held_area);
    s_input = (lv_indev_data_t){.point={(held_area.x1+held_area.x2)/2,(held_area.y1+held_area.y2)/2},
        .state=LV_INDEV_STATE_PRESSED};
    lv_indev_read(s_pointer); step(40);
    status.battery_pct = 20; orb_shell_update(s_shell, &status); step(40);
    CHECK(find(lv_display_get_layer_top(s_display), "20% · Battery"));
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); step(100);
    CHECK(s_requests == 0 && orb_shell_surface(s_shell) == ORB_SHELL_ONBOARDING);
    orb_shell_button(s_shell, &(orb_button_event_t){.button=ORB_BUTTON_BOOT,.gesture=ORB_BUTTON_SHORT});
    step(100); CHECK(orb_kit_press_is_cancelled(got) && !lv_obj_has_state(got, LV_STATE_PRESSED));
    tap("Got it");
    CHECK(s_requests == 1 && s_saving && s_needed);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_ONBOARDING);
    CHECK(find(orb_shell_root(s_shell), "Saving…"));
    shot(argc > 2 ? argv[2] : NULL, "onboarding-saving");
    s_saving = false; s_error = true; step(100);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_ONBOARDING);
    CHECK(find(orb_shell_root(s_shell), "Not saved"));
    CHECK(find(orb_shell_root(s_shell), "Try again"));
    lv_area_t error_area, action_area;
    lv_obj_get_coords(find(orb_shell_root(s_shell), "Not saved"), &error_area);
    lv_obj_get_coords(find(orb_shell_root(s_shell), "Try again"), &action_area);
    CHECK(error_area.y2 + 20 < action_area.y1);
    shot(argc > 2 ? argv[2] : NULL, "onboarding-error");
    tap("Try again"); CHECK(s_requests == 2 && s_saving && s_needed);
    s_saving = false; s_needed = false; step(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    /* Completion can race the view reconstruction after acceptance. The
     * renderer must not erase the last observed saving transition. */
    orb_shell_navigate(s_shell, ORB_SHELL_ONBOARDING); step(1200);
    s_needed = s_finish_during_read = true; tap("Got it");
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE && !s_needed && s_requests == 3);
    status.battery_pct = 80; orb_shell_update(s_shell, &status); step(1200);
    maintenance(); sim_display_rgb(material_after);
    shot(argc > 2 ? argv[2] : NULL, "maintenance-after-notice");
    CHECK(!memcmp(material_before, material_after, SIM_D * SIM_D * 3));
    free(material_before); free(material_after);
    orb_shell_destroy(s_shell);
    puts("onboarding UI: remains visible while saving, failure/retry, durable completion pass");
    return 0;
}
