/* Design 2.0 Agents evidence over the production shell and wire decoder.
 * All provider messages, titles, transcripts and input are process-local fakes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "link_v11.h"
#include "orb_fonts.h"
#include "orb_home.h"
#include "orb_input.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_time.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include "orb_kit_controls_motion.h"
#include <math.h>
#include "src/widgets/arclabel/lv_arclabel_private.h"
#include "src/misc/lv_text_private.h"
#include "sim_display.h"

/* Process-local requested-payload counters; firmware owners still need board admission. */
#include "emu_memory.c"

static uint32_t s_now;
static unsigned s_checks;
static unsigned s_max_damage;
static lv_display_t *s_display;
static lv_indev_t *s_pointer, *s_physical, *s_synthetic;
static lv_indev_data_t s_input;
static orb_shell_t *s_shell;
static const char *s_scene = "initialization";
static unsigned s_layout_errors;
static unsigned s_behaviour_errors;
static const char *s_mode = "default";
static uint8_t s_fitted_pixels[SIM_D * SIM_D * 3];

#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "Agents %s at %u ms: %s:%d: %s\n", s_scene, s_now, __FILE__, __LINE__, #value); \
    exit(1); \
} } while (0)

static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev); *data = s_input;
}
static void invalidated(lv_event_t *event)
{
    const lv_area_t *area = lv_event_get_param(event);
    unsigned pixels = (unsigned)lv_area_get_width(area) * lv_area_get_height(area);
    s_max_damage = LV_MAX(s_max_damage, pixels);
}
static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 20) {
        s_now += 20; orb_shell_poll(s_shell); lv_timer_handler(); lv_refr_now(s_display);
    }
}
static void wire(const char *method, const char *json)
{
    cJSON *params = cJSON_Parse(json); CHECK(params);
    CHECK(link_v11_event(method, params) == LINK_V11_APPLIED);
    cJSON_Delete(params);
}
static const char *s_sessions =
    "{\"reset\":true,\"more\":false,\"sessions\":["
    "{\"id\":\"firmware\",\"agent\":\"claude\",\"title\":\"esp32s3-firmware\",\"project\":\"Orb\","
    "\"state\":\"busy\",\"reason\":\"building\",\"summary\":\"Building the navigation fix.\",\"updated\":1793196300,\"can_message\":true},"
    "{\"id\":\"media\",\"agent\":\"codex\",\"title\":\"demo-project\",\"project\":\"Media\","
    "\"state\":\"done\",\"reason\":\"done 9 min ago\",\"summary\":\"638 tests passed\",\"updated\":1793195760,\"can_message\":true},"
    "{\"id\":\"dot\",\"agent\":\"dot\",\"title\":\"Dot\",\"project\":\"\","
    "\"state\":\"idle\",\"reason\":\"Listening for your Orb\",\"summary\":\"\",\"updated\":1793196300,\"can_message\":true},"
    "{\"id\":\"site\",\"agent\":\"claude\",\"title\":\"alex-site\",\"project\":\"Web\","
    "\"state\":\"idle\",\"reason\":\"idle\",\"summary\":\"\",\"updated\":1793192700,\"can_message\":true}]}";
static const char *s_stream =
    "{\"session\":\"firmware\",\"seq\":0,\"at\":1793196300,\"state\":\"busy\",\"stale\":false,\"activity\":\"working 4 min\","
    "\"steps\":[{\"id\":\"read\",\"kind\":\"tool\",\"label\":\"Read orb_workspace.c\",\"state\":\"done\"},"
    "{\"id\":\"edit\",\"kind\":\"tool\",\"label\":\"Edited orb_nav.c\",\"state\":\"done\",\"added\":42,\"removed\":8},"
    "{\"id\":\"build\",\"kind\":\"tool\",\"label\":\"idf.py -j4 build\",\"state\":\"running\"}],"
    "\"assistant\":{\"id\":\"sentence\",\"text\":\"Building to check the nav fix compiles, then I’ll flash it.\",\"final\":true}}";
static const char *s_usage =
    "{\"windows\":["
    "{\"provider\":\"claude\",\"label\":\"5 hours\",\"used_percent\":62,\"resets_at\":1793196840,\"observed_at\":1793194860,\"stale\":false,\"source\":\"usage\"},"
    "{\"provider\":\"claude\",\"label\":\"week\",\"used_percent\":41,\"resets_at\":1793456400,\"observed_at\":1793194860,\"stale\":false,\"source\":\"usage\"},"
    "{\"provider\":\"codex\",\"label\":\"5 hours\",\"used_percent\":38,\"resets_at\":1793206200,\"observed_at\":1793194860,\"stale\":false,\"source\":\"rollout\"},"
    "{\"provider\":\"codex\",\"label\":\"week\",\"used_percent\":71,\"resets_at\":1793456400,\"observed_at\":1793194860,\"stale\":false,\"source\":\"rollout\"}],\"unknown\":[]}";

static lv_obj_t *find(lv_obj_t *root, const char *text)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (lv_obj_check_type(root, &lv_label_class) && strstr(lv_label_get_text(root), text)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *result = find(lv_obj_get_child(root, i), text);
        if (result) return result;
    }
    return NULL;
}
static lv_obj_t *label(const char *text) { return find(orb_shell_root(s_shell), text); }
static lv_obj_t *find_exact(lv_obj_t *root, const char *text)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *result = find_exact(lv_obj_get_child(root, i), text);
        if (result) return result;
    }
    return NULL;
}
static lv_obj_t *exact(const char *text) { return find_exact(orb_shell_root(s_shell), text); }
static lv_obj_t *find_any(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root, &lv_label_class) && strstr(lv_label_get_text(root), text)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *result = find_any(lv_obj_get_child(root, i), text);
        if (result) return result;
    }
    return NULL;
}
static void tap(const char *text)
{
    lv_obj_t *caption = exact(text); if (!caption) caption = label(text);
    if (!caption) caption = find_any(orb_shell_root(s_shell), text);
    CHECK(caption);
    lv_obj_t *control = lv_obj_get_parent(caption);
    while (control && !lv_obj_is_clickable(control)) control = lv_obj_get_parent(control);
    CHECK(control);
    lv_obj_scroll_to_view_recursive(control, LV_ANIM_OFF); advance(40);
    /* Relative-age polling may replace the Notifications tree while scrolling
     * settles. Resolve the current visible target, never a retired row pointer. */
    caption = exact(text); if (!caption) caption = label(text); CHECK(caption);
    control = lv_obj_get_parent(caption);
    while (control && !lv_obj_is_clickable(control)) control = lv_obj_get_parent(control);
    CHECK(control);
    lv_area_t area; lv_obj_get_coords(control, &area);
    lv_area_t presented;
    if (orb_round_list_child_bounds(control, control, &presented)) area = presented;
    s_input.point = (lv_point_t){(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    s_input.state = LV_INDEV_STATE_PRESSED; lv_indev_read(s_pointer); advance(40);
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); advance(80);
}

static void drag(int x1, int y1, int x2, int y2)
{
    s_input.point = (lv_point_t){x1, y1}; s_input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_pointer); advance(40);
    for (unsigned i = 1; i <= 8; i++) {
        s_input.point = (lv_point_t){x1 + (x2 - x1) * (int)i / 8, y1 + (y2 - y1) * (int)i / 8};
        lv_indev_read(s_pointer); advance(20);
    }
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); advance(900);
}

