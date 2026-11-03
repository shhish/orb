/* Isolated wire/input evidence; never contacts a player or changes Mac volume. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "link_v11.h"
#include "orb_fonts.h"
#include "orb_home.h"
#include "orb_input.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks;
static lv_display_t *s_display;
static orb_shell_t *s_shell;
static lv_indev_t *s_pointer, *s_physical, *s_synthetic;
static lv_indev_data_t s_input;
#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "NowPlaying %s:%d: %s\n", __FILE__, __LINE__, #value); exit(1); \
} } while (0)
static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *input, lv_indev_data_t *data) { (void)input; *data = s_input; }
static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 20) {
        s_now += 20; orb_shell_poll(s_shell); lv_timer_handler(); lv_refr_now(s_display);
    }
}
static lv_obj_t *find(lv_obj_t *object, const char *text)
{
    if (!lv_obj_is_visible(object)) return NULL;
    const char *rim = orb_kit_rim_text_get(object);
    if (rim && !strcasecmp(rim, text)) return object;
    if (lv_obj_check_type(object, &lv_label_class)) {
        const char *shown = lv_label_get_text(object);
        if (!strcmp(shown, text)) return object;
        /* Kit content keeps one line at AX3. LVGL's DOTS renderer updates
         * its visible buffer; the fixture identity remains its full string. */
        const char *dots = strstr(shown, "…");
        if (!dots) dots = strstr(shown, "...");
        if (dots && dots - shown >= 4 && !strncmp(shown, text, dots - shown)) return object;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); i++) {
        lv_obj_t *found = find(lv_obj_get_child(object, i), text);
        if (found) return found;
    }
    return NULL;
}
static void tap(const char *text)
{
    lv_obj_t *glyph = find(orb_shell_root(s_shell), text); CHECK(glyph);
    lv_obj_t *button = lv_obj_get_parent(glyph); CHECK(lv_obj_is_clickable(button));
    lv_area_t area; lv_obj_get_coords(button, &area);
    s_input.point = (lv_point_t){(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    s_input.state = LV_INDEV_STATE_PRESSED; lv_indev_read(s_pointer); advance(40);
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); advance(100);
}
static void fitted_labels(lv_obj_t *object)
{
    if (!lv_obj_is_visible(object)) return;
    if (lv_obj_check_type(object, &lv_label_class)) {
        const char *text = lv_label_get_text(object);
        lv_point_t size;
        lv_text_get_size(&size, text, lv_obj_get_style_text_font(object, 0),
            lv_obj_get_style_text_letter_space(object, 0), lv_obj_get_style_text_line_space(object, 0),
            lv_obj_get_width(object), LV_TEXT_FLAG_NONE);
        /* Dots deliberately ellipsize long metadata. Every actual line box
         * must still fit at BOTH circle chord ends, as in agents_main.c. */
        lv_area_t area; lv_obj_get_coords(object, &area);
        int chord = orb_chord_width(area.y1, area.y2, ORB_RIM_MARGIN_TEXT);
        int left = (466 - chord) / 2;
        if (area.x1 < left || area.x2 >= left + chord)
            fprintf(stderr, "Outside chord: %s (%d,%d)..(%d,%d), chord %d\n", text,
                area.x1, area.y1, area.x2, area.y2, chord);
        CHECK(area.x1 >= left && area.x2 < left + chord);
        CHECK(lv_obj_get_height(object) >= lv_obj_get_style_text_font(object, 0)->line_height);
        if (!strstr(text, "A much longer track name")) {
            CHECK(size.x <= lv_obj_get_width(object));
            CHECK(size.y <= lv_obj_get_height(object));
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); i++) fitted_labels(lv_obj_get_child(object, i));
}
static unsigned primary_count(lv_obj_t *obj)
{
    if (!lv_obj_is_visible(obj)) return 0;
    unsigned count = orb_kit_is_primary(obj) ? 1 : 0;
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); i++)
        count += primary_count(lv_obj_get_child(obj, i));
    return count;
}
static const char *s_snapshot =
    "{\"title\":\"Midnight City\",\"artist\":\"M83\",\"album\":\"Hurry Up, We’re Dreaming\","
    "\"duration\":243,\"position\":121,\"playing\":true,\"state\":\"playing\",\"app\":\"Music\",\"volume\":65}";
