/* Cross-component Home regressions: real shell navigation, model revisions,
 * hold ownership and outer-edge gesture arbitration. Synthetic data only. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_fonts.h"
#include "orb_home.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include "src/widgets/arclabel/lv_arclabel_private.h"
#include <ctype.h>
#include "orb_input.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks;
static orb_shell_t *s_shell;
static lv_display_t *s_display;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;

static void check(bool value, const char *why)
{
    ++s_checks;
    if (value) return;
    fprintf(stderr,"Home shell: %s (%u ms)\n",why,s_now);
    const char *capture = getenv("ORB_HOME_TEST_FAILURE");
    if (capture) sim_display_save_png(capture);
    exit(1);
}
static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *pointer, lv_indev_data_t *data) { (void)pointer; *data=s_input; }
static void advance(unsigned milliseconds)
{
    unsigned end=s_now+milliseconds;
    do {
        s_now += LV_MIN(8,end-s_now);
        orb_shell_poll(s_shell);
        lv_timer_handler();
        lv_refr_now(s_display);
    } while(s_now<end);
}
static void press(int x,int y)
{
    s_input.point=(lv_point_t){x,y};s_input.state=LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_pointer);advance(16);
}
static void release(void)
{
    s_input.state=LV_INDEV_STATE_RELEASED;lv_indev_read(s_pointer);advance(16);
}
static void tap(int x,int y) { press(x,y);release();advance(800); }
static lv_obj_t *find_text(lv_obj_t *root,const char *text)
{
    if(!lv_obj_is_visible(root))return NULL;
    if(lv_obj_check_type(root,&lv_label_class)&&lv_obj_get_style_text_opa(root,0)!=LV_OPA_TRANSP&&
       strstr(lv_label_get_text(root),text))return root;
    for(unsigned i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *found=find_text(lv_obj_get_child(root,(int32_t)i),text);if(found)return found;
    }
    return NULL;
}
/* Ellipsis mutates LVGL's label buffer while the row is on screen. Resolve
 * canonical accessory names through their stable row ID rather than text. */