static void fresh(void)
{
    if (s_pointer && s_input.state==LV_INDEV_STATE_PRESSED) {
        s_input.state=LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); advance(40);
    }
    if (s_shell) { orb_shell_destroy(s_shell); s_shell = NULL; }
    orb_model_reset(); link_v11_reset();
    wire("sessions.snapshot", s_sessions); orb_model_set_connection(true, true);
    if (strncmp(s_scene, "SessionLive", 11) && strcmp(s_scene, "first-stream")) wire("agent.stream", s_stream);
    wire("usage.snapshot", s_usage);
    wire("card.show", "{\"id\":\"approval-1\",\"session\":\"firmware\",\"kind\":\"approval\",\"title\":\"Run this command?\","
        "\"body\":\"idf.py -j4 build\",\"detail\":\"in firmware/ · uses the network\",\"priority\":\"normal\",\"reply\":true,"
        "\"options\":[{\"id\":\"deny\",\"label\":\"Deny\",\"role\":\"cancel\"},{\"id\":\"allow\",\"label\":\"Allow\",\"role\":\"primary\"}]}" );
    wire("card.show", "{\"id\":\"approval-2\",\"session\":\"media\",\"kind\":\"choice\",\"title\":\"Which checks?\","
        "\"body\":\"Choose checks\",\"detail\":\"\",\"priority\":\"normal\",\"reply\":true,"
        "\"options\":[{\"id\":\"all\",\"label\":\"All tests\",\"role\":\"default\"},{\"id\":\"cancel\",\"label\":\"Cancel\",\"role\":\"cancel\"}]}" );
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL); CHECK(s_shell);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .rssi = -54, .brightness = 70, .usb = true};
    strcpy(status.clock_time, "10:09"); strcpy(status.hub_name, "Mac mini");
    orb_shell_update(s_shell, &status); advance(900);
}
static void voice(orb_voice_phase_t phase)
{
    CHECK(orb_model_voice_focus("firmware", NULL));
    orb_voice_state_t state = {.phase = phase, .stream = 1, .level = 76};
    strcpy(state.target, "firmware"); strcpy(state.text, "Use four build jobs and run the tests");
    CHECK(orb_model_set_voice(&state));
    orb_shell_navigate(s_shell, ORB_SHELL_VOICE); advance(340);
}
static void boot(bool synthetic)
{
    const orb_button_event_t hold = {.button = ORB_BUTTON_BOOT, .gesture = ORB_BUTTON_HOLD, .synthetic = synthetic};
    const orb_button_event_t up = {.button = ORB_BUTTON_BOOT, .gesture = ORB_BUTTON_UP, .synthetic = synthetic};
    orb_shell_button(s_shell, &hold); orb_shell_button(s_shell, &up);
}
static void receipt(orb_send_status_t status)
{
    CHECK(orb_model_voice_focus("firmware", NULL));
    orb_message_t message = {0};
    strcpy(message.target, "firmware"); strcpy(message.text, "Use four build jobs and run the tests");
    cJSON *response = cJSON_Parse(status == ORB_SEND_QUEUED ?
        "{\"accepted\":true,\"status\":\"queued\",\"reason\":\"Waiting for the session inbox\"}" :
        status == ORB_SEND_FAILED ?
        "{\"code\":\"not_listening\",\"status\":\"failed\",\"msg\":\"Session inbox is not listening\"}" :
        "{\"accepted\":true,\"status\":\"delivered\",\"reason\":\"Delivered to session\"}");
    CHECK(response);
    orb_message_result_t parsed = {0};
    CHECK(orb_link_read_message_receipt(status == ORB_SEND_FAILED ? NULL : response,
        status == ORB_SEND_FAILED ? response : NULL, &parsed));
    CHECK(parsed.status == status);
    cJSON_Delete(response);
    orb_model_complete_message_receipt(&message, parsed.status, parsed.reason, parsed.error);
    orb_voice_state_t state = {.phase = ORB_VOICE_IDLE};
    strcpy(state.target, message.target); CHECK(orb_model_set_voice(&state));
    orb_shell_navigate(s_shell, ORB_SHELL_VOICE); advance(340);
}
static void select_scene(const char *scene)
{
    if (!strcmp(scene, "Agents")) { orb_shell_navigate(s_shell, ORB_SHELL_AGENTS); advance(900); }
    else if (!strcmp(scene, "SessionLive")) {
        wire("sessions.snapshot", s_sessions);
        orb_message_t message = {.target = "firmware", .text = "Fix the crown paging in the Stack"};
        orb_model_complete_message_receipt(&message, ORB_SEND_DELIVERED, "Delivered to session", NULL);
        wire("agent.stream", "{\"session\":\"firmware\",\"seq\":0,\"at\":1793196300,\"state\":\"busy\",\"stale\":false,\"activity\":\"working 4 min\","
            "\"steps\":[],\"assistant\":{\"id\":\"sentence\",\"text\":\"The card’s text area was taking the crown. I’ll bind paging to the Stack instead.\",\"final\":true}}");
        wire("agent.stream", "{\"session\":\"firmware\",\"seq\":1,\"at\":1793196300,\"state\":\"busy\",\"stale\":false,\"activity\":\"working 4 min\","
            "\"steps\":[{\"id\":\"edit\",\"kind\":\"tool\",\"label\":\"Edited orb_workspace_nav.inc\",\"state\":\"done\"},"
            "{\"id\":\"test\",\"kind\":\"tool\",\"label\":\"638 tests passed\",\"state\":\"done\"},"
            "{\"id\":\"build\",\"kind\":\"tool\",\"label\":\"idf.py -j4 build\",\"state\":\"running\"}],"
            "\"assistant\":{\"id\":\"sentence\",\"text\":\"The card’s text area was taking the crown. I’ll bind paging to the Stack instead.\",\"final\":true}}");
        orb_shell_select_session(s_shell, "firmware"); advance(900);
    } else if (!strcmp(scene,"VoiceIdle") || !strcmp(scene,"VoiceTouchCapture")) {
        voice(ORB_VOICE_IDLE); advance(900);
        if (!strcmp(scene,"VoiceTouchCapture")) {
            s_input.point=(lv_point_t){233,380}; s_input.state=LV_INDEV_STATE_PRESSED;
            lv_indev_read(s_pointer); advance(650); lv_indev_read(s_pointer); advance(40);
            orb_voice_request_t request; bool started=false;
            while(orb_model_take_voice_request(&request)) started |= request.action==ORB_VOICE_START;
            CHECK(started);
            orb_voice_state_t capture={.phase=ORB_VOICE_RECORDING,.stream=1,.level=76,.target="firmware"};
            strcpy(capture.text,"Use four build jobs and run the tests"); CHECK(orb_model_set_voice(&capture));
            advance(340); CHECK(label(ORB_ICON_STOP));
            for(unsigned i=0;i<3;++i) {lv_indev_read(s_pointer);advance(40);}
            while(orb_model_take_voice_request(&request)) {
                if(request.action==ORB_VOICE_STOP || request.action==ORB_VOICE_CANCEL) {
                    ++s_behaviour_errors; fprintf(stderr,"BEHAVIOUR %s touch capture ended while finger held\n",s_mode);
                }
            }
        }
    } else if (!strcmp(scene,"VoiceTranscribing")) voice(ORB_VOICE_TRANSCRIBING);
    else if (!strcmp(scene,"VoiceNoSpeech")) {
        orb_voice_state_t state={.phase=ORB_VOICE_ERROR,.target="firmware",.error_code="no_speech",.error="No speech detected"};
        CHECK(orb_model_set_voice(&state)); orb_shell_navigate(s_shell,ORB_SHELL_VOICE);advance(900);
    } else if (!strcmp(scene, "Talk")) { boot(false); voice(ORB_VOICE_RECORDING); }
    else if (!strcmp(scene, "TalkSend")) { boot(false); voice(ORB_VOICE_CONFIRMING); advance(900); }
    else if (!strcmp(scene, "SendQueued")) receipt(ORB_SEND_QUEUED);
    else if (!strcmp(scene, "SendDelivered")) receipt(ORB_SEND_DELIVERED);
    else if (!strcmp(scene, "SendFailed")) receipt(ORB_SEND_FAILED);
    else if (!strcmp(scene, "Usage") || !strcmp(scene, "UsageStale") || !strcmp(scene, "UsageUnknown")) {
        if (!strcmp(scene, "UsageStale")) {
            orb_usage_snapshot_t usage;
            orb_model_snapshot_t *snapshot = calloc(1, sizeof(*snapshot)); CHECK(snapshot);
            CHECK(orb_model_snapshot(snapshot)); usage = snapshot->usage; free(snapshot);
            for (size_t i = 0; i < usage.count; ++i) usage.windows[i].stale = true;
            CHECK(orb_model_set_usage(&usage));
        } else if (!strcmp(scene, "UsageUnknown")) wire("usage.snapshot", "{\"windows\":[],\"unknown\":[\"claude\",\"codex\"]}");
        orb_shell_navigate(s_shell, ORB_SHELL_USAGE); advance(900);
    } else if (!strcmp(scene, "Sessions")) { orb_shell_navigate(s_shell, ORB_SHELL_SESSIONS); advance(900); }
    else if (!strcmp(scene, "Dot")) {
        orb_message_t message = {.target = "dot", .text = "What’s on tomorrow?"};
        orb_model_complete_message_receipt(&message, ORB_SEND_DELIVERED, "Delivered to Dot", NULL);
        wire("agent.stream", "{\"session\":\"dot\",\"seq\":0,\"at\":1793196300,\"state\":\"idle\",\"stale\":false,\"activity\":\"Listening\","
            "\"steps\":[],\"assistant\":{\"id\":\"answer\",\"text\":\"Standup at 9:30, then a free afternoon. Rain after 4, so take the umbrella.\",\"final\":true}}");
        orb_shell_navigate(s_shell, ORB_SHELL_DOT); advance(900);
    } else if (!strcmp(scene, "Approval")) { orb_shell_select_card(s_shell, "approval-1"); advance(900); }
    else if (!strcmp(scene, "Question")) { orb_shell_select_card(s_shell, "approval-2"); advance(900); }
    else if (!strcmp(scene, "FaceWaiting")) { orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(900); }
    else CHECK(false);
    if (!strcmp(scene, "Agents")) {
        CHECK(label("Edited")); CHECK(label("idf.py -j4 build"));
        /* The production glance truncates this long reply at every size.
         * Repeated shortening must leave one terminal U+2026, not a trail. */
        lv_obj_t *quote = label("“Building"); CHECK(quote);
        const char *text = lv_label_get_text(quote), *ellipsis = strstr(text, "…");
        CHECK(ellipsis && !strcmp(ellipsis, "…"));
        lv_point_t measured;
        lv_text_get_size(&measured, text, lv_obj_get_style_text_font(quote, 0),
            lv_obj_get_style_text_letter_space(quote, 0), lv_obj_get_style_text_line_space(quote, 0),
            lv_obj_get_width(quote), LV_TEXT_FLAG_NONE);
        CHECK(measured.x <= lv_obj_get_width(quote) && measured.y <= lv_obj_get_height(quote));
    }
    else if (!strcmp(scene, "SessionLive")) {
        lv_obj_t *question = find_any(orb_shell_root(s_shell), "Fix the crown paging"); CHECK(question);
        lv_obj_t *list = lv_obj_get_parent(lv_obj_get_parent(question));
        lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF); advance(100); CHECK(label("Fix the crown paging"));
        lv_obj_scroll_to_y(list, LV_COORD_MAX, LV_ANIM_OFF); advance(100); CHECK(label("638 tests passed"));
    }
    else if (!strcmp(scene, "Talk")) CHECK(label("Use four build jobs"));
    else if (!strcmp(scene, "TalkSend")) { CHECK(label("Cancel")); CHECK(label("Send")); }
    else if (!strcmp(scene, "SendQueued")) CHECK(label("Sent"));
    else if (!strcmp(scene, "SendDelivered")) CHECK(label("Delivered"));
    else if (!strcmp(scene, "SendFailed")) {
        CHECK(label("Not sent"));
        lv_obj_t *reason = find_any(orb_shell_root(s_shell), "Session inbox is not listening"); CHECK(reason);
        /* The AX3 reader admits whole lines; the complete reason remains
         * reachable below the symbol/title rather than showing a cut line. */
        lv_obj_t *reader = lv_obj_get_parent(reason);
        lv_obj_scroll_to_y(reader, LV_COORD_MAX, LV_ANIM_OFF); advance(100);
        CHECK(label("Session inbox is not listening"));
        lv_obj_scroll_to_y(reader, 0, LV_ANIM_OFF); advance(100);
    }
    else if (!strcmp(scene, "Usage")) {
        CHECK(label("Claude")); CHECK(label("62%")); CHECK(label("41%"));
        CHECK(label("10:14 AM")); CHECK(label("Sat")); CHECK(label("10:20 AM"));
        CHECK(!label("Codex")); CHECK(orb_shell_twist(s_shell, 1)); advance(100);
        CHECK(label("Codex")); CHECK(label("38%")); CHECK(label("71%")); CHECK(label("12:50 PM"));
        CHECK(orb_shell_twist(s_shell, -1)); advance(100);
    }
    else if (!strcmp(scene, "UsageStale")) CHECK(label("Last known"));
    else if (!strcmp(scene, "UsageUnknown")) { CHECK(label("Not available")); CHECK(!label("0%")); }
    else if (!strcmp(scene, "Sessions")) {
        lv_obj_t *first = label("esp32s3-"); CHECK(first);
        lv_obj_t *list = lv_obj_get_parent(lv_obj_get_parent(first));
        /* Padded endpoint lanes let every long name reach the equator. Reveal
         * each real row, rather than assuming all sessions paint at once. */
        lv_obj_t *dot = find_any(list, "Dot"), *site = find_any(list, "alex-site");
        CHECK(dot && site);
        lv_area_t bounds;
        CHECK(orb_round_list_row_bounds(lv_obj_get_parent(first), &bounds));
        /* Sessions rest below the top fade; AX3 title growth keeps the
         * same row top and moves its centre by half the added height. */
        CHECK(abs((bounds.y1+bounds.y2)/2-164)<=2+(lv_obj_get_height(lv_obj_get_parent(first))-ORB_KIT_ROW_H)/2);
        lv_obj_scroll_to_y(list, lv_obj_get_y(lv_obj_get_parent(dot)), LV_ANIM_OFF); advance(100);
        CHECK(label("Dot"));
        lv_obj_scroll_to_y(list, lv_obj_get_y(lv_obj_get_parent(site)), LV_ANIM_OFF); advance(100);
        CHECK(label("alex-site"));
        lv_obj_t *usage = find_any(list, "Usage"); CHECK(usage);
        lv_obj_scroll_to_y(list, LV_COORD_MAX, LV_ANIM_OFF); advance(100);
        CHECK(orb_round_list_row_bounds(lv_obj_get_parent(usage), &bounds));
        CHECK(abs((bounds.y1+bounds.y2)/2-233)<=2+(lv_obj_get_height(lv_obj_get_parent(usage))-ORB_KIT_ROW_H)/2);
        lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF); advance(100);
    }
    else if (!strcmp(scene, "Dot")) { CHECK(label("What’s on tomorrow?")); CHECK(label("Standup at 9:30")); }
    else if (!strcmp(scene, "Approval")) { CHECK(label("Allow")); CHECK(label("Deny")); }
    else if (!strcmp(scene, "FaceWaiting")) {
        /* Pending approvals remain in Agents/Notifications; Faces 2.0 keeps
         * its clock and canonical complications instead of an attention pill. */
        CHECK(label("10:09") && label(ORB_ICON_APPS) && label(ORB_ICON_BATTERY_HIGH));
        CHECK(!label("2 waiting"));
    }
}
static lv_obj_t *find_rim(lv_obj_t *object, const char *needle)
{
    if (!lv_obj_is_visible(object)) return NULL;
    const char *semantic = orb_kit_rim_text_get(object);
    if (semantic && strstr(semantic, needle)) return object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *result = find_rim(lv_obj_get_child(object, i), needle);
        if (result) return result;
    }
    return NULL;
}

typedef struct { lv_obj_t *object; lv_area_t area; } text_box_t;
static text_box_t s_text_boxes[256];
static unsigned s_text_count, s_primary_count;
static lv_obj_t *s_rims[8], *s_controls[128];
static unsigned s_rim_count, s_control_count;
static lv_obj_t *s_arcs[16];
static unsigned s_arc_count;

static bool intersect(lv_area_t *area, const lv_area_t *clip);

static bool descendant(lv_obj_t *child, lv_obj_t *ancestor)
{
    for (lv_obj_t *p = child; p; p = lv_obj_get_parent(p)) if (p == ancestor) return true;
    return false;
}

static bool control_contains(lv_obj_t *control, const lv_area_t *area)
{
    lv_area_t bounds; lv_obj_get_coords(control, &bounds);
    lv_obj_t *row = control; orb_round_list_cache_stats_t cache;
    while (lv_obj_get_parent(row) && !orb_round_list_cache_stats(lv_obj_get_parent(row), &cache)) row=lv_obj_get_parent(row);
    if (lv_obj_get_parent(row) && !orb_round_list_child_bounds(row, control, &bounds)) return false;
    lv_area_t intersection = *area;
    if (!intersect(&intersection, &bounds)) return false;
    for (lv_obj_t *p=lv_obj_get_parent(control);p;p=lv_obj_get_parent(p)) {
        lv_area_t clip; lv_obj_get_coords(p,&clip); if (!intersect(&intersection,&clip)) return false;
    }
    int r = lv_area_get_height(&bounds) / 2;
    for (int y = intersection.y1; y <= intersection.y2; ++y)
        for (int x = intersection.x1; x <= intersection.x2; ++x) {
            int cx = LV_CLAMP(bounds.x1 + r, x, bounds.x2 - r);
            int cy = (bounds.y1 + bounds.y2) / 2;
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) return true;
        }
    return false;
}

