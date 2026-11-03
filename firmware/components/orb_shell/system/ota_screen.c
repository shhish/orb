/* HIG §§1/3/4/7/8: preserve the existing update reader, inset progress
 * ring and explicit physical installation. No OTA service belongs here. */
#include "ota_screen.h"
#include "../orb_system_copy.h"
#include "orb_kit.h"
#include "orb_kit_controls_motion.h"
#include "orb_kit_effects.h"
#include "orb_kit_pages.h"
#include "orb_page.h"
#include "orb_input.h"
#include "orb_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef enum { LINE_TITLE, LINE_SUBTITLE, LINE_CODE, LINE_BODY, LINE_COMMAND, LINE_DETAIL } line_t;
typedef struct {
    orb_ota_source_t source;
    lv_obj_t *title, *art, *action, *ready_check;
    int16_t scroll[6];
    orb_ota_page_t page;
    bool online, pending, action_shown, progress_set;
    orb_ota_wait_t wait;
} ota_state_t;
typedef struct { int32_t scroll[6]; uint32_t wait_started, wait_page; bool waiting; } ota_context_t;

static void progress_format(float value, char *out, size_t capacity, void *context)
{
    (void)context;
    snprintf(out, capacity, "%d%%", (int)lroundf(value));
}

static bool borrow(const ota_state_t *state, orb_ota_scaffold_t *port)
{
    return state->source.read && state->source.read(state->source.context, port);
}
static void visible(lv_obj_t *object, bool show)
{
    lv_obj_set_hidden(object, !show);
    orb_kit_scroll_arc_refresh(object);
}
static bool online(const orb_shell_snapshot_t *status)
{
    return status->net == ORB_SHELL_NET_CONNECTED && status->link == ORB_SHELL_LINK_ONLINE;
}
static void owned_deleted(lv_event_t *event)
{
    ota_state_t *state = lv_event_get_user_data(event);
    lv_obj_t *object = lv_event_get_current_target_obj(event);
    if (object == state->art) state->art = NULL;
    if (object == state->action) state->action = NULL;
    if (object == state->ready_check) state->ready_check = NULL;
}
static void clicked(lv_event_t *event)
{
    orb_screen_instance_t *instance = lv_event_get_user_data(event);
    const ota_state_t *state = orb_screen_state(instance);
    orb_intent_t intent = {.kind = ORB_INTENT_APP_COMMAND, .origin = ORB_INPUT_TOUCH,
        .physical = lv_indev_active() && !orb_input_is_synthetic(lv_indev_active()),
        .value.command = {.target = ORB_OTA_SCREEN_ID,
            .action = state->page == ORB_OTA_OFFER || state->page == ORB_OTA_FAILED ?
                ORB_SHELL_ACTION_UPDATE : ORB_SHELL_ACTION_CHECK_UPDATE}};
    /* The shell retains its online/physical guard. Acceptance can synchronously
     * retire the instance; no state is touched after the copied dispatch. */
    (void)orb_screen_emit(instance, &intent);
}
static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    ota_state_t *state = orb_screen_state(instance);
    lv_obj_t *root = orb_screen_root(instance);
    lv_obj_set_clickable(root, false);
    lv_obj_set_event_bubble(root, true);
    orb_screen_host_t *host = orb_screen_host(instance);
    orb_screen_update_t projection = {0};
    if (host->config.project && host->config.project(host->config.context, ORB_OTA_SCREEN_ID, &projection) &&
        projection.model && projection.model->data && projection.model->size == sizeof(state->source))
        memcpy(&state->source, projection.model->data, sizeof(state->source));
    orb_ota_scaffold_t port;
    if (!borrow(state, &port)) return false;
    /* Only these three controls have OTA lifetime. Direct siblings preserve
     * shared-label reparenting and exact foreground order on every rebuild. */
    state->title = orb_kit_rim_title_create(port.system, "Updates", ORB_KIT_CODEX);
    if (!state->title) return false;
    orb_screen_page_bind(instance, port.reading, state->title);
    state->art = orb_kit_disc_create(port.system, ORB_KIT_DISC_STANDARD,
        ORB_KIT_ACCENT_FILL, ORB_KIT_CODEX, ORB_ICON_DOWNLOAD);
    if (!state->art) return false;
    lv_obj_add_event_cb(state->art, owned_deleted, LV_EVENT_DELETE, state);
    lv_obj_set_clickable(state->art, false);
    state->action = orb_kit_button_primary_create(port.system, "Check now", ORB_KIT_CODEX);
    if (!state->action) return false;
    lv_obj_add_event_cb(state->action, owned_deleted, LV_EVENT_DELETE, state);
    lv_obj_set_event_bubble(state->action, true);
    lv_obj_add_event_cb(state->action, clicked, LV_EVENT_CLICKED, instance);
    state->action_shown = true;
    return true;
}
/* Design 1.0 coordinates are panel pixels. Semantic type replaces only the
 * unavailable font sizes; AX3 reflows the same hierarchy (HIG §§3/4/8). */
