/* Real shell sound screens, bounded overlay allocation and safe input regressions. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "orb_fonts.h"
#include "orb_input.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_sound.h"
#include "orb_time.h"
#include <time.h>
#include "orb_sound_overlay.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include "src/misc/lv_text_private.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks, s_stops;
static bool s_speaking;
static lv_display_t *s_display;
static lv_indev_t *s_pointer, *s_physical, *s_synthetic;
static lv_indev_data_t s_input;
static orb_shell_t *s_shell;
#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "sound %u ms: %s:%d: %s\n", s_now, __FILE__, __LINE__, #value); exit(1); \
} } while (0)
static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *input, lv_indev_data_t *data) { (void)input; *data = s_input; }
static bool stop(void *ctx) { (void)ctx; if (!s_speaking) return false; ++s_stops; s_speaking = false; return true; }
static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 20) {
        s_now += 20; orb_shell_poll(s_shell); lv_timer_handler(); lv_refr_now(s_display);
    }
}
static lv_obj_t *find(lv_obj_t *root, const char *value)
{
    const char *rim_text = orb_kit_rim_text_get(root);
    if (rim_text && !strcasecmp(rim_text, value)) return root;
    if (lv_obj_is_hidden(root)) return NULL;
    const char *semantic = orb_kit_button_get_text(root);
    if (semantic && !strcmp(semantic, value)) return root;
    if (lv_obj_check_type(root, &lv_label_class)) {
        const char *shown = lv_label_get_text(root);
        if (!strcmp(shown, value)) return root;
        /* LVGL DOTS mutates its retained display buffer on refresh. Match the
         * fixture's visible prefix when AX3 ellipsises a shared 72px row. */
        const char *dots = strstr(shown, "...");
        if (!dots) dots = strstr(shown, "\xe2\x80\xa6");
        if (dots && dots - shown >= 4 && !strncmp(shown, value, dots - shown)) return root;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *hit = find(lv_obj_get_child(root, i), value); if (hit) return hit;
    }
    return NULL;
}
static lv_obj_t *first_rim(lv_obj_t *root)
{
    orb_kit_rim_text_info_t info;
    if (orb_kit_rim_text_info(root, &info)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *hit = first_rim(lv_obj_get_child(root, i)); if (hit) return hit;
    }
    return NULL;
}
static void no_input(lv_obj_t *root)
{
    if (lv_obj_is_clickable(root) || lv_obj_is_scrollable(root))
        fprintf(stderr, "HUD input target %p label=%s click%d scroll%d children%u\n", (void *)root,
            lv_obj_check_type(root, &lv_label_class) ? lv_label_get_text(root) : "-",
            lv_obj_is_clickable(root), lv_obj_is_scrollable(root), (unsigned)lv_obj_get_child_count(root));
    CHECK(!lv_obj_is_clickable(root) && !lv_obj_is_scrollable(root));
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) no_input(lv_obj_get_child(root, i));
}
static void text_ink_bounds(lv_obj_t *label, lv_area_t *area)
{
    lv_obj_get_coords(label, area);
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    const char *text = lv_label_get_text(label);
    int32_t top = font->line_height, bottom = 0;
    uint32_t offset = 0;
    while (text[offset]) {
        uint32_t cp = lv_text_encoded_next(text, &offset);
        uint32_t peek = offset, next = text[peek] ? lv_text_encoded_next(text, &peek) : 0;
        lv_font_glyph_dsc_t glyph;
        if (!lv_font_get_glyph_dsc(font, &glyph, cp, next) || !glyph.box_h) continue;
        int32_t y = font->line_height - font->base_line - glyph.box_h - glyph.ofs_y;
        top = LV_MIN(top, y); bottom = LV_MAX(bottom, y + glyph.box_h - 1);
    }
    area->y2 = area->y1 + bottom; area->y1 += top;
}
/* Check actual prepared row geometry, including the narrow trailing switch
 * lane under AX3. Measuring each label alone misses title/subtitle collisions. */