static bool intersect(lv_area_t *area, const lv_area_t *clip)
{
    area->x1 = LV_MAX(area->x1, clip->x1); area->y1 = LV_MAX(area->y1, clip->y1);
    area->x2 = LV_MIN(area->x2, clip->x2); area->y2 = LV_MIN(area->y2, clip->y2);
    return area->x1 <= area->x2 && area->y1 <= area->y2;
}

static void layout_error(const char *rule, const char *a, const char *b)
{
    ++s_layout_errors;
    fprintf(stderr, "LAYOUT %s/%s %s: %s%s%s\n", s_mode, s_scene, rule, a, b ? " / " : "", b ? b : "");
}

static uint32_t codepoint(const char **text)
{
    const unsigned char *p = (const unsigned char *)*text;
    uint32_t cp = *p++;
    unsigned count = cp < 0x80 ? 0 : cp < 0xe0 ? 1 : cp < 0xf0 ? 2 : 3;
    if (count) cp &= (1u << (6 - count)) - 1;
    while (count-- && (*p & 0xc0) == 0x80) cp = (cp << 6) | (*p++ & 0x3f);
    *text = (const char *)p; return cp;
}

static void font_ink(lv_obj_t *object, int *top, int *height)
{
    const lv_font_t *font = lv_obj_get_style_text_font(object, 0);
    const char *text = lv_label_get_text(object);
    int first = INT32_MAX, last = INT32_MIN;
    while (*text) {
        uint32_t cp = codepoint(&text); const char *peek = text;
        uint32_t next = *peek ? codepoint(&peek) : 0;
        lv_font_glyph_dsc_t glyph;
        if (!lv_font_get_glyph_dsc(font, &glyph, cp, next) || !glyph.box_h) continue;
        int y = font->line_height - font->base_line - glyph.box_h - glyph.ofs_y;
        first = LV_MIN(first, y); last = LV_MAX(last, y + glyph.box_h - 1);
    }
    *top = first == INT32_MAX ? 0 : first;
    *height = first == INT32_MAX ? 0 : last - first + 1;
}

static bool arc_contains(lv_obj_t *arc, int x, int y)
{
    lv_area_t bounds; lv_obj_get_coords(arc,&bounds);
    float cx=(bounds.x1+bounds.x2+1)/2.f,cy=(bounds.y1+bounds.y2+1)/2.f;
    float dx=x-cx,dy=y-cy,distance=sqrtf(dx*dx+dy*dy);
    for (unsigned p=0;p<2;++p) {
        lv_part_t part=p ? LV_PART_INDICATOR : LV_PART_MAIN;
        if (!lv_obj_get_style_arc_opa(arc,part)) continue;
        int width=lv_obj_get_style_arc_width(arc,part);
        float radius=(lv_area_get_width(&bounds)-width)/2.f;
        if (fabsf(distance-radius)>width/2.f) continue;
        float start=p ? lv_arc_get_angle_start(arc) : lv_arc_get_bg_angle_start(arc);
        float end=p ? lv_arc_get_angle_end(arc) : lv_arc_get_bg_angle_end(arc);
        float span=end-start; if (span<0) span+=360;
        float angle=atan2f(dy,dx)*180.f/(float)M_PI-lv_arc_get_rotation(arc)-start;
        while (angle<0) angle+=360; while (angle>=360) angle-=360;
        if (span>=359.9f || angle<=span) return true;
    }
    return false;
}

static void collect_layout(lv_obj_t *object)
{
    if (!lv_obj_is_visible(object)) return;
    if (orb_kit_is_primary(object)) s_primary_count++;
    if (lv_obj_check_type(object, &lv_arc_class)) { CHECK(s_arc_count<16); s_arcs[s_arc_count++]=object; }
    int h = lv_obj_get_height(object), width = lv_obj_get_width(object);
    bool shape = h == 40 || h == 52 || h == 56 || h == 64 || h == 72 || h == 80;
    if (shape && width >= h && (lv_obj_is_clickable(object) || width == 40) &&
        s_control_count < sizeof(s_controls) / sizeof(s_controls[0])) s_controls[s_control_count++] = object;
    orb_kit_rim_text_info_t rim;
    if (orb_kit_rim_text_info(object, &rim)) {
        CHECK(rim.angle_degrees <= ORB_KIT_TITLE_MAX_DEGREES + 0.1f);
        /* Compare the prepared raster's alpha against actual content below. */
        CHECK(s_rim_count < sizeof(s_rims) / sizeof(s_rims[0])); s_rims[s_rim_count++] = object;
        return;
    }
    if (lv_obj_check_type(object, &lv_label_class) && lv_label_get_text(object)[0]) {
        lv_area_t area; lv_obj_get_coords(object, &area);
        lv_obj_t *row = object;
        orb_round_list_cache_stats_t cache;
        while (lv_obj_get_parent(row) && !orb_round_list_cache_stats(lv_obj_get_parent(row), &cache))
            row = lv_obj_get_parent(row);
        if (lv_obj_get_parent(row)) {
            if (!orb_round_list_child_bounds(row, object, &area)) return;
        }
        lv_area_t own_clip = area;
        lv_point_t measured;
        lv_text_get_size(&measured, lv_label_get_text(object), lv_obj_get_style_text_font(object, 0),
            lv_obj_get_style_text_letter_space(object, 0), lv_obj_get_style_text_line_space(object, 0),
            lv_obj_get_width(object), LV_TEXT_FLAG_NONE);
        if (measured.x < lv_obj_get_width(object)) {
            int fitted = (int)((int64_t)measured.x * lv_area_get_width(&area) / LV_MAX(1, lv_obj_get_width(object)));
            int shift = lv_obj_get_style_text_align(object, 0) == LV_TEXT_ALIGN_CENTER ? (lv_area_get_width(&area) - fitted) / 2 :
                lv_obj_get_style_text_align(object, 0) == LV_TEXT_ALIGN_RIGHT ? lv_area_get_width(&area) - fitted : 0;
            area.x1 += shift; area.x2 = area.x1 + fitted - 1;
        }
        int ink_top, ink_height; font_ink(object, &ink_top, &ink_height);
        int natural_h = lv_obj_get_height(object), presented_h = lv_area_get_height(&area);
        int ink_bottom = measured.y - lv_obj_get_style_text_font(object, 0)->line_height + ink_top + ink_height;
        int origin_y = area.y1;
        area.y1 = origin_y + (int)((int64_t)ink_top * presented_h / LV_MAX(1, natural_h));
        area.y2 = origin_y + (int)((int64_t)ink_bottom * presented_h / LV_MAX(1, natural_h)) - 1;
        if (!intersect(&area, &own_clip)) return;
        for (lv_obj_t *p = lv_obj_get_parent(object); p; p = lv_obj_get_parent(p)) {
            lv_area_t clip; lv_obj_get_coords(p, &clip);
            if (!intersect(&area, &clip)) return;
        }
        if (s_text_count < sizeof(s_text_boxes) / sizeof(s_text_boxes[0]))
            s_text_boxes[s_text_count++] = (text_box_t){object, area};
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        collect_layout(lv_obj_get_child(object, i));
}

/* Check the actual presentation boxes after clipping and row transforms.
 * No semantic source rectangle is compared as if it were the on-panel row. */
static void design_contract(void)
{
    s_text_count = s_primary_count = s_rim_count = s_control_count = s_arc_count = 0;
    collect_layout(orb_shell_root(s_shell));
    CHECK(s_primary_count <= 1);
    if (!strncmp(s_scene, "Approval", 8)) CHECK(s_primary_count == 0);
    /* Match the actual control, so a result such as Saved is not mistaken
     * for Save. Only an enabled Send/Save is tinted (polish #13). */
    lv_obj_t *send = NULL;
    for (unsigned i = 0; i < s_control_count; i++) {
        const char *text = orb_kit_button_get_text(s_controls[i]);
        if (text && (!strcmp(text, "Send") || !strcmp(text, "Save"))) send = s_controls[i];
    }
    if (orb_shell_surface(s_shell) == ORB_SHELL_VOICE && send)
        CHECK(s_primary_count == !lv_obj_has_state(send, LV_STATE_DISABLED));
    for (unsigned i = 0; i < s_text_count; ++i) {
        const text_box_t *a = &s_text_boxes[i];
        for (unsigned j = i + 1; j < s_text_count; ++j) {
            const text_box_t *b = &s_text_boxes[j];
            lv_area_t overlap = a->area;
            if (intersect(&overlap, &b->area))
                layout_error("text-overlap", lv_label_get_text(a->object), lv_label_get_text(b->object));
        }
        for (unsigned c = 0; c < s_control_count; ++c) {
            lv_obj_t *control = s_controls[c];
            if (!descendant(a->object, control) && !descendant(control, a->object) && control_contains(control, &a->area))
                layout_error("text-control-overlap", lv_label_get_text(a->object), NULL);
        }
        for (unsigned r=0;r<s_arc_count;++r) {
            bool overlap=false;
            for(int y=a->area.y1;y<=a->area.y2 && !overlap;++y)
                for(int x=a->area.x1;x<=a->area.x2;++x)
                    if(arc_contains(s_arcs[r],x,y)) { overlap=true;break; }
            if(overlap) layout_error("text-arc-overlap",lv_label_get_text(a->object),NULL);
        }
        if (orb_shell_surface(s_shell) == ORB_SHELL_USAGE) {
            int dx = LV_MAX(abs(a->area.x1 - 233), abs(a->area.x2 - 233));
            int dy = LV_MAX(abs(a->area.y1 - 233), abs(a->area.y2 - 233));
            if (dx * dx + dy * dy >= 191 * 191)
                layout_error("inner-ring-clearance", lv_label_get_text(a->object), NULL);
        }
    }
    for (unsigned r = 0; r < s_rim_count; ++r) {
        lv_obj_t *rim = s_rims[r]; lv_area_t origin; lv_obj_get_coords(rim, &origin);
        lv_draw_buf_t *raster = lv_snapshot_take(rim, LV_COLOR_FORMAT_ARGB8888); CHECK(raster);
        for (unsigned i = 0; i < s_text_count; ++i) {
            lv_area_t bounds = s_text_boxes[i].area;
            bool overlap = false;
            for (int y = LV_MAX(bounds.y1, origin.y1); y <= LV_MIN(bounds.y2, origin.y1 + (int)raster->header.h - 1) && !overlap; ++y)
                for (int x = LV_MAX(bounds.x1, origin.x1); x <= LV_MIN(bounds.x2, origin.x1 + (int)raster->header.w - 1); ++x)
                    if (raster->data[(size_t)(y - origin.y1) * raster->header.stride + (x - origin.x1) * 4 + 3] >= 16) { overlap = true; break; }
            if (overlap) layout_error("rim-text-overlap", orb_kit_rim_text_get(rim), lv_label_get_text(s_text_boxes[i].object));
        }
        for (unsigned c = 0; c < s_control_count; ++c) {
            lv_area_t bounds; lv_obj_get_coords(s_controls[c], &bounds);
            bool overlap = false;
            for (int y = LV_MAX(bounds.y1, origin.y1); y <= LV_MIN(bounds.y2, origin.y1 + (int)raster->header.h - 1) && !overlap; ++y)
                for (int x = LV_MAX(bounds.x1, origin.x1); x <= LV_MIN(bounds.x2, origin.x1 + (int)raster->header.w - 1); ++x) {
                    lv_area_t pixel = {x, y, x, y};
                    if (raster->data[(size_t)(y - origin.y1) * raster->header.stride + (x - origin.x1) * 4 + 3] >= 16 &&
                        control_contains(s_controls[c], &pixel)) { overlap = true; break; }
                }
            if (overlap) layout_error("rim-control-overlap", orb_kit_rim_text_get(rim), NULL);
        }
        for (unsigned a=0;a<s_arc_count;++a) {
            bool overlap=false;
            for (unsigned y=0;y<raster->header.h && !overlap;++y)
                for (unsigned x=0;x<raster->header.w;++x)
                    if(raster->data[(size_t)y*raster->header.stride+x*4+3]>=16 && arc_contains(s_arcs[a],x+origin.x1,y+origin.y1)) { overlap=true;break; }
            if(overlap) layout_error("rim-arc-overlap",orb_kit_rim_text_get(rim),NULL);
        }
        lv_draw_buf_destroy(raster);
    }
}

static void kit_layout_contract(void)
{
    lv_obj_t *root = lv_obj_create(lv_screen_active()); lv_obj_remove_style_all(root);
    lv_obj_set_size(root, 466, 466);
    lv_obj_t *disc = orb_kit_disc_create(root, ORB_KIT_DISC_STANDARD, ORB_KIT_ACCENT_FILL,
                                        ORB_KIT_CLAUDE, ORB_ICON_TERMINAL);
    lv_obj_update_layout(root);
    int width = lv_obj_get_width(disc), height = lv_obj_get_height(disc);
    orb_kit_disc_set_focus(disc, true); lv_obj_update_layout(root);
    CHECK(width == ORB_KIT_DISC && height == ORB_KIT_DISC);
    CHECK(lv_obj_get_width(disc) == width && lv_obj_get_height(disc) == height);
    CHECK(lv_obj_get_style_transform_scale_x(disc, 0) == LV_SCALE_NONE);
    CHECK(lv_obj_get_style_outline_width(disc, 0) == ORB_KIT_FOCUS_WIDTH);
    CHECK(lv_obj_get_style_outline_pad(disc, 0) == ORB_KIT_FOCUS_OFFSET);
    lv_obj_t *title = orb_kit_rim_title_inset_create(root, "ESP32 DEVICE DETECTION", ORB_KIT_CLAUDE);
    orb_kit_rim_text_info_t info; CHECK(orb_kit_rim_text_info(title, &info));
    CHECK(info.angle_degrees <= 120.1f);
    /* Inspect the real arclabel children recursively through the cached slices. */
    lv_obj_t *stack[32] = {title}; unsigned count = 1, arcs = 0;
    while (count) {
        lv_obj_t *object = stack[--count];
        if (lv_obj_check_type(object, &lv_arclabel_class)) {
            CHECK(((lv_arclabel_t *)object)->radius == ORB_KIT_TITLE_INSET_RADIUS); arcs++;
        }
        for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
            CHECK(count < 32); stack[count++] = lv_obj_get_child(object, i);
        }
    }
    CHECK(arcs > 0); lv_obj_delete(root);
}