static int32_t design_line(const orb_ota_scaffold_t *port, line_t which, int32_t y, int32_t width,
                           lv_color_t color)
{
    lv_obj_t *obj = port->lines[which];
    if (lv_obj_get_parent(obj) != port->system) lv_obj_set_parent(obj, port->system);
    lv_obj_set_size(obj, width, LV_SIZE_CONTENT);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(obj, color, 0);
    lv_obj_update_layout(obj);
    int32_t height = lv_obj_get_height(obj);
    int32_t chord = orb_chord_width(y, y + height - 1, ORB_RIM_MARGIN_TEXT);
    if (chord < width) {
        width = LV_MAX(32, chord);
        lv_obj_set_width(obj, width);
        lv_obj_update_layout(obj);
        height = lv_obj_get_height(obj);
    }
    lv_obj_set_pos(obj, (466 - width) / 2, y);
    return y + height;
}

void orb_ota_screen_capture(orb_screen_instance_t *instance, orb_ota_page_t page)
{
    if (!orb_screen_alive(instance) || page > ORB_OTA_FAILED) return;
    ota_state_t *state = orb_screen_state(instance);
    orb_ota_scaffold_t port;
    if (borrow(state, &port) && lv_obj_is_visible(port.reading))
        state->scroll[page] = (int16_t)lv_obj_get_scroll_y(port.reading);
}
void orb_ota_screen_content(orb_screen_instance_t *instance, orb_ota_page_t page,
    const orb_shell_snapshot_t *m, bool pending)
{
    if (!orb_screen_alive(instance) || !m || page > ORB_OTA_FAILED) return;
    ota_state_t *state = orb_screen_state(instance);
    orb_ota_scaffold_t port;
    if (!borrow(state, &port)) return;
    bool waiting = (page == ORB_OTA_UPDATES && pending) || page == ORB_OTA_VERIFY || page == ORB_OTA_READY;
    orb_ota_wait_t *budget = port.wait ? port.wait : &state->wait;
    bool continuing = budget->waiting && budget->page == page;
    if (waiting && !continuing) *budget = (orb_ota_wait_t){lv_tick_get(), page, true};
    if (!waiting) budget->waiting = false;
    state->wait = *budget;
    if (state->page != page && state->ready_check) { lv_obj_delete(state->ready_check); state->ready_check = NULL; }
    if (page == ORB_OTA_READY && state->page != ORB_OTA_READY) {
        state->ready_check = orb_kit_check_create(port.system, orb_kit_accent_ink(ORB_KIT_CODEX), false);
        if (state->ready_check) {
            lv_obj_set_pos(state->ready_check, 214, 107);
            lv_obj_add_event_cb(state->ready_check, owned_deleted, LV_EVENT_DELETE, state);
            if (continuing) orb_kit_motion_finish(state->ready_check);
        }
    }
    if (page >= ORB_OTA_DOWNLOAD && page <= ORB_OTA_READY &&
        (state->page < ORB_OTA_DOWNLOAD || state->page > ORB_OTA_READY)) state->progress_set = false;
    state->page = page; state->online = online(m); state->pending = pending;
    char code[24] = "", body[192] = "", version[48] = "";
    switch (page) {
    case ORB_OTA_UPDATES:
        orb_system_software_version(version, sizeof(version), m->firmware_version);
        port.line(port.context, LINE_TITLE, "Updates", ORB_TEXT_TITLE3, ORB_TEXT_EMPHASIZED, 1);
        snprintf(body, sizeof(body), "Orb %s", version);
        port.line(port.context, LINE_SUBTITLE, body, ORB_TEXT_TITLE3, ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR, 2);
        port.line(port.context, LINE_BODY, !online(m) ? "Connect your Mac to check for updates." : pending ? "Checking for updates…" :
             m->update_check_result[0] ? m->update_check_result : "Check for software from your Mac.",
             ORB_TEXT_CAPTION2, ORB_TEXT_REGULAR, 3);
        if (!pending) port.primary_action(port.context, "Check now", ORB_SHELL_ACTION_CHECK_UPDATE, online(m));
        break;
    case ORB_OTA_OFFER:
        orb_system_software_version(version, sizeof(version), m->version);
        port.line(port.context, LINE_TITLE, "Update available", ORB_TEXT_HEADLINE, ORB_TEXT_EMPHASIZED, 1);
        snprintf(body, sizeof(body), "Orb %s", version);
        port.line(port.context, LINE_SUBTITLE, body, ORB_TEXT_TITLE3, ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR, 2);
        port.line(port.context, LINE_BODY, m->notes[0] ? m->notes : "Ready to install on Orb.", ORB_TEXT_BODY, ORB_TEXT_REGULAR, 2);
        if (m->battery_pct >= 0 && m->battery_pct < 30 && !m->charging) {
            port.line(port.context, LINE_DETAIL, "Charge Orb, or choose Update to continue.", ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, 2);
        }
        port.primary_action(port.context, "Update", ORB_SHELL_ACTION_UPDATE, online(m));
        break;
    case ORB_OTA_DOWNLOAD:
    case ORB_OTA_VERIFY:
    case ORB_OTA_READY:
        snprintf(code, sizeof(code), "%d%%", state->page == ORB_OTA_DOWNLOAD ? LV_CLAMP(0, m->progress, 100) : 100);
        orb_system_software_version(version, sizeof(version), m->version);
        snprintf(body, sizeof(body), "Updating to %s", version);
        port.line(port.context, LINE_TITLE, body, ORB_TEXT_FOOTNOTE1, ORB_TEXT_EMPHASIZED, 1);
        port.line(port.context, LINE_CODE, code, ORB_TEXT_TITLE1, ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR, 1);
        /* Review #22: one stage line carrying the version (AX3 dropped a separate
         * one); no "Downloading" at 100%, no "keep using" as ORB_OTA_READY restarts. */
        snprintf(body, sizeof(body), "%s %s", state->page == ORB_OTA_READY ? "Installing" : state->page == ORB_OTA_VERIFY ?
                 "Verifying" : m->progress >= 100 ? "Downloaded" : "Downloading", version);
        port.line(port.context, LINE_SUBTITLE, body, ORB_TEXT_FOOTNOTE2, ORB_TEXT_EMPHASIZED, 2);
        port.line(port.context, LINE_BODY, state->page == ORB_OTA_READY ? "Orb restarts now to finish." :
             "Keep using Orb.\nRestarts when ready.", ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, 2);
        break;
    case ORB_OTA_FAILED:
        port.line(port.context, LINE_TITLE, "Update interrupted", ORB_TEXT_HEADLINE, ORB_TEXT_EMPHASIZED, 1);
        port.line(port.context, LINE_SUBTITLE, "Update interrupted", ORB_TEXT_TITLE3, ORB_TEXT_EMPHASIZED, 2);
        port.line(port.context, LINE_BODY, m->error[0] ? m->error : "The update could not finish.", ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, 3);
        port.line(port.context, LINE_DETAIL, "Current software is safe.", ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, 2);
        port.primary_action(port.context, "Try again", ORB_SHELL_ACTION_UPDATE, online(m));
        break;
    }
    if (page >= ORB_OTA_DOWNLOAD && page <= ORB_OTA_READY)
        orb_ota_screen_progress(instance, page == ORB_OTA_DOWNLOAD ? m->progress : 100);
    orb_kit_breathe_since(port.lines[LINE_BODY], page == ORB_OTA_UPDATES && pending, state->wait.started);
    orb_kit_breathe_since(port.lines[LINE_SUBTITLE], page == ORB_OTA_VERIFY || page == ORB_OTA_READY, state->wait.started);
}