static void row_overlap(lv_obj_t *row)
{
    if (lv_obj_get_height(row) < ORB_KIT_ROW_H || lv_obj_get_width(row) != 418) return;
    for (unsigned i = 0; i < lv_obj_get_child_count(row); i++) {
        lv_obj_t *a = lv_obj_get_child(row, i);
        if (!lv_obj_check_type(a, &lv_label_class)) continue;
        lv_area_t aa; text_ink_bounds(a, &aa);
        for (unsigned j = i + 1; j < lv_obj_get_child_count(row); j++) {
            lv_obj_t *b = lv_obj_get_child(row, j);
            if (!lv_obj_check_type(b, &lv_label_class)) continue;
            lv_area_t bb; text_ink_bounds(b, &bb);
            if (aa.x1 <= bb.x2 && bb.x1 <= aa.x2 && aa.y1 <= bb.y2 && bb.y1 <= aa.y2)
                fprintf(stderr, "Row overlap: %s / %s\n", lv_label_get_text(a), lv_label_get_text(b));
            CHECK(aa.x1 > bb.x2 || bb.x1 > aa.x2 || aa.y1 > bb.y2 || bb.y1 > aa.y2);
        }
    }
}
static void fit(lv_obj_t *object)
{
    if (!lv_obj_is_visible(object)) return;
    row_overlap(object);
    if (lv_obj_check_type(object, &lv_label_class) && *lv_label_get_text(object)) {
        lv_area_t area; lv_obj_get_coords(object, &area);
        for (lv_obj_t *parent = lv_obj_get_parent(object); parent; parent = lv_obj_get_parent(parent)) {
            lv_area_t shown;
            orb_round_list_cache_stats_t cache;
            if (orb_round_list_cache_stats(lv_obj_get_parent(parent), &cache)) {
                /* Cached sources stay semantically present outside their
                 * visible chord; inspect only the mapped, clipped label. */
                if (!orb_round_list_child_bounds(parent, object, &shown) ||
                    !lv_obj_area_is_visible(parent, &shown)) return;
                area = shown;
                break;
            }
            if (orb_round_list_child_bounds(parent, object, &shown)) { area = shown; break; }
            if (lv_obj_get_style_opa(parent, 0) == 0) return;
            if (!lv_obj_is_overflow_visible(parent)) {
                lv_area_t clip; lv_obj_get_coords(parent, &clip);
                area.x1 = LV_MAX(area.x1, clip.x1); area.x2 = LV_MIN(area.x2, clip.x2);
                area.y1 = LV_MAX(area.y1, clip.y1); area.y2 = LV_MIN(area.y2, clip.y2);
                if (area.x1 > area.x2 || area.y1 > area.y2) return;
            }
        }
        if (area.y1 >= 0 && area.y2 < 466) {
            int chord = orb_chord_width(area.y1, area.y2, ORB_RIM_MARGIN_TEXT), left = (466 - chord) / 2;
            if (area.x1 < left || area.x2 >= left + chord)
                fprintf(stderr, "chord: %s (%d,%d)-(%d,%d) width%d\n", lv_label_get_text(object), area.x1,area.y1,area.x2,area.y2,chord);
            CHECK(area.x1 >= left && area.x2 < left + chord);
            lv_point_t size; lv_text_get_size(&size, lv_label_get_text(object), lv_obj_get_style_text_font(object,0),
                lv_obj_get_style_text_letter_space(object,0), lv_obj_get_style_text_line_space(object,0),
                lv_obj_get_width(object), LV_TEXT_FLAG_NONE);
            if (lv_label_get_long_mode(object) != LV_LABEL_LONG_MODE_DOTS &&
                (size.x > lv_obj_get_width(object) || size.y > lv_obj_get_height(object)))
                fprintf(stderr,"fit: %s text%d,%d box%d,%d\n",lv_label_get_text(object),size.x,size.y,(int)lv_obj_get_width(object),(int)lv_obj_get_height(object));
            CHECK(lv_label_get_long_mode(object) == LV_LABEL_LONG_MODE_DOTS ||
                  (size.x <= lv_obj_get_width(object) && size.y <= lv_obj_get_height(object)));
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) fit(lv_obj_get_child(object,i));
}
static void tap(const char *value, bool synthetic)
{
    s_pointer = synthetic ? s_synthetic : s_physical;
    lv_obj_t *caption = find(lv_display_get_layer_top(s_display), value);
    if (!caption) caption = find(orb_shell_root(s_shell), value);
    CHECK(caption);
    lv_obj_t *button = lv_obj_is_clickable(caption) ? caption : lv_obj_get_parent(caption);
    while (button && !lv_obj_is_clickable(button)) button = lv_obj_get_parent(button);
    CHECK(button);
    if (lv_obj_is_scrollable(lv_obj_get_parent(button))) lv_obj_scroll_to_view(button, LV_ANIM_OFF);
    advance(40); lv_area_t area; lv_obj_get_coords(button, &area);
    s_input.point = (lv_point_t){(area.x1+area.x2)/2,(area.y1+area.y2)/2};
    s_input.state=LV_INDEV_STATE_PRESSED; lv_indev_read(s_pointer); advance(40);
    s_input.state=LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); advance(80);
}
static void shot(const char *dir, const char *name, lv_obj_t *root)
{
    fit(root); char path[1024]; snprintf(path,sizeof(path),"%s/%s.png",dir,name);
    CHECK(sim_display_save_png(path));
}
static const char *s_caption = "Standup at 9:30, then a free afternoon. Rain after 4, so take the umbrella.";
int main(int argc, char **argv)
{
    CHECK(argc == 3); bool ax = !strncmp(argv[2], "ax3", 3);
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); CHECK(orb_model_init()); orb_model_reset(); orb_model_set_connection(true,true);
    if (ax) {
        CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3)==ORB_FONTS_OK);
        if (!strcmp(argv[2], "ax3-bold-rt")) {
            CHECK(orb_fonts_set_bold_text(true)==ORB_FONTS_OK); orb_ui_set_reduce_transparency(true);
        }
    }
    orb_glass_cache_prewarm();
    s_physical=lv_indev_create();s_synthetic=lv_indev_create(); CHECK(s_physical && s_synthetic);
    lv_indev_t *inputs[]={s_physical,s_synthetic};
    for(unsigned i=0;i<2;i++) {lv_indev_set_type(inputs[i],LV_INDEV_TYPE_POINTER);lv_indev_set_display(inputs[i],s_display);
        lv_indev_set_read_cb(inputs[i],read_pointer);lv_indev_set_mode(inputs[i],LV_INDEV_MODE_EVENT);}
    orb_input_mark_synthetic(s_synthetic);
    orb_sound_preferences_t prefs; orb_sound_preferences(&prefs); prefs.volume=60; prefs.spoken_replies=true;
    prefs.quiet_enabled=false; CHECK(orb_sound_set_preferences(&prefs));
    s_shell=orb_shell_create(lv_screen_active(),NULL,NULL); CHECK(s_shell);
    orb_shell_snapshot_t status={.net=ORB_SHELL_NET_CONNECTED,.link=ORB_SHELL_LINK_ONLINE,.battery_pct=-1,.brightness=70};
    strcpy(status.clock_time,"10:09");orb_shell_update(s_shell,&status);orb_shell_set_speech_stop(s_shell,stop,NULL);
    orb_shell_navigate(s_shell,ORB_SHELL_SOUND);advance(1200);
    CHECK(find(orb_shell_root(s_shell), "Sound"));
    CHECK(find(orb_shell_root(s_shell),"Spoken replies"));
    CHECK(!find(orb_shell_root(s_shell), "Voice"));
    shot(argv[1], "SoundSettings-unknown", orb_shell_root(s_shell));
    CHECK(orb_model_set_speech_voice("Ava")); advance(200);
    CHECK(find(orb_shell_root(s_shell), "Voice")); CHECK(find(orb_shell_root(s_shell), "Ava"));
    shot(argv[1],"SoundSettings",orb_shell_root(s_shell));
    CHECK(orb_model_set_speech_voice("Ava, English United States, Enhanced, from the Mac mini")); advance(200);
    shot(argv[1], "SoundSettings-long", orb_shell_root(s_shell));
    CHECK(orb_model_set_speech_voice("Ava"));
    orb_model_set_connection(false, false); advance(200);
    shot(argv[1], "SoundSettings-offline", orb_shell_root(s_shell));
    orb_model_set_connection(true, true); CHECK(orb_model_set_speech_voice("Ava")); advance(200);
    lv_obj_scroll_to_view(lv_obj_get_parent(find(orb_shell_root(s_shell), "Voice")), LV_ANIM_OFF); advance(80);
    shot(argv[1], "SoundSettings-voice", orb_shell_root(s_shell));
    tap("Volume", false); advance(450); CHECK(find(orb_shell_root(s_shell), "60%"));
    CHECK(orb_sound_volume() == 60);
    shot(argv[1], "SoundVolume", orb_shell_root(s_shell));
    CHECK(orb_shell_twist(s_shell, 1)); advance(40); CHECK(orb_sound_volume() == 61);
    orb_sound_set_volume(60); orb_shell_back(s_shell); advance(1600);
    lv_obj_t *volume_label = find(orb_shell_root(s_shell), "Volume"); CHECK(volume_label);
    lv_obj_t *settings_list = lv_obj_get_parent(lv_obj_get_parent(volume_label));
    lv_obj_scroll_to_y(settings_list, ax ? 540 : 290, LV_ANIM_OFF); advance(80);
    CHECK(find(orb_shell_root(s_shell), "Quiet hours")); CHECK(find(orb_shell_root(s_shell), "Always On"));
    shot(argv[1], "SoundSettings-scrolled", orb_shell_root(s_shell));
    lv_mem_monitor_t settings_memory, palette_memory; lv_mem_monitor(&settings_memory);
    tap("System sounds", false); advance(100);
    lv_mem_monitor(&palette_memory);
    printf("SystemSounds: LVGL payload delta over SoundSettings %td bytes; no C view state or timer\n",
           (ptrdiff_t)palette_memory.total_size - (ptrdiff_t)settings_memory.total_size);
    CHECK(find(orb_shell_root(s_shell), "Detent"));
    shot(argv[1], "SystemSounds", orb_shell_root(s_shell));
    orb_sound_stats_t preview_before, preview_after; orb_sound_stats(&preview_before);
    tap("Alarm", false); orb_sound_stats(&preview_after);
    CHECK(preview_after.queued_count > preview_before.queued_count);
    CHECK(orb_sound_volume() == 60);
    shot(argv[1], "SystemSounds-scrolled", orb_shell_root(s_shell));
    orb_shell_back(s_shell); advance(100);
    CHECK(find(orb_shell_root(s_shell), "Spoken replies"));
    tap("Always On", false); advance(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_DISPLAY);
    orb_shell_back(s_shell); advance(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SOUND);
    orb_time_set_clock_format(false);
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
    orb_shell_navigate(s_shell, ORB_SHELL_SOUND); advance(1200);
    CHECK(find(orb_shell_root(s_shell), "Off · 10:00 PM–7:00 AM"));
    tap("Quiet hours", false); advance(1200);
    shot(argv[1], "QuietHours-12", orb_shell_root(s_shell));
    CHECK(find(orb_shell_root(s_shell), "10:00 PM"));
    CHECK(find(orb_shell_root(s_shell), "7:00 AM"));
    tap("Starts", false); advance(1200); CHECK(find(orb_shell_root(s_shell), "10:00 PM"));
    orb_shell_back(s_shell); advance(1200);
    tap("Ends", false); advance(1200); CHECK(find(orb_shell_root(s_shell), "7:00 AM"));
    orb_shell_back(s_shell); orb_shell_back(s_shell); advance(1200);
    orb_time_set_clock_format(true);
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
    orb_shell_navigate(s_shell, ORB_SHELL_SOUND); advance(1200);
    CHECK(find(orb_shell_root(s_shell), "Off · 22:00–07:00"));
    tap("Quiet hours", false); shot(argv[1], "QuietHours", orb_shell_root(s_shell));
    CHECK(find(orb_shell_root(s_shell), "22:00"));
    CHECK(find(orb_shell_root(s_shell), "07:00"));
    tap("Starts", false); shot(argv[1], "QuietHours-start", orb_shell_root(s_shell));
    CHECK(orb_shell_twist(s_shell, 1)); advance(80); orb_sound_preferences(&prefs); CHECK(prefs.quiet_start == 23);
    CHECK(orb_sound_volume() == 60); orb_shell_back(s_shell); orb_shell_back(s_shell); advance(1200);
    tap("Spoken replies",false);orb_sound_preferences(&prefs);CHECK(!prefs.spoken_replies);CHECK(!orb_sound_speech_allowed());
    tap("Spoken replies",false);orb_sound_preferences(&prefs);CHECK(prefs.spoken_replies);
    tap("Silent",false);orb_sound_stats_t stats;orb_sound_stats(&stats);CHECK(stats.silent);
    CHECK(orb_sound_play(ORB_SOUND_ALARM));orb_sound_stop(ORB_SOUND_ALARM);tap("Silent",false);
    lv_obj_t *title=first_rim(orb_shell_root(s_shell));CHECK(title);
    lv_obj_t *page=lv_obj_get_parent(title);lv_mem_monitor_t before,after;lv_mem_monitor(&before);lv_obj_clean(page);lv_mem_monitor(&after);
    printf("SoundSettings: LVGL tree children %zu bytes; C view state 0 bytes\n",before.total_size-after.total_size);
    orb_shell_navigate(s_shell,ORB_SHELL_FACE);advance(1200);
    /* Design 2.0: the rim drives what is on screen or nothing; it no longer
     * falls back to system volume where nothing scrolls. */
    orb_shell_twist(s_shell,1);advance(20);CHECK(orb_sound_volume()==60);
    CHECK(!find(lv_display_get_layer_top(s_display),"60%"));
    orb_sound_set_volume(61);advance(20);orb_sound_set_volume(60);advance(20);
    /* The observer's real change now Follows; inspect its settled ink. */
    advance(300);
    lv_obj_t *value=find(lv_display_get_layer_top(s_display),"60%");CHECK(value);
    shot(argv[1],"VolumeHUD",lv_obj_get_parent(value));
    /* The HUD is a pure observer: no descendant is an input target and its
     * legacy adjustment entry point no longer mutates a hidden global value. */
    no_input(lv_obj_get_parent(value));
    s_pointer = s_physical; s_input.point = (lv_point_t){233, 21};
    s_input.state = LV_INDEV_STATE_PRESSED; lv_indev_read(s_pointer); advance(40);
    s_input.point = (lv_point_t){400, 90}; lv_indev_read(s_pointer); advance(40);
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); advance(40);
    CHECK(orb_sound_volume() == 60); advance(1600);
    orb_shell_navigate(s_shell, ORB_SHELL_CONTROL_CENTER); advance(1200);
    CHECK(orb_shell_twist(s_shell, 1)); advance(40);
    CHECK(orb_shell_brightness(s_shell) == 71 && orb_sound_volume() == 60);
    tap(ORB_ICON_SPEAKER_LOW " 60%", false); advance(80);
    CHECK(orb_shell_twist(s_shell, 1)); advance(40);
    CHECK(orb_sound_volume() == 61 && orb_shell_brightness(s_shell) == 71);
    advance(1600);
    orb_voice_state_t speech_origin = {.stream = 1, .phase = ORB_VOICE_IDLE, .target = "dot"};
    CHECK(orb_model_set_voice(&speech_origin));
    s_speaking=true;orb_shell_spoken_reply(s_shell,true,false,s_caption);advance(40);
    lv_obj_t *caption=find(lv_display_get_layer_top(s_display),s_caption);CHECK(caption);
    int caption_height = lv_obj_get_style_text_font(caption, 0)->line_height;
    int caption_spacing = lv_obj_get_style_text_line_space(caption, 0);
    int viewport_height = lv_obj_get_height(lv_obj_get_parent(caption));
    CHECK((viewport_height + caption_spacing) % (caption_height + caption_spacing) == 0);
    CHECK(lv_obj_get_y(lv_obj_get_parent(caption)) + viewport_height <= 312);
    CHECK(find(lv_display_get_layer_top(s_display), "Dot"));
    /* The stopping hint is now an upright kit rim caption. */
    CHECK(first_rim(lv_display_get_layer_top(s_display)));
    shot(argv[1],"SpokenReply",lv_obj_get_parent(lv_obj_get_parent(caption)));
    /* Control Center is retained behind the caption modal, but the owner is
     * not looking at its level. An external change still deserves the HUD. */
    orb_sound_set_volume(62); advance(40);
    CHECK(find(lv_display_get_layer_top(s_display), "62%"));
    orb_sound_set_volume(61); advance(1600);
    tap(ORB_ICON_STOP_FILL,true);CHECK(s_stops==0 && s_speaking);
    tap(ORB_ICON_STOP_FILL,false);CHECK(s_stops==1 && !s_speaking);
    orb_shell_spoken_reply(s_shell,false,false,NULL);advance(40);
    const orb_session_t speakers[] = {
        {.id = "speech-claude", .agent = ORB_AGENT_CLAUDE, .title = "Claude", .state = ORB_SESSION_DONE},
        {.id = "speech-codex", .agent = ORB_AGENT_CODEX, .title = "Codex", .state = ORB_SESSION_DONE},
    };
    CHECK(orb_model_sessions_chunk(speakers, 2, true, false)); advance(40);
    const char *speaker_ids[] = {"speech-claude", "speech-codex", "unknown-speaker"};
    const char *speaker_names[] = {"Claude", "Codex", "Spoken reply"};
    for (unsigned i = 0; i < 3; i++) {
        speech_origin.stream++;
        snprintf(speech_origin.target, sizeof(speech_origin.target), "%s", speaker_ids[i]);
        CHECK(orb_model_set_voice(&speech_origin));
        orb_shell_spoken_reply(s_shell, true, false, s_caption); advance(40);
        CHECK(find(lv_display_get_layer_top(s_display), speaker_names[i]));
        orb_shell_spoken_reply(s_shell, false, false, NULL);
    }
    s_speaking=true;orb_shell_spoken_reply(s_shell,true,false,"Actual streamed caption");CHECK(orb_shell_double_tap(s_shell));
    CHECK(s_stops==2);CHECK(orb_model_pending_answers()==0);orb_shell_spoken_reply(s_shell,false,false,NULL);
    /* A game/foreign active screen cannot hide a ringing alarm. Speech and
     * transient volume yield. Design 2.0 Back dismisses the presentation,
     * while PWR, orientation and polling retain the service's ringing latch. */
    lv_obj_t *previous_screen = lv_screen_active(), *game_screen = lv_obj_create(NULL);
    lv_screen_load(game_screen);
    s_speaking = true; orb_shell_spoken_reply(s_shell, true, false, "Interrupted actual caption");
    struct tm before_alarm = {.tm_year=126,.tm_mon=9,.tm_mday=28,.tm_hour=6,.tm_min=59,.tm_isdst=-1};
    time_t wall = mktime(&before_alarm); orb_time_init(); orb_time_poll((int64_t)s_now * 1000, wall);
    CHECK(orb_time_alarm_set(7,0)); CHECK(orb_time_alarm_enable(true,wall));
    orb_time_poll((int64_t)s_now * 1000,wall+60); advance(80);
    CHECK(find(lv_display_get_layer_top(s_display), "Snooze"));
    CHECK(!find(lv_display_get_layer_top(s_display), "Interrupted actual caption"));
    CHECK(!s_speaking); CHECK(orb_shell_surface(s_shell)==ORB_SHELL_TIME);
    orb_shell_back(s_shell); advance(80);
    CHECK(!find(lv_display_get_layer_top(s_display), "Snooze"));
    orb_time_snapshot_t ringing; orb_time_snapshot(orb_time_now_us(), &ringing);
    CHECK(ringing.alarm_ringing);
    orb_shell_power(s_shell); orb_shell_navigate(s_shell,ORB_SHELL_SETTINGS);
    CHECK(orb_shell_surface(s_shell)==ORB_SHELL_SETTINGS);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    orb_shell_environment(s_shell,0,0,-1,false);advance(80);
    CHECK(!find(lv_display_get_layer_top(s_display), "Snooze"));
    orb_time_snapshot(orb_time_now_us(), &ringing); CHECK(ringing.alarm_ringing);
    /* Explicit service Stop for fixture cleanup; actual Stop/Snooze pointer
     * controls are covered by the Time shell integration flow. */
    orb_time_dismiss(false, true);
    orb_shell_spoken_reply(s_shell,false,false,NULL);
    lv_screen_load(previous_screen);lv_obj_delete(game_screen);
    orb_shell_destroy(s_shell);s_shell=NULL;
    /* Isolated exact overlay payloads, after shared material keys are warm. */
    orb_sound_overlay_t overlay;orb_sound_overlay_init(&overlay);lv_mem_monitor(&before);
    orb_sound_overlay_speech(&overlay,s_display,true,false,s_caption, -1);lv_mem_monitor(&after);
    printf("SpokenReply: LVGL tree %zu bytes; overlay handles shared %zu bytes\n",after.total_size-before.total_size,orb_sound_overlay_size());
    orb_sound_overlay_destroy(&overlay);orb_sound_set_volume(60);orb_sound_overlay_init(&overlay);lv_mem_monitor(&before);
    orb_sound_set_volume(61);orb_sound_overlay_poll(&overlay,s_display,true);lv_mem_monitor(&after);
    printf("VolumeHUD: LVGL tree %zu bytes; shared overlay handles %zu bytes\n",after.total_size-before.total_size,orb_sound_overlay_size());
    CHECK(orb_sound_volume() == 61);
    no_input(overlay.volume);
    orb_sound_overlay_destroy(&overlay);
    printf("sound screens %s: %u checks passed\n",argv[2],s_checks);return 0;
}