static void painted_label(const char *text)
{
    lv_obj_t *object=label(text); CHECK(object);
    lv_area_t area;lv_obj_get_coords(object,&area);
    for(lv_obj_t *p=lv_obj_get_parent(object);p;p=lv_obj_get_parent(p)) {
        lv_area_t clip;lv_obj_get_coords(p,&clip);CHECK(intersect(&area,&clip));
    }
    unsigned painted=0;
    for(int y=LV_MAX(0,area.y1);y<=LV_MIN(SIM_D-1,area.y2);++y)
        for(int x=LV_MAX(0,area.x1);x<=LV_MIN(SIM_D-1,area.x2);++x) {
            const uint8_t *pixel=&s_fitted_pixels[((size_t)y*SIM_D+x)*3];
            painted += pixel[0]>45 || pixel[1]>45 || pixel[2]>45;
        }
    CHECK(painted>32);
}

static void voice_semantic_contract(const char *name)
{
    if(strncmp(name,"Voice",5)) return;
    orb_voice_state_t state;CHECK(orb_model_get_voice(&state));
    sim_display_rgb(s_fitted_pixels);
    if(!strncmp(name,"VoiceIdle",9) && state.phase==ORB_VOICE_IDLE) {
        painted_label("Hold BOOT to talk");painted_label(ORB_ICON_MIC);
        if(strstr(name,"Offline")) painted_label("Voice needs your Mac");
    }
    if(!strncmp(name,"VoiceTranscribing",17) && state.phase==ORB_VOICE_TRANSCRIBING) {
        painted_label("Transcribing");painted_label("Cancel");
    }
    if(state.phase==ORB_VOICE_ERROR) {
        painted_label(!strcmp(state.error_code,"no_speech") ? "Didn’t catch that" : "Not sent");
        if(strcmp(state.error_code,"no_speech")) CHECK(!label("Hold BOOT to talk"));
        if(strncmp(name,"VoiceNoSpeech",13) || strcmp(state.error_code,"no_speech")) {
            char prefix[21]; snprintf(prefix, sizeof(prefix), "%.20s", state.error);
            lv_obj_t *reason = find_any(orb_shell_root(s_shell), prefix); CHECK(reason);
            lv_obj_t *reader = lv_obj_get_parent(reason);
            int scroll = lv_obj_get_scroll_y(reader);
            /* Result hints remain complete at AX3 in the shared whole-line
             * reader; verify their actual ink after revealing the hint. */
            lv_obj_scroll_to_y(reader, lv_obj_get_y(reason), LV_ANIM_OFF); advance(100);
            sim_display_rgb(s_fitted_pixels); painted_label(prefix);
            lv_obj_scroll_to_y(reader, scroll, LV_ANIM_OFF); advance(100);
            sim_display_rgb(s_fitted_pixels);
        }
        if(!strncmp(name,"VoiceTouchCapture",17)) {
            CHECK(s_input.state==LV_INDEV_STATE_PRESSED);painted_label(ORB_ICON_STOP);
            for(unsigned i=0;i<3;++i) {lv_indev_read(s_pointer);advance(40);}
            orb_voice_request_t request;
            while(orb_model_take_voice_request(&request)) {
                CHECK(request.action!=ORB_VOICE_CONFIRM);
                CHECK(request.action!=ORB_VOICE_STOP && request.action!=ORB_VOICE_CANCEL);
            }
        }
    }
}

static void shot(const char *directory, const char *name)
{
    voice_semantic_contract(name);
    design_contract();
    lv_mem_monitor_t memory; lv_mem_monitor(&memory);
    orb_glass_build_stats_t shared; orb_glass_build_stats(&shared);
    printf("MEMORY %s retained=%zu shared-glass=%zu private=%zu\n", name, memory.total_size, shared.bytes, memory.total_size - shared.bytes);
    static uint8_t rgb[SIM_D * SIM_D * 3];
    sim_display_rgb(rgb);
    unsigned ink = 0;
    for (size_t i = 0; i < sizeof(rgb); ++i) ink += rgb[i] > 60;
    CHECK(ink > 1000); /* A successful PNG write alone can still save black. */
    char path[1024]; CHECK(snprintf(path, sizeof(path), "%s/%s.png", directory, name) < sizeof(path));
    CHECK(sim_display_save_png(path));
    if (!strncmp(name,"VoiceTouchCapture",17) && s_input.state==LV_INDEV_STATE_PRESSED) {
        orb_voice_state_t state; CHECK(orb_model_get_voice(&state));
        s_input.state=LV_INDEV_STATE_RELEASED;lv_indev_read(s_pointer);advance(100);
        unsigned stops=0;orb_voice_request_t request;
        while(orb_model_take_voice_request(&request)) {
            CHECK(request.action!=ORB_VOICE_CONFIRM); stops += request.action==ORB_VOICE_STOP;
        }
        if(state.phase==ORB_VOICE_ERROR) CHECK(stops<=1);
        if(state.phase==ORB_VOICE_RECORDING && stops!=1) {
            ++s_behaviour_errors;fprintf(stderr,"BEHAVIOUR %s touch release queued %u STOP requests instead of one\n",s_mode,stops);
        }
    }
}
/* The decoder's 64-byte title limit still permits real 60+ character names.
 * Exercise the same title in every shell surface, not a short design caption. */
static const char s_long_title[] = "ESP32 device detection and firmware diagnostics across the house";

static void long_title_fixture(void)
{
    CHECK(strlen(s_long_title) >= 60 && strlen(s_long_title) <= 64);
    cJSON *snapshot = cJSON_Parse(s_sessions); CHECK(snapshot);
    cJSON *session = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(snapshot, "sessions"), 0);
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(session, "title", cJSON_CreateString(s_long_title)));
    CHECK(link_v11_event("sessions.snapshot", snapshot) == LINK_V11_APPLIED);
    cJSON_Delete(snapshot);
    wire("agent.stream", s_stream);
    orb_card_t approval = {.id = "approval-1", .session = "firmware", .kind = ORB_CARD_APPROVAL,
        .body = "idf.py -j4 build", .detail = "in firmware/ · uses the network", .reply = true,
        .options_count = 2, .options = {{.id = "deny", .label = "Deny", .role = ORB_OPTION_CANCEL},
                                     {.id = "allow", .label = "Allow", .role = ORB_OPTION_PRIMARY}}};
    snprintf(approval.title, sizeof(approval.title), "%s", s_long_title);
    CHECK(orb_model_upsert_card(&approval));
    orb_card_t notice = {.id = "long-notice", .session = "firmware", .kind = ORB_CARD_ATTENTION,
        .body = "The device completed its diagnostics.", .reply = true};
    snprintf(notice.title, sizeof(notice.title), "%s", s_long_title);
    CHECK(orb_model_upsert_card(&notice));
    advance(100);
}

static bool verify_fitted_label(lv_obj_t *object)
{
    CHECK(object);
    const char *text = lv_label_get_text(object);
    lv_point_t size;
    lv_text_get_size(&size, text, lv_obj_get_style_text_font(object, 0),
        lv_obj_get_style_text_letter_space(object, 0), lv_obj_get_style_text_line_space(object, 0),
        lv_obj_get_width(object), LV_TEXT_FLAG_NONE);
    if (size.x > lv_obj_get_width(object) || size.y > lv_obj_get_height(object))
        fprintf(stderr, "Label does not fit: %s; measured=%d,%d box=%d,%d\n", text,
            size.x, size.y, lv_obj_get_width(object), lv_obj_get_height(object));
    CHECK(size.x <= lv_obj_get_width(object));
    CHECK(size.y <= lv_obj_get_height(object));
    lv_area_t area; lv_obj_get_coords(object, &area);
    /* Lists present cached rows at their rim scale; raw LVGL coords are
     * the larger source bitmap, not the title seen on the round panel. */
    lv_obj_t *row = object;
    orb_round_list_cache_stats_t cache;
    while (lv_obj_get_parent(row) && !orb_round_list_cache_stats(lv_obj_get_parent(row), &cache))
        row = lv_obj_get_parent(row);
    if (lv_obj_get_parent(row)) {
        lv_area_t presented;
        if (!orb_round_list_child_bounds(row, object, &presented) ||
            !lv_obj_area_is_visible(row, &presented)) return false;
        /* A source label can span several lines beyond the viewport. The
         * cached compositor also clips each visible line to its chord, so
         * inspect emitted pixels rather than its un-clipped semantic box. */
        sim_display_rgb(s_fitted_pixels);
        for (int y = LV_MAX(0, presented.y1); y <= LV_MIN(SIM_D - 1, presented.y2); ++y) {
            int chord = orb_chord_width(y, y, ORB_RIM_MARGIN_TEXT);
            int left = (SIM_D - chord) / 2;
            for (int x = LV_MAX(0, presented.x1); x <= LV_MIN(SIM_D - 1, presented.x2); ++x) {
                if (x >= left && x < left + chord) continue;
                const uint8_t *pixel = &s_fitted_pixels[((size_t)y * SIM_D + x) * 3];
                CHECK(!pixel[0] && !pixel[1] && !pixel[2]);
            }
        }
        return true;
    }
    int chord = orb_chord_width(area.y1, area.y2, ORB_RIM_MARGIN_TEXT);
    int left = (SIM_D - chord) / 2;
    if (area.x1 < left || area.x2 >= left + chord)
        fprintf(stderr, "Label outside chord: %s (%d,%d)..(%d,%d), chord=%d\n",
            text, area.x1, area.y1, area.x2, area.y2, chord);
    CHECK(area.x1 >= left && area.x2 < left + chord);
    return true;
}