void orb_ota_screen_layout(orb_screen_instance_t *instance)
{
    if (!orb_screen_alive(instance)) return;
    ota_state_t *state = orb_screen_state(instance);
    orb_ota_scaffold_t port;
    if (!borrow(state, &port)) return;
    bool ax = orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3;
    bool progress = state->page == ORB_OTA_DOWNLOAD || state->page == ORB_OTA_VERIFY || state->page == ORB_OTA_READY;
    bool failed = state->page == ORB_OTA_FAILED;
    visible(port.art, false);
    visible(port.wifi, false);
    visible(port.battery, false);
    visible(port.power, false);
    visible(port.settings, false);
    visible(port.primary, false);
    visible(port.cancel, false);
    /* Review #22: failure has no progress; its red warning disc carries the state. */
    visible(port.ring, progress);
    /* A complete gauge owns the outer rim. The title baseline is inset so
     * its actual glyphs cannot cross that ring, including AX3/Bold Text. */
    orb_kit_rim_title_set_inset(state->title, progress);
    lv_obj_set_style_bg_color(state->art, failed ? ORB_RED : ORB_KIT_CODEX, 0);
    lv_obj_set_pos(state->art, 193, 84);
    visible(state->art, true);
    orb_kit_disc_set_glyph_hidden(state->art, state->ready_check != NULL);
    orb_kit_disc_set_glyph(state->art, failed ? ORB_ICON_WARNING : state->page == ORB_OTA_VERIFY ?
                      ORB_ICON_SHIELD_CHECK : state->page == ORB_OTA_READY ? ORB_ICON_CHECK : ORB_ICON_DOWNLOAD);
    orb_kit_place_bottom_arc(state->action);
    const char *caption = state->page == ORB_OTA_UPDATES ? "Check now" : failed ? "Try again" : "Update";
    orb_kit_button_set_text(state->action, caption);
    state->action_shown = !progress;
    visible(state->action, !progress);
    if (!state->online || state->pending) lv_obj_add_state(state->action, LV_STATE_DISABLED);
    else lv_obj_remove_state(state->action, LV_STATE_DISABLED);
    lv_obj_set_style_opa(state->action, LV_OPA_50, LV_STATE_DISABLED);
    int hero_bottom;
    if (progress) {
        hero_bottom = design_line(&port, LINE_CODE, ax ? 171 : 174, 354, ORB_LABEL);
        visible(port.lines[LINE_TITLE], false);
    } else {
        hero_bottom = design_line(&port, LINE_SUBTITLE, ax ? 174 : 186, 374, ORB_LABEL);
        visible(port.lines[LINE_TITLE], false);
    }
    /* Variable Mac results, release notes and failure text stay fully readable
     * in one bounded content viewport. The rim scrolls this visible reader. */
    visible(port.reading, true);
    int content_top = LV_MAX(ax ? 244 : 236, hero_bottom + 12);
    int content_bottom = progress ? 390 : 338;
    int content_width = progress ? 286 : 334;
    lv_obj_set_pos(port.reading, (466 - content_width) / 2, content_top);
    lv_obj_set_size(port.reading, content_width, LV_MAX(32, content_bottom - content_top));
    lv_obj_set_scrollbar_mode(port.reading, LV_SCROLLBAR_MODE_OFF);
    int y = 0;
    /* Review #2: an offer leads with the battery caution, then the notes. */
    const line_t copy[] = {LINE_SUBTITLE, state->page == ORB_OTA_OFFER ? LINE_DETAIL : LINE_BODY,
                           state->page == ORB_OTA_OFFER ? LINE_BODY : LINE_DETAIL};
    for (unsigned i = 0; i < sizeof(copy) / sizeof(*copy); i++) {
        if (!progress && copy[i] == LINE_SUBTITLE) continue;
        lv_obj_t *obj = port.lines[copy[i]];
        if (lv_obj_is_hidden(obj)) continue;
        lv_obj_set_parent(obj, port.reading);
        lv_obj_set_width(obj, content_width);
        lv_obj_set_height(obj, LV_SIZE_CONTENT);
        lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_color(obj, ORB_KIT_SECONDARY, 0);
        lv_obj_set_style_text_opa(obj, ORB_KIT_SECONDARY_OPA, 0);
        lv_obj_update_layout(obj);
        lv_obj_set_pos(obj, 0, y);
        y += lv_obj_get_height(obj) + 12;
    }
    lv_obj_set_height(port.reading, orb_kit_reader_whole_lines(port.reading, lv_obj_get_height(port.reading)));
    lv_obj_update_layout(port.reading);
    lv_obj_scroll_to_y(port.reading, state->scroll[state->page], LV_ANIM_OFF);
    lv_obj_move_foreground(state->title);
    lv_obj_move_foreground(state->action);
}