static void wire(const char *json)
{
    cJSON *value = cJSON_Parse(json); CHECK(value);
    CHECK(link_v11_event("nowplaying.snapshot", value) == LINK_V11_APPLIED);
    cJSON_Delete(value);
}
static void shot(const char *dir, const char *name)
{
    advance(2500); fitted_labels(orb_shell_root(s_shell));
    CHECK(primary_count(orb_shell_root(s_shell)) <= 1);
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.png", dir, name);
    CHECK(sim_display_save_png(path));
}
static void decoder_tests(void)
{
    cJSON *value = cJSON_Parse(s_snapshot); CHECK(value);
    orb_nowplaying_t media; CHECK(link_v11_read_nowplaying(value, &media));
    CHECK(!strcmp(media.title, "Midnight City") && media.position == 121 && media.volume == 65);
    cJSON_ReplaceItemInObjectCaseSensitive(value, "volume", cJSON_CreateNumber(101));
    CHECK(!link_v11_read_nowplaying(value, &media));
    cJSON_ReplaceItemInObjectCaseSensitive(value, "volume", cJSON_CreateBool(true));
    CHECK(!link_v11_read_nowplaying(value, &media));
    cJSON_ReplaceItemInObjectCaseSensitive(value, "volume", cJSON_CreateNumber(65));
    cJSON_ReplaceItemInObjectCaseSensitive(value, "position", cJSON_CreateNumber(-1));
    CHECK(!link_v11_read_nowplaying(value, &media));
    cJSON_ReplaceItemInObjectCaseSensitive(value, "position", cJSON_CreateNumber(1));
    cJSON_ReplaceItemInObjectCaseSensitive(value, "playing", cJSON_CreateBool(false));
    CHECK(!link_v11_read_nowplaying(value, &media));
    cJSON_ReplaceItemInObjectCaseSensitive(value, "playing", cJSON_CreateBool(true));
    char title[601];
    for (unsigned i = 0; i < 100; i++) memcpy(title + i * 6, "音楽", 6);
    title[600] = '\0';
    cJSON_ReplaceItemInObjectCaseSensitive(value, "title", cJSON_CreateString(title));
    CHECK(link_v11_read_nowplaying(value, &media));
    CHECK(strlen(media.title) == 159); /* Never retain half a UTF-8 character. */
    cJSON_AddNumberToObject(value, "volume", 65); CHECK(!link_v11_read_nowplaying(value, &media));
    cJSON_Delete(value);
    CHECK(!orb_model_nowplaying_request(ORB_MEDIA_VOLUME, 50));
    CHECK(!orb_model_set_nowplaying(&media));
    orb_model_set_connection(true, true); orb_model_nowplaying_capability(true); wire(s_snapshot);
    CHECK(!orb_model_nowplaying_request(ORB_MEDIA_VOLUME, -1));
    CHECK(!orb_model_nowplaying_request(ORB_MEDIA_VOLUME, 101));
    CHECK(orb_model_nowplaying_request(ORB_MEDIA_PAUSE, 0));
    CHECK(!orb_model_nowplaying_request(ORB_MEDIA_NEXT, 0));
    orb_media_request_t command; CHECK(orb_model_take_nowplaying_request(&command));
    CHECK(command.action == ORB_MEDIA_PAUSE); CHECK(!orb_model_take_nowplaying_request(&command));
    cJSON *params = link_v11_nowplaying_params(&command); CHECK(params);
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "action")), "pause"));
    CHECK(!cJSON_GetObjectItemCaseSensitive(params, "value")); cJSON_Delete(params);
    orb_model_complete_nowplaying("timeout"); CHECK(!orb_model_take_nowplaying_request(&command));
    orb_model_set_connection(false, false);
    static orb_model_snapshot_t snapshot; CHECK(orb_model_snapshot(&snapshot));
    CHECK(!snapshot.nowplaying_supported && !snapshot.nowplaying_valid && !snapshot.nowplaying_pending);
}
int main(int argc, char **argv)
{
    CHECK(argc >= 2); mkdir(argv[1], 0755);
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); CHECK(orb_model_init()); CHECK(orb_home_init());
    decoder_tests(); orb_model_reset();
    if (argc > 2 && !strcmp(argv[2], "ax3-bold-rt")) {
        CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
        CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK); orb_ui_set_reduce_transparency(true);
    }
    s_physical = lv_indev_create(); s_synthetic = lv_indev_create(); CHECK(s_physical && s_synthetic);
    lv_indev_t *inputs[] = {s_physical, s_synthetic};
    for (unsigned i = 0; i < 2; i++) {
        lv_indev_set_type(inputs[i], LV_INDEV_TYPE_POINTER); lv_indev_set_display(inputs[i], s_display);
        lv_indev_set_read_cb(inputs[i], read_pointer); lv_indev_set_mode(inputs[i], LV_INDEV_MODE_EVENT);
    }
    orb_input_mark_synthetic(s_synthetic); s_pointer = s_physical;
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL); CHECK(s_shell);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = -1, .brightness = 70};
    strcpy(status.hub_name, "Mac mini"); orb_shell_update(s_shell, &status);
    orb_model_set_connection(true, true); orb_shell_navigate(s_shell, ORB_SHELL_STACK); advance(1200);
    CHECK(!find(orb_shell_root(s_shell), "Midnight City"));
    orb_model_nowplaying_capability(true); advance(100);
    CHECK(!find(orb_shell_root(s_shell), "Midnight City"));
    wire(s_snapshot); advance(1000); orb_shell_navigate(s_shell, ORB_SHELL_STACK); advance(1000);
    CHECK(find(orb_shell_root(s_shell), "Midnight City"));
    shot(argv[1], "NowPlaying");

    lv_obj_t *pause = find(orb_shell_root(s_shell), ORB_ICON_PAUSE_FILL); CHECK(pause);
    lv_obj_t *previous = find(orb_shell_root(s_shell), ORB_ICON_SKIP_BACK_FILL);
    lv_obj_t *next = find(orb_shell_root(s_shell), ORB_ICON_SKIP_FORWARD_FILL);
    CHECK(previous && next);
    CHECK(lv_obj_get_width(lv_obj_get_parent(pause)) == ORB_KIT_DISC);
    CHECK(lv_obj_get_width(lv_obj_get_parent(previous)) == ORB_KIT_DISC);
    CHECK(lv_obj_get_width(lv_obj_get_parent(next)) == ORB_KIT_DISC);
    CHECK(orb_kit_is_primary(lv_obj_get_parent(pause)));
    CHECK(!orb_kit_is_primary(lv_obj_get_parent(previous)) && !orb_kit_is_primary(lv_obj_get_parent(next)));
    lv_obj_send_event(lv_obj_get_parent(pause), LV_EVENT_CLICKED, NULL);
    orb_media_request_t direct; CHECK(!orb_model_take_nowplaying_request(&direct));
    s_pointer = s_synthetic; tap(ORB_ICON_PAUSE_FILL);
    orb_media_request_t command; CHECK(!orb_model_take_nowplaying_request(&command));
    CHECK(orb_model_pending_answers() == 0); s_pointer = s_physical;
    tap(ORB_ICON_PAUSE_FILL); CHECK(orb_model_take_nowplaying_request(&command));
    CHECK(command.action == ORB_MEDIA_PAUSE); orb_model_complete_nowplaying(NULL); advance(100);
    /* Design 2.0 reserves volume for Control Center and Sound. Stack's rim
     * pages the visible cards and never emits a Mac-volume request. */
    bool paged = orb_shell_twist(s_shell, 2);
    CHECK(!orb_model_take_nowplaying_request(&command));
    if (paged) { CHECK(orb_shell_twist(s_shell, -2)); advance(500); }
    orb_model_complete_nowplaying(NULL); advance(100);
    wire("{\"title\":\"\",\"artist\":\"\",\"album\":\"\",\"duration\":0,\"position\":0,"
         "\"playing\":false,\"state\":\"stopped\",\"app\":\"\",\"volume\":65}");
    shot(argv[1], "NowPlayingIdle"); CHECK(find(orb_shell_root(s_shell), "Nothing playing"));
    CHECK(!find(orb_shell_root(s_shell), "Paused on Mac mini"));
    CHECK(!find(orb_shell_root(s_shell), "Paused on Mac"));
    CHECK(!find(orb_shell_root(s_shell), "0:00"));
    wire("{\"title\":\"A much longer track name that exceeds the available circular chord\",\"artist\":\"A long artist name\","
         "\"album\":\"\",\"duration\":0,\"position\":0,\"playing\":false,\"state\":\"paused\",\"app\":\"Spotify\",\"volume\":20}");
    shot(argv[1], "NowPlayingLongTitle");
    orb_model_nowplaying_capability(false); advance(1000);
    CHECK(!find(orb_shell_root(s_shell), "A much longer track name that exceeds the available circular chord"));
    CHECK(!orb_model_nowplaying_request(ORB_MEDIA_PLAY, 0));
    orb_model_nowplaying_capability(true); wire(s_snapshot); advance(1600);
    lv_obj_t *title = find(orb_shell_root(s_shell), "Midnight City"); CHECK(title);
    lv_mem_monitor_t before, after; lv_mem_monitor(&before);
    lv_obj_clean(lv_obj_get_parent(title)); lv_mem_monitor(&after);
    CHECK(before.total_size > after.total_size);
    printf("NowPlaying sizes: retained LVGL tree=%zu bytes; C view state=0; media=%zu; request=%zu (host payload; no owner budgets)\n",
        before.total_size - after.total_size, sizeof(orb_nowplaying_t), sizeof(orb_media_request_t));
    orb_shell_destroy(s_shell); lv_indev_delete(s_physical); lv_indev_delete(s_synthetic);
    printf("NowPlaying: %u wire/render/input checks passed\n", s_checks); return 0;
}