static unsigned verify_long_titles(lv_obj_t *object)
{
    if (!lv_obj_is_visible(object)) return 0;
    unsigned found = 0;
    const char *semantic = orb_kit_rim_text_get(object);
    if (semantic && strstr(semantic, "ESP32")) {
        orb_kit_rim_text_info_t info; CHECK(orb_kit_rim_text_info(object, &info));
        CHECK(info.angle_degrees <= 120.1f); CHECK(info.ellipsised); found++;
    }
    if (lv_obj_check_type(object, &lv_label_class) && strstr(lv_label_get_text(object), "ESP32")) {
        const char *text = lv_label_get_text(object);
        if (strcmp(text, s_long_title)) CHECK(strstr(text, "…") || strstr(text, "..."));
        if (verify_fitted_label(object)) found++;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        found += verify_long_titles(lv_obj_get_child(object, i));
    return found;
}

static void long_activity_scene(const char *directory)
{
    s_scene = "first-stream"; fresh(); /* Without a stream, show the real session reason. */
    orb_session_t update = {.id = "firmware", .agent = ORB_AGENT_CLAUDE,
        .title = "esp32s3-firmware", .project = "Orb", .state = ORB_SESSION_BUSY,
        .reason = "Approve Bash: idf.py build with 4 jobs and all firmware checks",
        .updated = 1793196301, .can_message = true};
    CHECK(orb_model_upsert_session(&update));
    orb_shell_navigate(s_shell, ORB_SHELL_AGENTS); advance(2300);
    lv_obj_t *title = find_rim(orb_shell_root(s_shell), "ESP32S3-FIRMWARE"); CHECK(title);
    CHECK(!label("Claude"));
    shot(directory, "AgentsLongActivity");
    orb_model_set_connection(false, false); advance(100);
    CHECK(find_rim(orb_shell_root(s_shell), "ESP32S3-FIRMWARE") == title);
    CHECK(label("offline"));
    shot(directory, "AgentsLongActivityStale");
}

static void long_title_scenes(const char *directory)
{
    static const char *names[] = {"Agents", "SessionLive", "Sessions", "Approval"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        s_scene = names[i]; fresh(); long_title_fixture();
        if (!strcmp(s_scene, "Agents")) {
            orb_shell_navigate(s_shell, ORB_SHELL_AGENTS); advance(900);
            /* A retained model refresh must keep both the title object and
             * its ellipsis, not switch the full text back to clipping. */
            lv_obj_t *retained = find_rim(orb_shell_root(s_shell), "ESP32"); CHECK(retained);
            orb_session_t update = {.id = "firmware", .agent = ORB_AGENT_CLAUDE,
                .project = "Diagnostics", .state = ORB_SESSION_BUSY, .reason = "building",
                .updated = 1793196301, .can_message = true};
            snprintf(update.title, sizeof(update.title), "%s", s_long_title);
            CHECK(orb_model_upsert_session(&update)); advance(100);
            CHECK(find_rim(orb_shell_root(s_shell), "ESP32") == retained);
        } else if (!strcmp(s_scene, "SessionLive")) orb_shell_select_session(s_shell, "firmware");
        else if (!strcmp(s_scene, "Sessions")) orb_shell_navigate(s_shell, ORB_SHELL_SESSIONS);
        else if (!strcmp(s_scene, "Approval")) orb_shell_select_card(s_shell, "approval-1");
        else {
            orb_shell_navigate(s_shell, ORB_SHELL_NOTIFICATIONS); advance(900);
            if (!strcmp(s_scene, "NotificationDetail")) tap("ESP32");
        }
        advance(2300); /* Capture after the incoming-card toast has cleared. */
        char name[80]; snprintf(name, sizeof(name), "%sLongTitle", s_scene);
        shot(directory, name);
        CHECK(verify_long_titles(orb_shell_root(s_shell)) > 0);
    }
}

static bool disabled(lv_obj_t *caption)
{
    CHECK(caption);
    lv_obj_t *control = lv_obj_get_parent(caption);
    while (control && !lv_obj_is_clickable(control)) control = lv_obj_get_parent(control);
    CHECK(control);
    return lv_obj_has_state(control, LV_STATE_DISABLED);
}

static void polish_scenes(const char *directory)
{
    s_scene = "UsageDetails"; fresh(); select_scene("Usage");
    CHECK(orb_shell_twist(s_shell, 1)); advance(100);
    CHECK(label("Week 71%"));
    shot(directory, s_scene);

    s_scene = "UsageLiveBoundary"; fresh();
    orb_model_snapshot_t *model = calloc(1, sizeof(*model)); CHECK(model);
    CHECK(orb_model_snapshot(model));
    for (unsigned i = 0; i < model->usage.count; i++)
        if (model->usage.windows[i].provider == ORB_AGENT_CLAUDE) model->usage.windows[i].used_percent = 9;
    CHECK(orb_model_set_usage(&model->usage));
    orb_shell_navigate(s_shell, ORB_SHELL_USAGE); advance(900);
    lv_obj_t *live[] = {label("Claude"), label("9%"), label("resets"), label("Week 9%")};
    lv_area_t original[4];
    for (unsigned i = 0; i < 4; i++) { CHECK(live[i]); lv_obj_get_coords(live[i], &original[i]); }
    orb_kit_value_info_t before, after; CHECK(orb_kit_value_info(live[1], &before));
    lv_obj_raster_stats_t raster_before, raster_after; lv_obj_raster_get_stats(&raster_before);
    for (unsigned i = 0; i < model->usage.count; i++)
        if (model->usage.windows[i].provider == ORB_AGENT_CLAUDE) model->usage.windows[i].used_percent = 100;
    CHECK(orb_model_set_usage(&model->usage)); advance(100);
    /* Alive Follow presents an intermediate count at 100ms, while the
     * semantic target is already authoritative. Retained labels stay put. */
    const char *target;
    CHECK(orb_kit_ring_semantic_text(live[1], &target) && !strcmp(target, "100%"));
    CHECK(orb_kit_ring_semantic_text(live[3], &target) && !strcmp(target, "Week 100%"));
    for (unsigned i = 0; i < 4; i++) {
        lv_area_t current; lv_obj_get_coords(live[i], &current);
        CHECK(current.x1 == original[i].x1 && current.y1 == original[i].y1 &&
              current.x2 == original[i].x2 && current.y2 == original[i].y2);
    }
    CHECK(orb_kit_value_info(live[1], &after));
    CHECK(after.tabular && before.width == after.width && before.template_measures == after.template_measures);
    lv_obj_raster_get_stats(&raster_after); CHECK(raster_before.builds == raster_after.builds);
    advance(450);
    CHECK(label("100%") == live[1] && label("Week 100%") == live[3]);
    free(model); shot(directory, s_scene);

    s_scene = "UsageModelOnly"; fresh();
    wire("usage.snapshot", "{\"windows\":[{\"provider\":\"claude\",\"label\":\"Sonnet\",\"used_percent\":62,"
        "\"resets_at\":1793456400,\"observed_at\":1793194860,\"stale\":false,\"source\":\"usage\"}],\"unknown\":[]}");
    orb_shell_navigate(s_shell, ORB_SHELL_USAGE); advance(900);
    CHECK(label("62%")); CHECK(!label("Not available")); CHECK(label("Sonnet")); shot(directory, s_scene);
    CHECK(!label("Usage · Sonnet"));

    s_scene = "DotDisconnected"; fresh(); select_scene("Dot");
    wire("session.update", "{\"session\":{\"id\":\"dot\",\"agent\":\"dot\",\"title\":\"Dot\",\"project\":\"\",\"state\":\"idle\","
         "\"reason\":\"Dot not connected\",\"summary\":\"\",\"updated\":1793196301,\"can_message\":false}}");
    advance(100); CHECK(label("Dot isn’t connected")); CHECK(disabled(label(ORB_ICON_MIC)));
    CHECK(find_any(orb_shell_root(s_shell), "Standup at 9:30"));
    shot(directory, s_scene);

    s_scene = "DotAbsent"; fresh(); CHECK(orb_model_remove_session("dot"));
    orb_shell_navigate(s_shell, ORB_SHELL_DOT); advance(900);
    orb_idle_request_t idle_request;
    CHECK(orb_model_take_idle_request(&idle_request));
    CHECK(orb_model_complete_idle(&idle_request, NULL, 0, NULL, NULL)); advance(100);
    CHECK(label("Dot isn’t connected")); CHECK(!label(ORB_ICON_MIC)); CHECK(!label("or hold BOOT"));
    shot(directory, s_scene);

    s_scene = "DotIdleDiscovery"; fresh(); CHECK(orb_model_remove_session("dot"));
    orb_shell_navigate(s_shell, ORB_SHELL_DOT); advance(900); CHECK(label("Loading…"));
    CHECK(orb_model_take_idle_request(&idle_request));
    orb_session_t dot = {.id = "dot", .agent = ORB_AGENT_DOT, .title = "Dot", .state = ORB_SESSION_IDLE,
        .reason = "Ready for your message", .summary = "Take the umbrella after 4.", .can_message = true};
    CHECK(orb_model_complete_idle(&idle_request, &dot, 1, NULL, NULL)); advance(100);
    CHECK(label("Take the umbrella after 4.")); CHECK(!disabled(label(ORB_ICON_MIC)));
    shot(directory, s_scene);

    s_scene = "TranscriptionError"; fresh();
    orb_voice_state_t failed = {.phase = ORB_VOICE_ERROR, .stream = 1, .target = "firmware",
        .error = "Transcription unavailable. Try again on your Mac.", .error_code = "internal"};
    CHECK(orb_model_set_voice(&failed)); orb_shell_navigate(s_shell, ORB_SHELL_VOICE); advance(900);
    CHECK(label("Not sent")); CHECK(find_any(orb_shell_root(s_shell), "Transcription unavailable"));
    shot(directory, s_scene);

    s_scene = "SessionEndedSend"; fresh(); boot(false); voice(ORB_VOICE_CONFIRMING);
    CHECK(orb_model_remove_session("firmware")); advance(100);
    CHECK(disabled(exact("Send"))); advance(2400);
    orb_voice_request_t request;
    while (orb_model_take_voice_request(&request)) CHECK(request.action != ORB_VOICE_CONFIRM);
    CHECK(find_rim(orb_shell_root(s_shell), "SESSION ENDED")); shot(directory, s_scene);

    s_scene = "SessionsNeedsYou"; fresh();
    wire("session.update", "{\"session\":{\"id\":\"media\",\"agent\":\"codex\",\"title\":\"Needs your permission\","
         "\"project\":\"A very long project name\",\"state\":\"waiting\",\"reason\":\"Permission requested\","
         "\"summary\":\"\",\"updated\":1793196301,\"can_message\":true}}");
    orb_shell_navigate(s_shell, ORB_SHELL_SESSIONS); advance(900);
    lv_obj_t *waiting = label("Needs your"); CHECK(waiting);
    lv_obj_t *list = lv_obj_get_parent(lv_obj_get_parent(waiting));
    lv_obj_t *working = find_any(list, "esp32s3-"); CHECK(working);
    CHECK(lv_obj_get_y(lv_obj_get_parent(waiting)) < lv_obj_get_y(lv_obj_get_parent(working)));
    shot(directory, s_scene);

    s_scene = "SessionsIdleOnly"; fresh();
    CHECK(orb_model_remove_session("firmware")); CHECK(orb_model_remove_session("media")); CHECK(orb_model_remove_session("dot"));
    orb_shell_navigate(s_shell, ORB_SHELL_SESSIONS); advance(900);
    CHECK(label("alex-site")); CHECK(lv_obj_get_y(lv_obj_get_parent(label("alex-site"))) >= 0);
    shot(directory, s_scene);

    s_scene = "AgentsMany"; fresh();
    for (unsigned i = 4; i < ORB_MODEL_MAX_SESSIONS; i++) {
        orb_session_t session = {.agent = ORB_AGENT_CODEX, .state = ORB_SESSION_BUSY, .updated = 1793196300};
        snprintf(session.id, sizeof(session.id), "session-%u", i);
        snprintf(session.title, sizeof(session.title), "Parallel task %u", i);
        CHECK(orb_model_upsert_session(&session));
    }
    orb_shell_navigate(s_shell, ORB_SHELL_AGENTS); advance(900);
    CHECK(find_rim(orb_shell_root(s_shell), "ESP32S3-FIRMWARE"));
    /* Legacy snapshots can contain 24 sessions. Rotating the focus must never
     * index the eight-angle rim layout with an unbounded session index. */
    for (unsigned i = 0; i < 25; i++) {
        orb_shell_twist(s_shell, 1); advance(40);
    }
    shot(directory, s_scene);
}

static const char *s_long_prose = "The firmware diagnostics are complete across the house. Please check the bedroom controller and the downstairs kitchen panel before starting another build, and keep the existing approval safeguards enabled.";

static void time_voice(orb_voice_phase_t phase, orb_send_status_t status)
{
    CHECK(orb_model_voice_focus("time", NULL));
    if (phase == ORB_VOICE_IDLE) {
        orb_message_t message = {.target = "time", .text = "Remind me to check the oven tomorrow at 6 PM"};
        orb_model_complete_message_receipt(&message, status,
            status == ORB_SEND_FAILED ? "The reminder service is unavailable. Try again on your Mac." : "Reminder added", NULL);
    }
    orb_voice_state_t state = {.phase = phase, .stream = 2, .target = "time"};
    strcpy(state.text, "Remind me to check the oven tomorrow at 6 PM");
    CHECK(orb_model_set_voice(&state)); orb_shell_navigate(s_shell, ORB_SHELL_VOICE); advance(900);
}

static void wire_next_stream(const char *json)
{
    cJSON *stream = cJSON_Parse(json); CHECK(stream);
    orb_agent_stream_t previous; CHECK(orb_model_get_stream("firmware", &previous));
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(stream, "seq", cJSON_CreateNumber(previous.seq + 1)));
    CHECK(link_v11_event("agent.stream", stream) == LINK_V11_APPLIED); cJSON_Delete(stream);
}