static lv_obj_t *home_row_label(lv_obj_t *root, const char *id)
{
    lv_obj_t *parent = lv_obj_get_parent(root);
    const char *data = lv_obj_get_user_data(root);
    if (parent && lv_obj_is_scrollable(parent) && lv_obj_is_clickable(root) &&
        (uintptr_t)data > 4096 && !strcmp(data, id)) {
        for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
            lv_obj_t *child = lv_obj_get_child(root, (int32_t)i);
            if (lv_obj_check_type(child, &lv_label_class)) return child;
        }
        return NULL;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *label = home_row_label(lv_obj_get_child(root, (int32_t)i), id);
        if (label) return label;
    }
    return NULL;
}
static lv_obj_t *semantic_control(lv_obj_t *root, const char *id)
{
    const char *data = lv_obj_get_user_data(root);
    if ((uintptr_t)data > 4096 && !strcmp(data, id)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = semantic_control(lv_obj_get_child(root, i), id);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *home_named_label(lv_obj_t *root, const char *caption)
{
    orb_home_snapshot_t snapshot;
    if (!orb_home_snapshot(&snapshot)) return NULL;
    for (unsigned i = 0; i < snapshot.accessory_count; i++) {
        const orb_home_accessory_t *a = &snapshot.accessories[i];
        if (!strcmp(a->name, caption)) return home_row_label(root, a->id);
    }
    for (unsigned i = 0; i < snapshot.room_count; i++) {
        if (strcmp(snapshot.rooms[i].name, caption)) continue;
        char id[64]; snprintf(id, sizeof(id), "room:%s", snapshot.rooms[i].id);
        lv_obj_t *disc = semantic_control(root, id);
        if (disc) return disc;
    }
    return NULL;
}
static lv_obj_t *rim_identity(lv_obj_t *root, const char *caption)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (lv_obj_check_type(root, &lv_arclabel_class)) {
        char caps[96];
        size_t n = strlen(caption);
        if (n >= sizeof(caps)) return NULL;
        for (size_t i = 0; i <= n; ++i) caps[i] = (char)toupper((unsigned char)caption[i]);
        const char *actual = ((lv_arclabel_t *)root)->text;
        if (actual && strstr(actual, caps)) {
            lv_obj_t *obj = root;
            orb_kit_rim_text_info_t info;
            while (obj && !orb_kit_rim_text_info(obj, &info)) obj = lv_obj_get_parent(obj);
            return obj;
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = rim_identity(lv_obj_get_child(root, i), caption);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *find(lv_obj_t *root, const char *caption)
{
    lv_obj_t *text = find_text(root, caption);
    if (text) return text;
    lv_obj_t *label = home_named_label(root, caption);
    if (label) return label;
    if (!strcmp(caption, "Scenes")) {
        label = semantic_control(root, "scenes:open");
        if (label) return label;
    }
    return rim_identity(root, caption);
}
static void tap_text(const char *text)
{
    lv_obj_t *label = find(orb_shell_root(s_shell), text);
    if (!label) fprintf(stderr, "Home shell missing control: %s surface=%d\n", text, orb_shell_surface(s_shell));
    check(label != NULL, "named Home control exists");
    lv_obj_t *control = lv_obj_is_clickable(label) ? label : lv_obj_get_parent(label);
    if (!lv_obj_is_clickable(control)) fprintf(stderr, "Home shell passive caption: %s\n", text);
    check(lv_obj_is_clickable(control), "named Home control is interactive");
    lv_area_t bounds;
    lv_obj_t *list = lv_obj_get_parent(control);
    if (list && lv_obj_is_scrollable(list)) {
        lv_obj_stop_scroll_anim(list);
        lv_obj_scroll_to_y(list, LV_MAX(0, lv_obj_get_y(control) -
            (lv_obj_get_height(list) - lv_obj_get_height(control)) / 2), LV_ANIM_OFF);
        advance(400);
    }
    if (!orb_round_list_child_bounds(control, label, &bounds)) lv_obj_get_coords(control, &bounds);
    tap((bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2);
}
static void boot_back(void)
{
    orb_button_event_t event = {.button = ORB_BUTTON_BOOT, .gesture = ORB_BUTTON_SHORT,
        .at_ms = s_now, .synthetic = false};
    orb_shell_button(s_shell, &event);
    advance(1000);
}
static void edge_back(void)
{
    press(17, 233);
    for (unsigned i = 1; i <= 8; i++) press(17 + (int)i * 25, 233);
    release(); advance(1000);
}
static unsigned count(lv_obj_t *root,const char *text)
{
    /* Faces retain inactive provider forms to make crown selection allocation
     * free. Count only the selected visible presentation, not cached sources. */
    if(!lv_obj_is_visible(root))return 0;
    unsigned total=lv_obj_check_type(root,&lv_label_class)&&strstr(lv_label_get_text(root),text)!=NULL;
    for(unsigned i=0;i<lv_obj_get_child_count(root);++i)total+=count(lv_obj_get_child(root,(int32_t)i),text);
    return total;
}
static lv_obj_t *find_any_text(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_any_text(lv_obj_get_child(root, (int32_t)i), text);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *find_any(lv_obj_t *root, const char *caption)
{
    lv_obj_t *label = home_named_label(root, caption);
    return label ? label : find_any_text(root, caption);
}
static void unrelated_revision(unsigned revision)
{
    orb_session_t session={.agent=ORB_AGENT_CODEX,.state=ORB_SESSION_BUSY};
    snprintf(session.id,sizeof(session.id),"home-shell-background");
    snprintf(session.title,sizeof(session.title),"Background build %u",revision);
    check(orb_model_upsert_session(&session),"publish unrelated agent revision");
    orb_shell_poll(s_shell);
}
static void capture_doorbell_receipt(const char *name)
{
    const char *directory = getenv("ORB_HOME_RECEIPT_CAPTURES");
    if (!directory || !directory[0]) return;
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s/%s.png", directory, name);
    check(length > 0 && (size_t)length < sizeof(path), "doorbell receipt capture path fits");
    check(sim_display_save_png(path), "save deterministic doorbell receipt capture");
}

static void doorbell_receipts(const orb_card_t *original)
{
    static const struct {
        const char *name, *title, *explanation, *error;
        bool accepted, delivered;
    } s_outcomes[] = {
        {"accepted-undelivered", "Recorded", "Saved on your Mac, but the session didn't get it. Answer there.", NULL, true, false},
        {"delivered", "Sent", "Answer delivered.", NULL, true, true},
        {"failed", "Not sent", "Your Mac didn't take this answer. Check the session.", "unavailable", false, false},
        {"cancelled", "Not sent", "This closed before your answer was sent.", "cancelled", false, false},
    };
    for (unsigned i = 0; i < sizeof(s_outcomes) / sizeof(s_outcomes[0]); i++) {
        orb_card_t card = *original;
        if (i) {
            snprintf(card.id, sizeof(card.id), "home-doorbell-receipt-%u", i);
            check(orb_model_upsert_card(&card), "publish a distinct doorbell for each receipt outcome");
            advance(800); orb_shell_select_card(s_shell, card.id); advance(1000);
            tap(233, 399);
        }
        orb_answer_t answer;
        check(orb_model_pending_answers() == 1 && orb_model_peek_answer(&answer) &&
            !strcmp(answer.card, card.id) && !strcmp(answer.option, "dismiss"),
            "one explicit doorbell Dismiss owns exactly one answer");
        check(find_any_text(orb_shell_root(s_shell), "Queued") &&
            find_any_text(orb_shell_root(s_shell), "Dismiss · sending") &&
            find_any_text(orb_shell_root(s_shell), "Waiting for your Mac."),
            "pending Home receipt retains its legacy wording");
        /* No receipt state may replay the removed Dismiss action or auto-leave
         * after the former 600 ms countdown. Exercise the real physical path. */
        tap(233, 399); advance(1200);
        check(orb_shell_surface(s_shell) == ORB_SHELL_CARD && orb_model_pending_answers() == 1,
            "pending receipt waits for explicit departure without repeating Dismiss");
        if (!i) capture_doorbell_receipt("queued");
        check(orb_model_complete_answer(card.id, s_outcomes[i].accepted ? &answer : NULL,
            s_outcomes[i].delivered, s_outcomes[i].error), "complete the canonical doorbell answer");
        advance(1200);
        check(find_any_text(orb_shell_root(s_shell), s_outcomes[i].title) &&
            find_any_text(orb_shell_root(s_shell), "Dismiss") &&
            find_any_text(orb_shell_root(s_shell), s_outcomes[i].explanation),
            "resolved Home receipt retains its exact outcome and decision wording");
        tap(233, 399); advance(1200);
        check(orb_shell_surface(s_shell) == ORB_SHELL_CARD && !orb_model_pending_answers() &&
            !orb_model_peek_answer(&answer), "resolved receipt persists without replaying its answer");
        capture_doorbell_receipt(s_outcomes[i].name);
        boot_back();
        check(orb_shell_surface(s_shell) != ORB_SHELL_CARD && !orb_model_pending_answers(),
            "physical Back explicitly leaves the Home receipt without a new answer");
    }
}
static void vertical_drag(int from, int to)
{
    press(233, from);
    for (unsigned i = 1; i <= 8; i++) press(233, from + (to-from) * (int)i / 8);
    release(); advance(1000);
}
/* A retained row can exist far outside its viewport. Resolve its rendered
 * bounds instead of mistaking an offscreen label for a visible device. */
static bool visible_text(const char *caption)
{
    lv_obj_t *label = find_any(orb_shell_root(s_shell), caption);
    if (!label) return false;
    lv_obj_t *row = lv_obj_get_parent(label);
    lv_area_t area;
    if (!orb_round_list_child_bounds(row, label, &area)) return false;
    lv_point_t point = {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    return orb_round_list_hit_test(row, &point);
}
static void scroll_to_text(const char *caption)
{
    lv_obj_t *label = find_any(orb_shell_root(s_shell), caption);
    check(label != NULL, "canonical room accessory has a retained row");
    lv_obj_t *row = lv_obj_get_parent(label), *list = lv_obj_get_parent(row);
    check(lv_obj_is_scrollable(list), "accessory setup uses the native room scroll owner");
    /* Lock/brightness cases exercise actuation, not fling distance. Position
     * their stable target centrally; busy_room_inputs covers real gestures. */
    lv_obj_stop_scroll_anim(list);
    lv_obj_scroll_to_y(list, LV_MAX(0, lv_obj_get_y(row) -
        (lv_obj_get_height(list) - lv_obj_get_height(row)) / 2), LV_ANIM_OFF);
    advance(400);
    check(visible_text(caption), "device is visibly centred in its embedded room");
}
static void busy_room_inputs(void)
{
    orb_home_snapshot_t *original = calloc(1, sizeof(*original));
    check(original && orb_home_snapshot(original), "save Home before shell scroll fixture");
    orb_home_room_t room = {.id="busy", .name="Bench Room"};
    orb_home_accessory_t accessories[24] = {0};
    for (unsigned i = 0; i < 24; i++) {
        orb_home_accessory_t *a = &accessories[i];
        snprintf(a->id, sizeof(a->id), "busy-%02u", i + 1);
        snprintf(a->name, sizeof(a->name), "Device %02u", i + 1);
        strcpy(a->room, room.id);
        a->kind = ORB_HOME_LIGHT; a->reachable = true;
        a->writable = ORB_HOME_ON | ORB_HOME_BRIGHTNESS;
        a->state = (orb_home_state_t){.present=ORB_HOME_ON|ORB_HOME_BRIGHTNESS,.on=true,.brightness=60};
    }
    check(orb_home_snapshot_chunk(&room, 1, accessories, 24, NULL, 0, true, false), "publish 24-device shell room");
    orb_shell_navigate(s_shell, ORB_SHELL_HOME); advance(1200); tap_text("Bench Room");
    check(visible_text("Device 01"), "embedded room opens with its first device visible");
    lv_obj_t *first = find_any(orb_shell_root(s_shell), "Device 01");
    lv_obj_t *list = lv_obj_get_parent(lv_obj_get_parent(first));
    check(lv_obj_is_scrollable(list), "device rows belong to their actual scroll owner");
    int previous = 0;
    for (unsigned i = 0; i < 24; i++) {
        char name[24]; snprintf(name, sizeof(name), "Device %02u", i + 1);
        lv_obj_t *label = find_any(list, name);
        check(label != NULL, "all 24 devices remain retained in the scroll owner");
        lv_obj_t *row = lv_obj_get_parent(label);
        check(lv_obj_get_parent(row) == list && lv_obj_get_width(row) == 418 &&
            lv_obj_get_height(row) >= ORB_KIT_ROW_H, "every room row uses the shared kit role");
        int y = lv_obj_get_y(row);
        if (i) check(y == previous, "kit row cadence stays uniform through the entire room");
        previous = y + lv_obj_get_height(row) + 12;
    }
    check(!visible_text("Device 24"), "retained final device is not falsely treated as visible");
    int maximum = lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list);
    check(maximum > 1000, "large room exposes a continuous native scroll extent");
    check(orb_shell_twist(s_shell, 1), "case twist has a real shell list owner"); advance(800);
    check(lv_obj_is_valid(list) && find_any(list, "Device 01") == first && lv_obj_get_scroll_y(list) > 0,
        "case twist translates retained rows without replacing a page");
    check(orb_shell_twist(s_shell, -1), "reverse case twist returns toward the first row"); advance(800);
    check(lv_obj_get_scroll_y(list) == 0 && visible_text("Device 01"), "reverse case twist restores the first row");
    vertical_drag(300, 170);
    int forward = lv_obj_get_scroll_y(list);
    check(orb_shell_surface(s_shell) == ORB_SHELL_HOME && forward > 0 && find_any(list, "Device 01") == first,
        "embedded vertical drag scrolls retained devices without opening a system place");
    vertical_drag(170, 300);
    check(orb_shell_surface(s_shell) == ORB_SHELL_HOME && lv_obj_get_scroll_y(list) < forward,
        "reverse drag scrolls toward the first row instead of opening Notifications");
    lv_obj_stop_scroll_anim(list); lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF); advance(400);
    for (unsigned i = 0; i <= 12; i++) {
        float radians = (200.f + 7.5f*i) * (float)M_PI / 180.f;
        press((int)lroundf(233 + 210*cosf(radians)), (int)lroundf(233 + 210*sinf(radians)));
    }
    release(); advance(800);
    check(orb_shell_surface(s_shell) == ORB_SHELL_HOME && lv_obj_is_valid(list) &&
        find_any(list, "Device 01") == first && lv_obj_get_scroll_y(list) > 0,
        "physical rim turn scrolls the retained room without triggering Back");
    for (unsigned turn = 0; turn < 32 && lv_obj_get_scroll_y(list) < maximum; turn++) {
        check(orb_shell_twist(s_shell, 8), "case twist advances toward the final row"); advance(800);
    }
    check(lv_obj_get_scroll_y(list) == maximum && visible_text("Device 24"),
        "shell rotary reaches the final device in a busy room");
    check(!orb_shell_twist(s_shell, 1), "room rotary clamps at the final scroll boundary"); advance(800);
    check(lv_obj_get_scroll_y(list) == maximum && visible_text("Device 24") && !visible_text("Device 01"),
        "turning past the final row never wraps to another accessory");
    check(!orb_home_take_request(&(orb_home_request_t){0}), "shell twist, rim and scrolling drags never actuate Home");
    check(orb_home_snapshot_chunk(original->rooms, original->room_count, original->accessories,
        original->accessory_count, original->scenes, original->scene_count, true, false), "restore Home after shell scrolling");
    free(original); advance(800); orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
}
int main(int argc, char **argv)
{
    lv_init();lv_tick_set_cb(tick);s_display=sim_display_create();check(s_display!=NULL,"display");
    orb_ui_init(NULL);check(orb_model_init()&&orb_home_init(),"models");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "ax3")) check(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK, "AX3");
        else if (!strcmp(argv[i], "bold")) check(orb_fonts_set_bold_text(true) == ORB_FONTS_OK, "Bold Text");
        else if (!strcmp(argv[i], "rt")) orb_ui_set_reduce_transparency(true);
        else if (!strcmp(argv[i], "rm")) orb_ui_set_reduce_motion(true);
    }
    orb_model_load_demo();orb_model_set_connection(true,true);orb_home_load_demo();
    orb_home_snapshot_t *home=calloc(1,sizeof(*home));
    check(home&&orb_home_snapshot(home),"copy Home fixture");
    check(orb_home_snapshot_chunk(home->rooms,home->room_count,home->accessories,home->accessory_count,
        home->scenes,home->scene_count,true,false),"authoritative Home fixture");free(home);
    orb_home_set_connection(true,true);
    s_pointer=lv_indev_create();lv_indev_set_type(s_pointer,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_pointer,s_display);lv_indev_set_read_cb(s_pointer,read_pointer);
    lv_indev_set_mode(s_pointer,LV_INDEV_MODE_EVENT);
    lv_indev_t *physical=s_pointer, *synthetic=lv_indev_create();
    lv_indev_set_type(synthetic,LV_INDEV_TYPE_POINTER);lv_indev_set_display(synthetic,s_display);
    lv_indev_set_read_cb(synthetic,read_pointer);lv_indev_set_mode(synthetic,LV_INDEV_MODE_EVENT);
    orb_input_mark_synthetic(synthetic);
    s_shell=orb_shell_create(lv_screen_active(),NULL,NULL);check(s_shell!=NULL,"shell");
    orb_shell_snapshot_t status={.net=ORB_SHELL_NET_CONNECTED,.link=ORB_SHELL_LINK_ONLINE,
        .battery_pct=80,.rssi=-54,.brightness=70,.usb=true};
    strcpy(status.clock_time,"10:09");strcpy(status.clock_date,"Tuesday, October 27");
    strcpy(status.hub_name,"Mac mini");orb_shell_update(s_shell,&status);advance(1200);
    orb_shell_navigate(s_shell,ORB_SHELL_APPS);advance(1200);
    tap(146,183);
    check(orb_shell_surface(s_shell)==ORB_SHELL_HOME,"Apps Home disc opens Home through pointer input");
    check(find(orb_shell_root(s_shell), "Living Room") &&
        find_any(orb_shell_root(s_shell), "Kitchen") && find(orb_shell_root(s_shell), "Scenes"),
        "Home launches one named room list with discoverable Scenes");
    check(!find(orb_shell_root(s_shell), "Floor Lamp") &&
        !find(orb_shell_root(s_shell), "More Devices") && !find(orb_shell_root(s_shell), ORB_ICON_STAR),
        "Home entry has no satellite favourites or floating star");
    lv_obj_t *landing_row = semantic_control(orb_shell_root(s_shell), "room:Living Room");
    lv_obj_t *next_landing_row = semantic_control(orb_shell_root(s_shell), "room:Bedroom");
    lv_obj_t *landing_list = landing_row ? lv_obj_get_parent(landing_row) : NULL;
    check(landing_list && next_landing_row && lv_obj_is_scrollable(landing_list),
        "landing rows expose their native scroll owner");
    orb_home_request_t request;
    int initial_scroll = lv_obj_get_scroll_y(landing_list);
    check(orb_shell_twist(s_shell, 1), "case twist has the Rooms list owner"); advance(800);
    check(lv_obj_get_scroll_y(landing_list) > initial_scroll &&
        !find_text(orb_shell_root(s_shell), "Floor Lamp"), "case twist scrolls without opening a room");
    int forward_scroll = lv_obj_get_scroll_y(landing_list);
    check(orb_shell_twist(s_shell, -1), "reverse case twist scrolls upward"); advance(800);
    /* The fresh resting position is separate from the rim's row stops. At
     * Largest the first cadence stop is slightly beyond that resting point. */
    int first_stop = lv_obj_get_y(next_landing_row) - lv_obj_get_y(landing_row);
    check(lv_obj_get_scroll_y(landing_list) == first_stop && first_stop < forward_scroll,
        "reverse twist returns to the first room-list stop");
    vertical_drag(225, 340);
    vertical_drag(320, 205);
    check(orb_shell_surface(s_shell) == ORB_SHELL_HOME &&
        !find_text(orb_shell_root(s_shell), "Floor Lamp"),
        "centre vertical list drags never open a layer or room");
    press(233, 260); press(233, 272); press(233, 260); release(); advance(800);
    check(orb_shell_surface(s_shell) == ORB_SHELL_HOME &&
        !find_text(orb_shell_root(s_shell), "Floor Lamp"),
        "real shell row drag returning to its start never opens a room");
    check(!orb_home_take_request(&request), "Rooms centre drags never issue Home writes");
    lv_obj_scroll_to_y(landing_list, 0, LV_ANIM_OFF); advance(100);
    for (unsigned i = 0; i <= 12; i++) {
        float angle = (270.f + 7.5f*i) * (float)M_PI / 180.f;
        press((int)lroundf(233 + 210*cosf(angle)), (int)lroundf(233 + 210*sinf(angle)));
    }
    release(); advance(800);
    check(orb_shell_surface(s_shell) == ORB_SHELL_HOME && lv_obj_get_scroll_y(landing_list) > 0 &&
        !find_text(orb_shell_root(s_shell), "Floor Lamp"),
        "physical rim drag scrolls rooms without opening or actuating a device");
    check(orb_shell_twist(s_shell, -100), "reverse twist clamps the list at its top"); advance(800);
    check(lv_obj_get_scroll_y(landing_list) == 0, "room list remains responsive after its physical rim gesture");
    tap_text("Scenes");
    s_pointer = synthetic; tap_text("Good Night");
    check(!orb_home_take_request(&request), "synthetic room scene tap never runs a scene");
    s_pointer = physical; tap_text("Good Night");
    check(orb_home_take_request(&request) && request.kind == ORB_HOME_REQUEST_SCENE &&
        !strcmp(request.id, "good-night"), "physical scene capsule queues its exact stable ID");
    orb_home_complete(&request, NULL, NULL); advance(800);
    home = calloc(1, sizeof(*home));
    check(home && orb_home_snapshot(home), "copy scene guard fixture");
    for (unsigned i = 0; i < home->scene_count; i++) {
        if (!strcmp(home->scenes[i].id, "good-night")) home->scenes[i].contains_unlock = true;
    }
    check(orb_home_snapshot_chunk(home->rooms, home->room_count, home->accessories,
        home->accessory_count, home->scenes, home->scene_count, true, false), "publish unsafe scene metadata");
    advance(800);
    check(!find(orb_shell_root(s_shell), "Good Night"), "scene containing an unlock is absent from Rooms");
    check(!orb_home_scene("good-night") && !orb_home_take_request(&request),
        "model also refuses the stale unlock scene ID");
    for (unsigned i = 0; i < home->scene_count; i++) home->scenes[i].contains_unlock = false;
    check(orb_home_snapshot_chunk(home->rooms, home->room_count, home->accessories,
        home->accessory_count, home->scenes, home->scene_count, true, false), "restore safe scenes");
    free(home); advance(800); boot_back();
    lv_obj_t *focus = find(orb_shell_root(s_shell), "Living Room");
    unrelated_revision(1); advance(100);
    check(lv_obj_is_valid(focus) && find(orb_shell_root(s_shell), "Living Room") == focus,
        "unrelated agent revision preserves the Rooms subtree");
    check(!find(orb_shell_root(s_shell), ORB_ICON_CHEVRON_LEFT),
        "embedded Home omits redundant Back chrome; BOOT remains available");
    tap_text("Living Room"); scroll_to_text("Front Door"); tap_text("Front Door");
    check(find(orb_shell_root(s_shell), "Hold to unlock") != NULL, "named room device opens lock detail");
    s_pointer=synthetic;press(233,140);advance(1500);release();
    check(lv_indev_active()==NULL,"hold timer finishes outside input dispatch");
    check(!orb_home_take_request(&request),"synthetic hold never unlocks after its timer deadline");
    check(!find(orb_shell_root(s_shell),"Keep holding"),"synthetic lock hold leaves the lock affordance unchanged");
    s_pointer=physical;
    press(233,140);advance(480);
    lv_obj_t *holding=find(orb_shell_root(s_shell),"Keep holding");check(holding!=NULL,"active hold feedback");
    unrelated_revision(2);advance(100);
    check(lv_obj_is_valid(holding)&&find(orb_shell_root(s_shell),"Keep holding")==holding,
        "unrelated model update does not cancel an active lock hold");
    advance(500);release();
    check(orb_home_take_request(&request)&&!strcmp(request.id,"front-door")&&
        request.kind==ORB_HOME_REQUEST_SET&&!request.values.locked,"active hold finishes exactly once");
    orb_home_complete(&request,&request.values,NULL);advance(800);
    boot_back();
    check(orb_shell_surface(s_shell) == ORB_SHELL_HOME && find(orb_shell_root(s_shell), "Living Room") &&
        !find(orb_shell_root(s_shell), "Tap to lock"), "BOOT returns from lock detail to its room");
    boot_back();
    check(orb_shell_surface(s_shell) == ORB_SHELL_HOME && find_any(orb_shell_root(s_shell), "Kitchen") &&
        !find(orb_shell_root(s_shell), "Floor Lamp"), "BOOT returns from room to Rooms");
    boot_back();
    check(orb_shell_surface(s_shell) == ORB_SHELL_FACE, "BOOT leaves Rooms for its canonical Face parent");
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
    unsigned complications=count(orb_shell_root(s_shell),ORB_ICON_HOUSE);
    check(complications==1,"Face has one Home complication");
    for(unsigned i=0;i<200;++i) {orb_shell_environment(s_shell,0,0,1,false);advance(16);}
    check(count(orb_shell_root(s_shell),ORB_ICON_HOUSE)==complications,
        "stable IMU delivery never appends Home complications");
    orb_shell_navigate(s_shell,ORB_SHELL_HOME);advance(1200);tap_text("Living Room");scroll_to_text("Floor Lamp");tap_text("Floor Lamp");
    check(find_text(orb_shell_root(s_shell),"%")!=NULL,"visible disclosure opens the light detail percentage reading");
    s_pointer=synthetic;
    press(36,161);press(128,51);release();advance(800);
    check(!orb_home_take_request(&request),"synthetic Home rim swipe never sends a value");
    s_pointer=physical;
    home=calloc(1,sizeof(*home));
    check(home&&orb_home_snapshot(home),"read brightness before the system Crown turn");
    const orb_home_accessory_t *lamp=orb_home_find(home,"floor-lamp");
    check(lamp&&(lamp->state.present&ORB_HOME_BRIGHTNESS),"selected lamp has a reported brightness");
    float previous_brightness=lamp->state.brightness;
    free(home);
    for(unsigned i=0;i<=7;++i) {
        float radians=(200.f+10.f*i)*(float)M_PI/180.f;
        press((int)lroundf(233+210*cosf(radians)),(int)lroundf(233+210*sinf(radians)));
    }
    check(!orb_home_take_request(&request),"system Crown previews its value until physical lift");
    release();advance(800);
    check(orb_shell_surface(s_shell)==ORB_SHELL_HOME,"left-edge Crown drag never triggers shell Back");
    check(orb_home_take_request(&request)&&request.kind==ORB_HOME_REQUEST_SET&&
        !strcmp(request.id,"floor-lamp")&&request.values.present==ORB_HOME_BRIGHTNESS&&
        request.values.brightness>previous_brightness&&request.values.brightness<=100,
        "physical system Crown lift commits the selected lamp's adjusted brightness");
    orb_home_request_t duplicate;
    check(!orb_home_take_request(&duplicate),"system Crown lift queues exactly one Home write");
    orb_home_complete(&request,&request.values,NULL);advance(800);
    check(!orb_home_take_request(&duplicate),"system Crown release has no continuing fling writes");
    edge_back();
    bool nested_back = orb_shell_surface(s_shell)==ORB_SHELL_HOME &&
        find(orb_shell_root(s_shell),"Floor Lamp") && find(orb_shell_root(s_shell), "Living Room");
    if (!nested_back) fprintf(stderr,"Home nested Back: surface=%d grid=%d detail=%d\n",
        orb_shell_surface(s_shell), find(orb_shell_root(s_shell),"Floor Lamp") != NULL,
        find(orb_shell_root(s_shell), "Living Room") == NULL);
    check(nested_back,"left-rim Back consumes nested Home detail before leaving its place");
    edge_back();
    check(orb_shell_surface(s_shell) == ORB_SHELL_HOME && find_any(orb_shell_root(s_shell), "Kitchen") &&
        !find(orb_shell_root(s_shell), "Floor Lamp"), "left-rim Back returns from the room to Rooms");
    edge_back();
    check(orb_shell_surface(s_shell) == ORB_SHELL_FACE, "left-rim Back leaves Home only from Rooms");
    check(!orb_home_take_request(&request), "Back gestures never queue Home writes");
    busy_room_inputs();
    orb_card_t doorbell = {.id="home-doorbell-test", .kind=ORB_CARD_ATTENTION,
        .title="Front Door", .body="Doorbell rang at 10:09", .priority=ORB_PRIORITY_HIGH,
        .options_count=1, .options={{.id="dismiss", .label="Dismiss", .role=ORB_OPTION_CANCEL}}};
    check(orb_model_upsert_card(&doorbell), "publish canonical doorbell attention card"); advance(800);
    orb_shell_select_card(s_shell, doorbell.id); advance(1000);
    check(find(orb_shell_root(s_shell), "Front Door") && find(orb_shell_root(s_shell), "10:09"),
        "doorbell retains its title and reported local time");
    lv_obj_t *home_source = find(orb_shell_root(s_shell), ORB_ICON_HOUSE);
    check(home_source && !find(orb_shell_root(s_shell), ORB_ICON_CHAT),
        "doorbell uses a Home house source rather than Dot's chat glyph");
    /* The shared Design 2.0 yellow is already brighter than the legacy
     * palette entry. Screen migration owns its contrast remapping. */
    lv_color_t home_yellow = lv_obj_get_style_text_color(home_source, 0);
    check(lv_color_eq(home_yellow, ORB_YELLOW) || lv_color_eq(home_yellow, lv_color_hex(0xFFD600)) ||
        (orb_ui_reduce_transparency() && lv_color_eq(home_yellow, lv_color_hex(0xFEDF43))),
        "doorbell source uses Home yellow");
    lv_obj_t *doorbell_time = find(orb_shell_root(s_shell), "10:09");
    check(doorbell_time && strstr(lv_label_get_text(doorbell_time), "\n10:09"),
        "doorbell local time stays together on a dedicated display line");
    check(find(orb_shell_root(s_shell), "Dismiss") && !find(orb_shell_root(s_shell), "Reply"),
        "doorbell offers explicit Dismiss without voice reply");
    unrelated_revision(3); advance(800);
    check(find(orb_shell_root(s_shell), "Dismiss") != NULL, "doorbell survives an unrelated model rebuild");
    tap(233,399);
    orb_answer_t answer;
    check(orb_model_peek_answer(&answer) && !strcmp(answer.card, doorbell.id) && !strcmp(answer.option, "dismiss"),
        "Dismiss queues the canonical first-answer request");
    doorbell_receipts(&doorbell);
    printf("Home shell: %u checks passed\n",s_checks);
    orb_shell_destroy(s_shell);lv_indev_delete(synthetic);lv_indev_delete(physical);lv_deinit();return 0;
}