void orb_ota_screen_progress(orb_screen_instance_t *instance, int progress)
{
    if (!orb_screen_alive(instance)) return;
    ota_state_t *state = orb_screen_state(instance);
    orb_ota_scaffold_t port;
    if (!borrow(state, &port)) return;
    orb_kit_ring_set_value_label(port.ring, port.lines[LINE_CODE], "100%");
    orb_kit_ring_set_formatter(port.ring, progress_format, NULL);
    orb_kit_ring_set_value(port.ring, LV_CLAMP(0, progress, 100), state->progress_set);
    state->progress_set = true;
}
static void update(orb_screen_instance_t *instance, const orb_screen_update_t *value)
{
    /* Full rendering and progress-only delivery are distinct shell opportunities.
     * Framework staging/revisions must not add a fit or reset a reader. */
    (void)instance; (void)value;
}
static void destroy(orb_screen_instance_t *instance)
{
    ota_state_t *state = orb_screen_state(instance);
    /* Borrowed scaffold nodes can already be gone during parent deletion. */
    if (state->art) lv_obj_delete(state->art);
    if (state->action) lv_obj_delete(state->action);
    if (state->ready_check) lv_obj_delete(state->ready_check);
}
static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    if (capacity < sizeof(*context) + sizeof(ota_context_t)) return false;
    const ota_state_t *state = orb_screen_state(instance);
    /* Zero padding too: context validation rejects any nonzero hole byte. */
    ota_context_t value;
    memset(&value, 0, sizeof(value));
    value.wait_started = state->wait.started;
    value.wait_page = state->wait.page;
    value.waiting = state->wait.waiting;
    for (unsigned i = 0; i < 6; i++) value.scroll[i] = state->scroll[i];
    memcpy(context->payload, &value, sizeof(value));
    return true;
}
static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    ota_state_t *state = orb_screen_state(instance);
    ota_context_t value;
    memcpy(&value, context->payload, sizeof(value));
    for (unsigned i = 0; i < 6; i++) state->scroll[i] = (int16_t)value.scroll[i];
    if (value.wait_page > ORB_OTA_FAILED) return false;
    state->wait = (orb_ota_wait_t){value.wait_started, (orb_ota_page_t)value.wait_page, value.waiting};
    orb_ota_scaffold_t port;
    if (borrow(state, &port)) {
        if (port.wait) state->wait = *port.wait;
        orb_kit_breathe_since(port.lines[LINE_BODY], state->wait.waiting && state->wait.page == ORB_OTA_UPDATES, state->wait.started);
        orb_kit_breathe_since(port.lines[LINE_SUBTITLE], state->wait.waiting &&
            (state->wait.page == ORB_OTA_VERIFY || state->wait.page == ORB_OTA_READY), state->wait.started);
    }
    return true;
}
static orb_back_result_t back(orb_screen_instance_t *instance)
{ (void)instance; return ORB_BACK_LEAVE_SCREEN; }
static void rim_begin(orb_screen_instance_t *instance, orb_input_origin_t origin, bool physical)
{ (void)instance; (void)origin; (void)physical; }
static void rim_preview(orb_screen_instance_t *instance, float value)
{
    ota_state_t *state = orb_screen_state(instance);
    orb_ota_scaffold_t port;
    if (borrow(state, &port)) lv_obj_scroll_to_y(port.reading, (int32_t)lrintf(value), LV_ANIM_OFF);
}
static void rim_finish(orb_screen_instance_t *instance, float value, bool commit,
    orb_input_origin_t origin, bool physical)
{ (void)commit; (void)origin; (void)physical; rim_preview(instance, value); }
static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{
    ota_state_t *state = orb_screen_state(instance);
    orb_ota_scaffold_t port;
    if (!borrow(state, &port) || lv_obj_is_hidden(port.reading)) return false;
    lv_obj_update_layout(port.reading);
    int32_t top = lv_obj_get_scroll_y(port.reading);
    int32_t maximum = LV_MAX(0, top + lv_obj_get_scroll_bottom(port.reading));
    if (!maximum) return false;
    *binding = (orb_rim_binding_t){.property = 1, .subpage = state->page, .kind = ORB_RIM_SCROLL,
        .maximum = maximum, .value = top, .detent = 72, .units_per_turn = 900,
        .begin = rim_begin, .preview = rim_preview, .finish = rim_finish};
    return true;
}
static void visibility(orb_screen_instance_t *instance, orb_visibility_t value)
{
    ota_state_t *state = orb_screen_state(instance);
    bool shown = value == ORB_VISIBILITY_ACTIVE || orb_screen_host(instance)->config.preview;
    if (state->art) visible(state->art, shown);
    if (state->ready_check) visible(state->ready_check, shown);
    if (state->action) visible(state->action, shown && state->action_shown);
}
static void chrome(const orb_screen_instance_t *instance, orb_screen_chrome_t *value)
{
    const ota_state_t *state = orb_screen_state(instance);
    *value = (orb_screen_chrome_t){.title = "Updates",
        .inset_ring = state->page == ORB_OTA_DOWNLOAD || state->page == ORB_OTA_VERIFY || state->page == ORB_OTA_READY};
}
static const orb_context_field_t s_fields[] = {
    {0, 4, ORB_CONTEXT_I32}, {4, 4, ORB_CONTEXT_I32}, {8, 4, ORB_CONTEXT_I32},
    {12, 4, ORB_CONTEXT_I32}, {16, 4, ORB_CONTEXT_I32}, {20, 4, ORB_CONTEXT_I32},
    {offsetof(ota_context_t, wait_started), sizeof(uint32_t), ORB_CONTEXT_U32},
    {offsetof(ota_context_t, wait_page), sizeof(uint32_t), ORB_CONTEXT_U32},
    {offsetof(ota_context_t, waiting), sizeof(bool), ORB_CONTEXT_BOOL},
};
static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = ORB_OTA_SCREEN_ID, .ops = &s_ops, .state_bytes = sizeof(ota_state_t),
    .context_bytes = sizeof(orb_screen_context_t) + sizeof(ota_context_t), .context_version = 2,
    .context_fields = s_fields, .context_field_count = sizeof(s_fields) / sizeof(s_fields[0]),
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(ota_state_t),
        .context_bytes = sizeof(orb_screen_context_t) + sizeof(ota_context_t), .declared = true},
    .subscriptions = ORB_DOMAIN_DEVICE, .privacy = ORB_PRIVACY_PRIVATE,
    .always_on = ORB_AOD_HIDDEN, .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN,
};
const orb_screen_descriptor_t *orb_ota_descriptor(void) { return &s_descriptor; }
static bool allow_synthetic(const orb_intent_t *intent)
{ return intent->kind == ORB_INTENT_APP_COMMAND && intent->value.command.target == ORB_OTA_SCREEN_ID; }
static void finish(orb_screen_instance_t *instance, bool commit) { (void)instance; (void)commit; }
orb_app_host_t *orb_ota_host_create(bool preview,
    bool (*command)(void *, orb_screen_instance_t *, const orb_intent_t *), void *context)
{
    return orb_app_host_create(preview, &(orb_app_host_config_t){.descriptor = orb_ota_descriptor,
        .name = "Installing", .model_bytes = sizeof(orb_ota_source_t), .command = command,
        .context = context, .allow_synthetic = allow_synthetic, .rim_finish = finish});
}
bool orb_ota_host_ensure(orb_app_host_t *host, lv_obj_t *parent, const orb_ota_source_t *source)
{
    if (orb_app_host_current(host)) return true;
    return source && orb_app_host_set_model(host, source, sizeof(*source)) && orb_app_host_attach(host, parent);
}