static void extra_scenes(const char *directory)
{
    static const char *bases[] = {"Agents", "SessionLive", "Sessions", "Usage", "Dot", "Approval", "Question",
        "Talk", "TalkSend", "TimeSave", "SendQueued", "SendDelivered", "SendFailed",
        "VoiceIdle", "VoiceTranscribing", "VoiceNoSpeech", "VoiceTouchCapture"};
    static const char *states[] = {"Long", "Empty", "Offline", "Error"};
    for (unsigned b = 0; b < sizeof(bases) / sizeof(bases[0]); ++b) {
        for (unsigned v = 0; v < sizeof(states) / sizeof(states[0]); ++v) {
            char name[96]; snprintf(name, sizeof(name), "%s%s", bases[b], states[v]);
            s_scene = name; fresh();
            if (v==0 && !strcmp(bases[b],"VoiceTouchCapture")) {
                orb_session_t recipient={.id="firmware",.agent=ORB_AGENT_CLAUDE,.state=ORB_SESSION_BUSY,
                    .updated=1793196301,.can_message=true};
                snprintf(recipient.title,sizeof(recipient.title),"%s",s_long_title); CHECK(orb_model_upsert_session(&recipient));
            }
            if (!strcmp(bases[b], "TimeSave")) time_voice(ORB_VOICE_CONFIRMING, ORB_SEND_UNKNOWN);
            else select_scene(bases[b]);
            if (v == 0) {
                orb_session_t update = {.id = "firmware", .agent = ORB_AGENT_CLAUDE, .state = ORB_SESSION_BUSY,
                    .can_message = true, .updated=1793196301, .reason = "Checking the bedroom controller before the next firmware release"};
                snprintf(update.title, sizeof(update.title), "%s", s_long_title); CHECK(orb_model_upsert_session(&update));
                cJSON *stream = cJSON_Parse(s_stream); CHECK(stream);
                orb_agent_stream_t previous; CHECK(orb_model_get_stream("firmware", &previous));
                CHECK(cJSON_ReplaceItemInObjectCaseSensitive(stream, "seq", cJSON_CreateNumber(previous.seq + 1)));
                cJSON *assistant = cJSON_GetObjectItemCaseSensitive(stream, "assistant");
                char preview[161]; snprintf(preview, sizeof(preview), "%s", s_long_prose);
                CHECK(cJSON_ReplaceItemInObjectCaseSensitive(assistant, "text", cJSON_CreateString(preview)));
                CHECK(link_v11_event("agent.stream", stream) == LINK_V11_APPLIED); cJSON_Delete(stream);
                if (!strcmp(bases[b], "Dot")) {
                    orb_agent_stream_t dot; CHECK(orb_model_get_stream("dot", &dot)); dot.seq++;
                    snprintf(dot.assistant.text, sizeof(dot.assistant.text), "%s", s_long_prose);
                    CHECK(orb_model_apply_stream(&dot));
                    orb_message_t message = {.target="dot"}; snprintf(message.text,sizeof(message.text),"%s",s_long_prose);
                    orb_model_complete_message_receipt(&message,ORB_SEND_DELIVERED,"Delivered to Dot",NULL);
                }
                if (!strcmp(bases[b], "Approval") || !strcmp(bases[b], "Question")) {
                    orb_card_t card = {.id = "approval-long", .session = "firmware", .kind = !strcmp(bases[b], "Approval") ? ORB_CARD_APPROVAL : ORB_CARD_CHOICE,
                        .reply = true, .options_count = 2,
                        .options = {{.id="deny", .label="Deny", .role=ORB_OPTION_CANCEL}, {.id="allow", .label="Allow", .role=ORB_OPTION_PRIMARY}}};
                    snprintf(card.title, sizeof(card.title), "%s", s_long_title);
                    snprintf(card.body, sizeof(card.body), "%s", s_long_prose);
                    snprintf(card.detail, sizeof(card.detail), "The Mac is asking for an explicit decision before continuing.");
                    CHECK(orb_model_upsert_card(&card)); orb_shell_select_card(s_shell, card.id);
                }
                if (orb_shell_surface(s_shell) == ORB_SHELL_VOICE) {
                    orb_voice_state_t state; CHECK(orb_model_get_voice(&state));
                    snprintf(state.text, sizeof(state.text), "%s", s_long_prose); CHECK(orb_model_set_voice(&state));
                }
            } else if (v == 1) {
                if (!strcmp(bases[b], "Agents") || !strcmp(bases[b], "Sessions") || !strcmp(bases[b], "SessionLive") || !strcmp(bases[b], "Dot")) {
                    orb_shell_surface_t surface = orb_shell_surface(s_shell);
                    orb_model_reset(); orb_model_set_connection(true, true); orb_shell_navigate(s_shell, surface);
                } else if (!strcmp(bases[b], "Approval") || !strcmp(bases[b], "Question")) {
                    CHECK(orb_model_cancel_card("approval-1")); CHECK(orb_model_cancel_card("approval-2"));
                } else if (!strcmp(bases[b], "Usage")) wire("usage.snapshot", "{\"windows\":[],\"unknown\":[\"claude\",\"codex\"]}");
                else if (orb_shell_surface(s_shell) == ORB_SHELL_VOICE) {
                    orb_voice_state_t state; CHECK(orb_model_get_voice(&state)); state.text[0] = '\0'; CHECK(orb_model_set_voice(&state));
                }
            } else if (v == 2) orb_model_set_connection(false, false);
            else {
                if (!strcmp(bases[b], "Agents") || !strcmp(bases[b], "SessionLive"))
                    wire_next_stream("{\"session\":\"firmware\",\"seq\":1,\"at\":1793196301,\"state\":\"busy\",\"stale\":true,\"activity\":\"Build failed\",\"steps\":[{\"id\":\"failed\",\"kind\":\"tool\",\"label\":\"Firmware diagnostics failed\",\"state\":\"failed\"}],\"assistant\":{\"id\":\"failure\",\"text\":\"The build failed. Check the Mac for the full compiler diagnostics.\",\"final\":true}}");
                else if (!strcmp(bases[b], "Usage")) wire("usage.snapshot", "{\"windows\":[],\"unknown\":[\"claude\",\"codex\"]}");
                else if (orb_shell_surface(s_shell) == ORB_SHELL_VOICE) {
                    orb_voice_state_t state = {.phase=ORB_VOICE_ERROR, .target="firmware", .error_code="internal",
                        .error="The transcription service is unavailable. Try again on your Mac."};
                    if (!strcmp(bases[b], "TimeSave")) strcpy(state.target, "time");
                    snprintf(state.text, sizeof(state.text), "%s", s_long_prose); CHECK(orb_model_set_voice(&state));
                } else orb_model_set_connection(false, false);
            }
            advance(2500); shot(directory, name);
        }
    }
    static const char *voice_states[]={"VoiceIdle","VoiceTranscribing","VoiceNoSpeech","VoiceTouchCapture"};
    for(unsigned i=0;i<4;++i) {s_scene=voice_states[i];fresh();select_scene(s_scene);shot(directory,s_scene);}
    s_scene="Question"; fresh(); select_scene(s_scene); shot(directory, s_scene);
    s_scene="TimeSave"; fresh(); time_voice(ORB_VOICE_CONFIRMING, ORB_SEND_UNKNOWN); shot(directory, s_scene);
    advance(6500);
    orb_voice_request_t request;
    while (orb_model_take_voice_request(&request)) CHECK(request.action != ORB_VOICE_CONFIRM);
    CHECK(label("Save"));
    s_scene="TimeSaveDelivered"; fresh(); time_voice(ORB_VOICE_IDLE, ORB_SEND_DELIVERED); shot(directory,s_scene);
    s_scene="TimeSaveFailed"; fresh(); time_voice(ORB_VOICE_IDLE, ORB_SEND_FAILED); shot(directory,s_scene);
    static const char *grants[] = {"ApprovalOperation", "ApprovalTurn", "QuestionFour"};
    for (unsigned g = 0; g < 3; ++g) {
        s_scene = grants[g]; fresh();
        orb_card_t card = {.id="grant-review", .session="firmware", .reply=true, .options_count=4,
            .kind=g==2 ? ORB_CARD_CHOICE : ORB_CARD_APPROVAL, .scope=g==2 ? ORB_APPROVAL_UNSPECIFIED : g ? ORB_APPROVAL_TURN : ORB_APPROVAL_OPERATION,
            .title="Review requested operation", .body="Run the firmware diagnostics using four build workers.",
            .detail="The decision applies only to the stated operation or current turn.",
            .options={{.id="deny",.label="Deny",.role=ORB_OPTION_CANCEL},
                {.id="allow",.label="Allow once",.role=ORB_OPTION_PRIMARY},
                {.id="inspect",.label="Inspect command",.role=ORB_OPTION_DEFAULT},
                {.id="later",.label="Decide later",.role=ORB_OPTION_DEFAULT}}};
        if (g==1) strcpy(card.options[1].label,"Allow for turn");
        CHECK(orb_model_upsert_card(&card)); orb_shell_select_card(s_shell,card.id); advance(2200); shot(directory,s_scene);
    }
    static const char *answers[] = {"QuestionQueued", "QuestionDelivered", "QuestionRecorded",
        "QuestionOtherAnswer", "QuestionRejected"};
    for (unsigned i=0;i<5;++i) {
        s_scene=answers[i]; fresh(); select_scene("Question");
        lv_obj_t *option=find_any(orb_shell_root(s_shell),"All tests"); CHECK(option);
        lv_obj_t *choices=lv_obj_get_parent(lv_obj_get_parent(option));
        lv_obj_t *viewport=lv_obj_get_parent(choices);
        orb_round_list_cache_stats_t before, after;
        CHECK(orb_round_list_cache_stats(choices,&before));
        size_t allocations=sim_memory_allocation_calls();
        lv_obj_scroll_to_y(viewport,24,LV_ANIM_OFF);
        CHECK(orb_round_list_cache_stats(choices,&after));
        CHECK(before.source_builds==after.source_builds);
        CHECK(sim_memory_allocation_calls()==allocations);
        advance(100);
        tap("All tests");
        orb_answer_t answer; CHECK(orb_model_peek_answer(&answer));
        CHECK(!strcmp(answer.card,"approval-2") && !strcmp(answer.option,"all"));
        CHECK(orb_model_pending_answers()==1);
        CHECK(orb_model_cancel_card(answer.card));
        if(i) {
            orb_answer_t recorded=answer;
            if(i==3) strcpy(recorded.option,"cancel");
            CHECK(orb_model_complete_answer(answer.card,i==4 ? NULL : &recorded,
                i!=2,i==4 ? "expired" : NULL));
        }
        advance(900);
        CHECK(label(i==0 ? "Sending…" : i==4 ? "Not sent" : i==2 ? "Sent" : "Delivered"));
        CHECK(orb_model_pending_answers()==(i==0 ? 1u : 0u));
        CHECK(label("Done")); /* Explicit dismissal remains available after card retirement. */
        shot(directory,s_scene);
    }
    s_scene="UsageWeeklyOnly"; fresh();
    wire("usage.snapshot", "{\"windows\":[{\"provider\":\"codex\",\"label\":\"week\",\"used_percent\":71,\"resets_at\":1793456400,\"observed_at\":1793194860,\"stale\":false,\"source\":\"rollout\"}],\"unknown\":[]}");
    orb_shell_navigate(s_shell, ORB_SHELL_USAGE); advance(900); CHECK(orb_shell_twist(s_shell,1)); advance(400);
    CHECK(label("71%")); CHECK(label("Week")); shot(directory,s_scene);
}

static void behaviour(void)
{
    s_scene = "stable-waiting-order"; fresh();
    orb_session_t first_waiting = {.id = "first-waiting", .agent = ORB_AGENT_CLAUDE,
        .title = "First waiting prompt", .state = ORB_SESSION_WAITING, .updated = 1};
    orb_session_t second_waiting = {.id = "second-waiting", .agent = ORB_AGENT_CODEX,
        .title = "Second waiting prompt", .state = ORB_SESSION_WAITING, .updated = 100};
    CHECK(orb_model_upsert_session(&first_waiting)); CHECK(orb_model_upsert_session(&second_waiting));
    orb_shell_navigate(s_shell, ORB_SHELL_SESSIONS); advance(900);
    lv_obj_t *first_prompt = find_any(orb_shell_root(s_shell), "First waiting"); CHECK(first_prompt);
    lv_obj_t *second_prompt = find_any(orb_shell_root(s_shell), "Second waiting"); CHECK(second_prompt);
    int first_y = lv_obj_get_y(lv_obj_get_parent(first_prompt));
    int second_y = lv_obj_get_y(lv_obj_get_parent(second_prompt));
    CHECK(first_y < second_y);
    second_waiting.updated = 1000;
    strcpy(second_waiting.title, "Second prompt renamed");
    strcpy(second_waiting.summary, "New metadata must preserve the prompt's queue position.");
    CHECK(orb_model_upsert_session(&second_waiting)); advance(100);
    CHECK(find_any(orb_shell_root(s_shell), "First waiting") == first_prompt);
    CHECK(find_any(orb_shell_root(s_shell), "Second prompt") == second_prompt);
    CHECK(lv_obj_get_y(lv_obj_get_parent(first_prompt)) == first_y);
    CHECK(lv_obj_get_y(lv_obj_get_parent(second_prompt)) == second_y);

    s_scene = "chrome-free-navigation"; fresh(); select_scene("Agents");
    CHECK(!label(ORB_ICON_CHEVRON_LEFT) && !label(ORB_ICON_CHEVRON_UP) && !label(ORB_ICON_CHEVRON_DOWN));
    CHECK(label("+42") && label("−8"));
    drag(320, 100, 140, 100);
    if (orb_shell_surface(s_shell) != ORB_SHELL_AGENTS) {
        ++s_behaviour_errors; fprintf(stderr, "BEHAVIOUR %s centre drag navigated Agents to Sessions\n", s_mode);
    }
    tap(ORB_ICON_LIST); advance(900);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SESSIONS);
    tap("Usage"); advance(900); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_USAGE);
    orb_shell_button(s_shell, &(orb_button_event_t){.button = ORB_BUTTON_BOOT, .gesture = ORB_BUTTON_SHORT, .synthetic = true});
    advance(900); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SESSIONS);
    orb_shell_button(s_shell, &(orb_button_event_t){.button = ORB_BUTTON_BOOT, .gesture = ORB_BUTTON_SHORT, .synthetic = true});
    advance(900); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_AGENTS);
    drag(140, 100, 320, 100);
    if (orb_shell_surface(s_shell) != ORB_SHELL_AGENTS) {
        ++s_behaviour_errors; fprintf(stderr, "BEHAVIOUR %s centre drag navigated Agents to Usage\n", s_mode);
    }
    orb_shell_navigate(s_shell, ORB_SHELL_USAGE); advance(900);
    orb_shell_button(s_shell, &(orb_button_event_t){.button = ORB_BUTTON_BOOT, .gesture = ORB_BUTTON_SHORT, .synthetic = true});
    advance(900); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_AGENTS);
    CHECK(orb_model_pending_answers() == 0);
    s_scene = "retained-stream"; fresh(); select_scene("Agents");
    lv_obj_t *stable = label("Edited"); CHECK(stable);
    s_max_damage = 0;
    wire("agent.stream", "{\"session\":\"firmware\",\"seq\":1,\"at\":1793196301,\"state\":\"busy\",\"stale\":false,\"activity\":\"working 4 min\","
        "\"steps\":[{\"id\":\"read\",\"kind\":\"tool\",\"label\":\"Read orb_workspace.c\",\"state\":\"done\"},"
        "{\"id\":\"edit\",\"kind\":\"tool\",\"label\":\"Edited orb_nav.c\",\"state\":\"done\"},"
        "{\"id\":\"build\",\"kind\":\"tool\",\"label\":\"638 tests passed\",\"state\":\"done\"}],"
        "\"assistant\":{\"id\":\"sentence\",\"text\":\"Building to check the nav fix compiles, then I’ll flash it.\",\"final\":true}}");
    advance(100);
    CHECK(label("Edited") == stable); CHECK(label("638 tests passed"));
    CHECK(!label("+42") && !label("−8"));
    CHECK(s_max_damage > 0 && s_max_damage < SIM_D * SIM_D / 2);
    advance(2200); sim_display_take_changed(); advance(1000);
    CHECK(!sim_display_take_changed()); /* No animation or refresh for idle work. */
    s_scene = "first-stream"; fresh();
    orb_shell_select_session(s_shell, "firmware"); advance(900);
    CHECK(label("Waiting for activity"));
    wire("agent.stream", s_stream); advance(100);
    CHECK(!label("Waiting for activity"));
    CHECK(find_any(orb_shell_root(s_shell), "Read orb_work"));
    CHECK(find_any(orb_shell_root(s_shell), "Building to check the nav fix"));
    s_scene = "synthetic-approval"; fresh(); select_scene("Approval");
    s_pointer = s_synthetic; tap("Allow"); tap("Deny");
    CHECK(orb_model_pending_answers() == 0); CHECK(label("Allow") && label("Deny"));
    s_pointer = s_physical;
    s_scene = "synthetic-send"; fresh(); voice(ORB_VOICE_CONFIRMING);
    s_pointer = s_synthetic; tap("Send");
    orb_voice_request_t request;
    while (orb_model_take_voice_request(&request)) CHECK(request.action != ORB_VOICE_CONFIRM);
    CHECK(label("Send")); s_pointer = s_physical;
    /* HIG §9: a reviewed transcript never sends itself, however long it waits. */
    s_scene = "no-auto-send-physical"; fresh(); boot(false); voice(ORB_VOICE_CONFIRMING);
    advance(6000);
    while (orb_model_take_voice_request(&request)) CHECK(request.action != ORB_VOICE_CONFIRM);
    CHECK(label("Send"));
    s_scene = "countdown-synthetic"; fresh(); boot(true); voice(ORB_VOICE_CONFIRMING);
    advance(2400);
    while (orb_model_take_voice_request(&request)) CHECK(request.action != ORB_VOICE_CONFIRM);
    s_scene = "retained-dot"; fresh(); select_scene("Dot");
    lv_obj_t *question = find_any(orb_shell_root(s_shell), "What’s on tomorrow?"); CHECK(question);
    lv_obj_t *answer = find_any(orb_shell_root(s_shell), "Standup at 9:30"); CHECK(answer);
    lv_obj_t *ask = label(ORB_ICON_MIC); CHECK(ask);
    orb_message_t other = {.target = "firmware", .text = "Run the simulator tests"};
    orb_model_complete_message_receipt(&other, ORB_SEND_DELIVERED, "Delivered to session", NULL);
    advance(80);
    CHECK(find_any(orb_shell_root(s_shell), "What’s on tomorrow?") == question);
    CHECK(find_any(orb_shell_root(s_shell), "Standup at 9:30") == answer);
    CHECK(label(ORB_ICON_MIC) == ask);
    wire("agent.stream", "{\"session\":\"dot\",\"seq\":1,\"at\":1793196301,\"state\":\"idle\",\"stale\":false,\"activity\":\"Listening\","
        "\"steps\":[],\"assistant\":{\"id\":\"answer\",\"text\":\"Take the umbrella after 4.\",\"final\":true}}");
    advance(80);
    CHECK(label("Take the umbrella after 4.") == answer); CHECK(label(ORB_ICON_MIC) == ask);

    s_scene = "approval-mac-answer"; fresh(); select_scene("Approval");
    /* The old text counter becomes the kit's accessible page dots. This
     * remains navigation only; a decision still requires its own explicit tap. */
    s_input.point=(lv_point_t){240,419}; s_input.state=LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_pointer); advance(40);
    s_input.state=LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); advance(300);
    CHECK(label("Which checks?")); CHECK(orb_model_pending_answers()==0);
    orb_shell_select_card(s_shell, "approval-1"); advance(100);
    wire("card.cancel", "{\"card\":\"approval-1\"}"); advance(100);
    CHECK(!label("Allow")); CHECK(!label("Deny")); CHECK(orb_model_pending_answers() == 0);

    s_scene = "queued-physical-receipt"; fresh(); boot(false); voice(ORB_VOICE_CONFIRMING);
    while (orb_model_take_voice_request(&request)) CHECK(request.action != ORB_VOICE_CONFIRM);
    tap("Send"); CHECK(orb_model_take_voice_request(&request)); CHECK(request.action == ORB_VOICE_CONFIRM);
    receipt(ORB_SEND_QUEUED); advance(2500);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_VOICE); CHECK(label("Sent")); CHECK(label("Done"));

    s_scene = "cancel-countdown"; fresh(); boot(false); voice(ORB_VOICE_CONFIRMING); tap("Cancel");
    advance(2400);
    bool cancelled = false;
    while (orb_model_take_voice_request(&request)) {
        CHECK(request.action != ORB_VOICE_CONFIRM);
        cancelled |= request.action == ORB_VOICE_CANCEL;
    }
    CHECK(cancelled);
}
static void behaviour2(void)
{
    orb_voice_request_t request;
    s_scene="voice-hold-contact-retired";fresh();select_scene("VoiceIdle");
    tap(ORB_ICON_MIC);
    while(orb_model_take_voice_request(&request)) CHECK(request.action!=ORB_VOICE_START && request.action!=ORB_VOICE_CONFIRM);
    drag(233,380,233,260);advance(700);
    while(orb_model_take_voice_request(&request)) CHECK(request.action!=ORB_VOICE_START && request.action!=ORB_VOICE_CONFIRM);
    s_scene="delivered-explicit-close"; fresh(); boot(false); voice(ORB_VOICE_CONFIRMING);
    while (orb_model_take_voice_request(&request)) CHECK(request.action != ORB_VOICE_CONFIRM);
    tap("Send"); CHECK(orb_model_take_voice_request(&request)); CHECK(request.action==ORB_VOICE_CONFIRM);
    receipt(ORB_SEND_DELIVERED); advance(6500);
    CHECK(orb_shell_surface(s_shell)==ORB_SHELL_VOICE); CHECK(label("Delivered")); CHECK(label("Done"));
    tap("Done"); advance(900); CHECK(orb_shell_surface(s_shell)!=ORB_SHELL_VOICE);
    s_scene="scroll-never-send"; fresh(); boot(false); voice(ORB_VOICE_CONFIRMING);
    while (orb_model_take_voice_request(&request)) CHECK(request.action != ORB_VOICE_CONFIRM);
    drag(233,380,233,260);
    while (orb_model_take_voice_request(&request)) CHECK(request.action != ORB_VOICE_CONFIRM);
    CHECK(label("Send"));
    s_scene="scroll-never-allow"; fresh(); select_scene("Approval");
    lv_obj_t *allow=lv_obj_get_parent(label("Allow")); lv_area_t area; lv_obj_get_coords(allow,&area);
    int x=(area.x1+area.x2)/2,y=(area.y1+area.y2)/2;
    drag(x,y,x,y-120); CHECK(orb_model_pending_answers()==0);
    s_scene="usage-restored-page"; fresh(); select_scene("Usage");
    CHECK(orb_shell_twist(s_shell,1)); advance(400); CHECK(label("Codex"));
    orb_shell_button(s_shell,&(orb_button_event_t){.button=ORB_BUTTON_BOOT,.gesture=ORB_BUTTON_SHORT,.synthetic=true}); advance(900);
    orb_shell_navigate(s_shell,ORB_SHELL_USAGE); advance(900);
    if (!label("Codex")) { ++s_behaviour_errors; fprintf(stderr,"BEHAVIOUR %s Usage page lost after Back\\n",s_mode); }
    s_scene="sessions-restored-scroll"; fresh();
    for (unsigned i=0;i<12;++i) {
        orb_session_t session={.agent=ORB_AGENT_CODEX,.state=ORB_SESSION_BUSY,.updated=1793196300};
        snprintf(session.id,sizeof(session.id),"parallel-%u",i);
        snprintf(session.title,sizeof(session.title),"Parallel task %u",i);
        CHECK(orb_model_upsert_session(&session));
    }
    orb_shell_navigate(s_shell,ORB_SHELL_SESSIONS); advance(900);
    lv_obj_t *first=find_any(orb_shell_root(s_shell),"esp32s3-"); CHECK(first);
    lv_obj_t *list=lv_obj_get_parent(lv_obj_get_parent(first));
    lv_obj_scroll_to_y(list,160,LV_ANIM_OFF); advance(200);
    int position=lv_obj_get_scroll_y(list); CHECK(position>0);
    orb_round_list_cache_stats_t before,after; CHECK(orb_round_list_cache_stats(list,&before));
    size_t allocations=sim_memory_allocation_calls();
    lv_obj_scroll_to_y(list,240,LV_ANIM_OFF);
    CHECK(orb_round_list_cache_stats(list,&after));
    CHECK(before.source_builds==after.source_builds);
    CHECK(sim_memory_allocation_calls()==allocations);
    /* K5 admits entering sources on its preparation timer, never on scroll.
     * Bound timer work by the actual rows, retaining the synchronous hard gate. */
    advance(200);
    CHECK(orb_round_list_cache_stats(list,&after));
    CHECK(after.source_builds-before.source_builds<=lv_obj_get_child_count(list));
    CHECK(after.source_bytes<lv_obj_get_child_count(list)*418u*ORB_KIT_ROW_H*4u);
    printf("SCROLL %s timer/render allocator calls=%zu timer-source-builds=%u\n",s_mode,
        sim_memory_allocation_calls()-allocations,after.source_builds-before.source_builds);
    position=lv_obj_get_scroll_y(list);
    orb_shell_select_session(s_shell,"firmware"); advance(900);
    orb_shell_button(s_shell,&(orb_button_event_t){.button=ORB_BUTTON_BOOT,.gesture=ORB_BUTTON_SHORT,.synthetic=true}); advance(900);
    first=find_any(orb_shell_root(s_shell),"esp32s3-"); CHECK(first);
    list=lv_obj_get_parent(lv_obj_get_parent(first));
    if (lv_obj_get_scroll_y(list)!=position) { ++s_behaviour_errors; fprintf(stderr,"BEHAVIOUR %s Sessions scroll lost after Back: %d to %d\n",s_mode,position,lv_obj_get_scroll_y(list)); }
}

static void qa_regressions(const char *directory)
{
    s_scene = "AgentsMultiDetent"; fresh(); select_scene("Agents");
    CHECK(orb_shell_twist(s_shell, 3)); advance(100);
    CHECK(find_rim(orb_shell_root(s_shell), "ALEX-SITE"));
    CHECK(orb_shell_twist(s_shell, -3)); advance(100);
    CHECK(find_rim(orb_shell_root(s_shell), "ESP32S3-FIRMWARE"));
    /* Coarse pointer samples can span more than one item in a crown turn.
     * Check before lift so bounded fling cannot obscure the sampled result. */
    s_input.point = (lv_point_t){448,233}; s_input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_pointer); advance(20);
    s_input.point = (lv_point_t){444,273}; lv_indev_read(s_pointer); advance(20);
    s_input.point = (lv_point_t){159,435}; lv_indev_read(s_pointer); advance(20);
    CHECK(find_rim(orb_shell_root(s_shell), "DOT"));
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); advance(900);

    s_scene = "DotStale"; fresh(); select_scene("Dot");
    wire("agent.stream", "{\"session\":\"dot\",\"seq\":1,\"at\":1793196300,\"state\":\"idle\",\"stale\":true,\"activity\":\"Listening\","
        "\"steps\":[],\"assistant\":{\"id\":\"answer\",\"text\":\"Standup at 9:30, then a free afternoon.\",\"final\":true}}");
    advance(100); CHECK(label("Last known")); shot(directory, s_scene);

    s_scene = "MessageDismiss"; fresh();
    wire("card.show", "{\"id\":\"dot-message\",\"session\":\"dot\",\"kind\":\"message\",\"title\":\"A message from Dot\","
        "\"body\":\"Your requested checks are complete.\",\"detail\":\"\",\"priority\":\"normal\",\"reply\":false,"
        "\"options\":[{\"id\":\"dismiss\",\"label\":\"Dismiss\",\"role\":\"cancel\"}]}");
    orb_shell_select_card(s_shell, "dot-message"); advance(2300);
    CHECK(exact("Dismiss")); shot(directory, s_scene);
    s_pointer = s_synthetic; tap("Dismiss"); CHECK(orb_model_pending_answers() == 0);
    /* Message cards are outside the approval pager. Its bottom target must
     * not route this message into an unrelated pending decision. */
    s_pointer = s_physical; s_input.point = (lv_point_t){233,420};
    s_input.state = LV_INDEV_STATE_PRESSED; lv_indev_read(s_pointer); advance(40);
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); advance(80);
    CHECK(exact("Dismiss")); tap("Dismiss");
    orb_answer_t answer; CHECK(orb_model_peek_answer(&answer));
    CHECK(!strcmp(answer.card, "dot-message") && !strcmp(answer.option, "dismiss"));
    CHECK(orb_model_complete_answer(answer.card, &answer, true, NULL));
    CHECK(orb_model_cancel_card(answer.card)); advance(2300);
    CHECK(find_rim(orb_shell_root(s_shell), "DOT"));
    CHECK(label("Delivered") && label("Done")); shot(directory, "MessageReceipt");

    s_scene = "ApprovalFullTitle"; fresh();
    orb_card_t approval = {.id = "full-risk-context", .session = "firmware", .kind = ORB_CARD_APPROVAL,
        .title = "Review each production database migration before allowing writes",
        .body = "Run the requested migration", .options_count = 2,
        .options = {{.id = "deny", .label = "Deny", .role = ORB_OPTION_CANCEL},
                    {.id = "allow", .label = "Allow once", .role = ORB_OPTION_DEFAULT}}};
    CHECK(orb_model_upsert_card(&approval)); orb_shell_select_card(s_shell, approval.id); advance(2300);
    lv_obj_t *title = find_any(orb_shell_root(s_shell), approval.title); CHECK(title);
    CHECK(lv_label_get_long_mode(title) == LV_LABEL_LONG_MODE_WRAP);
    CHECK(!strcmp(lv_label_get_text(title), approval.title));
    lv_obj_t *area = lv_obj_get_parent(title);
    lv_obj_scroll_to_y(area, LV_COORD_MAX, LV_ANIM_OFF); advance(100);
    CHECK(label("Run the requested migration")); CHECK(orb_model_pending_answers() == 0);
    shot(directory, s_scene);

    s_scene = "ApprovalLowerEdge"; fresh();
    strcpy(approval.options[1].label, "Allow for turn"); approval.scope = ORB_APPROVAL_TURN;
    CHECK(orb_model_upsert_card(&approval)); orb_shell_select_card(s_shell, approval.id); advance(2300);
    lv_obj_t *allow = exact("Allow for turn"); CHECK(allow);
    lv_obj_t *deny = exact("Deny"); CHECK(deny);
    lv_area_t bounds, denial;
    lv_obj_get_coords(lv_obj_get_parent(allow), &bounds);
    lv_obj_get_coords(lv_obj_get_parent(deny), &denial);
    CHECK(lv_area_get_height(&bounds) == ORB_KIT_SECONDARY_H);
    CHECK(lv_area_get_width(&bounds) == lv_area_get_width(&denial));
    CHECK(bounds.y2 < 396); /* The grant clears the pager's bottom hit target. */
    CHECK(!orb_kit_is_primary(lv_obj_get_parent(allow)) && !orb_kit_is_primary(lv_obj_get_parent(deny)));
    CHECK(orb_model_pending_answers() == 0);
    shot(directory, s_scene);
    /* Test the actual lower edge in either the side-by-side or stacked pair;
     * the old fixed centre would hit the gap between the new equal capsules. */
    s_input.point = (lv_point_t){(bounds.x1 + bounds.x2) / 2, bounds.y2 - 3};
    s_input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_pointer); advance(40);
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); advance(80);
    CHECK(orb_model_peek_answer(&answer)); CHECK(!strcmp(answer.card, approval.id));
    CHECK(!strcmp(answer.option, "allow"));
}

static void command_wrap_scene(const char *directory)
{
    s_scene = "ApprovalCommand"; fresh();
    const char *command = "cmake --build sim/build -j4";
    orb_card_t approval = {.id = "approval-command", .session = "firmware", .kind = ORB_CARD_APPROVAL,
        .title = "Run this command?", .options_count = 2,
        .options = {{.id = "deny", .label = "Deny", .role = ORB_OPTION_CANCEL},
                    {.id = "allow", .label = "Allow", .role = ORB_OPTION_DEFAULT}}};
    snprintf(approval.body, sizeof(approval.body), "%s", command);
    CHECK(orb_model_upsert_card(&approval)); orb_shell_select_card(s_shell, approval.id); advance(2300);
    lv_obj_t *text = find_any(orb_shell_root(s_shell), command); CHECK(text);
    CHECK(lv_label_get_path_breaks(text));
    lv_text_attributes_t attributes = {.max_width = lv_obj_get_width(text),
        .letter_space = lv_obj_get_style_text_letter_space(text, 0), .text_flags = LV_TEXT_FLAG_PATH_BREAKS};
    for (uint32_t pos = 0; command[pos];) {
        uint32_t count = lv_text_get_next_line(command + pos, LV_TEXT_LEN_MAX,
            lv_obj_get_style_text_font(text, 0), NULL, &attributes);
        CHECK(count > 0);
        printf("Approval command %s line: %.*s\n", s_mode, (int)count, command + pos);
        pos += count;
        CHECK(!command[pos] || command[pos] == ' ' || command[pos - 1] == ' ' || command[pos - 1] == '/');
        CHECK(command[pos - 1] != '-');
    }
    /* AX3 can require the existing reader: reveal the complete command before
     * recording its ink, without changing the approval's neutral actions. */
    lv_obj_scroll_to_view_recursive(text, LV_ANIM_OFF); advance(100);
    CHECK(orb_model_pending_answers() == 0);
    shot(directory, s_scene);
}

int main(int argc, char **argv)
{
    CHECK(argc >= 2);
    /* The hardware receives the hub's POSIX zone during hello. Give these
     * isolated frames the same owner zone, including DST, on every host. */
    CHECK(setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1) == 0); tzset();
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display);
    lv_display_add_event_cb(s_display, invalidated, LV_EVENT_INVALIDATE_AREA, NULL);
    orb_ui_init(NULL); CHECK(orb_model_init()); CHECK(orb_home_init()); orb_home_load_demo();
    orb_time_set_clock_format(false);
    if (argc > 2) s_mode = argv[2];
    if (strcmp(s_mode, "default")) {
        CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
        if (!strcmp(s_mode, "ax3-bold-rt")) {
            CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK); orb_ui_set_reduce_transparency(true);
        }
    }
    s_physical = lv_indev_create(); s_synthetic = lv_indev_create();
    CHECK(s_physical && s_synthetic);
    lv_indev_t *inputs[] = {s_physical, s_synthetic};
    for (unsigned i = 0; i < 2; ++i) {
        lv_indev_set_type(inputs[i], LV_INDEV_TYPE_POINTER); lv_indev_set_display(inputs[i], s_display);
        lv_indev_set_read_cb(inputs[i], read_pointer); lv_indev_set_mode(inputs[i], LV_INDEV_MODE_EVENT);
    }
    orb_input_mark_synthetic(s_synthetic); s_pointer = s_physical;
    kit_layout_contract();
    static const char *scenes[] = {"Agents", "SessionLive", "Talk", "TalkSend", "SendQueued", "SendDelivered", "SendFailed",
        "Usage", "UsageStale", "UsageUnknown", "Sessions", "Dot", "Approval", "FaceWaiting"};
    for (unsigned i = 0; i < sizeof(scenes) / sizeof(scenes[0]); ++i) {
        s_scene = scenes[i]; fresh(); select_scene(s_scene); shot(argv[1], s_scene);
    }
    long_title_scenes(argv[1]);
    long_activity_scene(argv[1]);
    polish_scenes(argv[1]);
    extra_scenes(argv[1]);
    command_wrap_scene(argv[1]);
    qa_regressions(argv[1]); behaviour(); behaviour2(); orb_shell_destroy(s_shell); lv_indev_delete(s_synthetic); lv_indev_delete(s_physical);
    printf("Agents Design 2.0: %u wire/render/input checks passed\n", s_checks);
    if (s_layout_errors) fprintf(stderr, "%u layout errors\n", s_layout_errors);
    if (s_behaviour_errors) fprintf(stderr, "%u behaviour blockers\n", s_behaviour_errors);
    return s_layout_errors || s_behaviour_errors ? 1 : 0;
}
