/* Spoken reply retains the original canvas, caption viewport and controls.
 * Audio ownership and shell layer precedence remain outside presentation. */
#include "orb_app_voice_spoken.h"

#include "orb_fonts.h"
#include "orb_input.h"
#include "orb_kit.h"
#include "orb_kit_effects.h"
#include "orb_model.h"
#include "orb_ui.h"

#include <string.h>

typedef struct {
    lv_obj_t *caption, *header, *disc;
    int agent, font_epoch;
    bool waiting;
    uint32_t wait_started;
} spoken_state_t;

struct orb_app_voice_spoken {
    orb_screen_host_t host;
    orb_screen_instance_t instance;
    orb_app_voice_spoken_stop_cb_t stop;
    void *stop_context;
    uint64_t revision;
    bool active, preview;
};

static void stop_clicked(lv_event_t *event)
{
    orb_screen_instance_t *instance = lv_event_get_user_data(event);
    orb_screen_host_t *host = orb_screen_host(instance);
    if (!host || host->config.preview || !lv_indev_active() ||
        !orb_input_require_physical(lv_event_get_target_obj(event))) return;
    orb_intent_t intent = {.kind = ORB_INTENT_APP_COMMAND,
        .origin = ORB_INPUT_TOUCH, .physical = true,
        .value.command = {.target = ORB_APP_VOICE_SPOKEN_SCREEN_ID,
            .action = ORB_APP_VOICE_SPOKEN_STOP}};
    (void)orb_screen_emit(instance, &intent);
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    spoken_state_t *state = orb_screen_state(instance);
    lv_obj_t *root = orb_screen_root(instance);
    lv_obj_set_style_radius(root, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    /* Modal input must never pass through the caption layer to Send. */
    lv_obj_set_clickable(root, true);
    state->agent = -2;
    state->header = orb_kit_rim_title_create(root, "Spoken reply", ORB_KIT_DOT);
    state->disc = orb_kit_disc_create(root, ORB_KIT_DISC_STANDARD,
        ORB_KIT_ACCENT_FILL, ORB_KIT_DOT, ORB_ICON_CHAT);
    lv_obj_set_clickable(state->disc, false);
    lv_obj_set_pos(state->disc, 193, 84);

    lv_obj_t *scroll = lv_obj_create(root);
    lv_obj_remove_style_all(scroll);
    lv_obj_set_size(scroll, 330, 132);
    lv_obj_set_pos(scroll, 68, 180);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_OFF);
    orb_kit_scroll_arc_attach(scroll);
    /* Captions dissolve beneath the fixed Stop control. */
    orb_scroll_edge_follow(scroll, 12, 20);
    state->caption = lv_label_create(scroll);
    orb_text_apply(state->caption, ORB_TEXT_CAPTION2, ORB_TEXT_REGULAR, 0);
    lv_obj_set_width(state->caption, 330);
    lv_obj_set_style_text_color(state->caption, ORB_LABEL, 0);
    lv_obj_set_style_text_align(state->caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(state->caption, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *stop = orb_kit_glyph_button_create(root, ORB_ICON_STOP_FILL, false, ORB_KIT_DOT);
    lv_obj_set_pos(stop, 193, 314);
    lv_obj_add_event_cb(stop, stop_clicked, LV_EVENT_CLICKED, instance);
    orb_kit_rim_caption_create(root, "Talk or double-tap to stop");
    return true;
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *update)
{
    if (!update->model || !update->model->data ||
        update->model->size != sizeof(orb_app_voice_spoken_model_t)) return;
    spoken_state_t *state = orb_screen_state(instance);
    const orb_app_voice_spoken_model_t *model = update->model->data;
    int font_epoch = lv_subject_get_int(orb_fonts_subject());
    /* Polling unchanged metadata reuses the retained rim rasters. */
    if (state->agent != model->agent || state->font_epoch != font_epoch) {
        state->agent = model->agent;
        state->font_epoch = font_epoch;
        /* Rest on a complete caption line, leaving the fixed Stop clear. */
        int line_height = lv_obj_get_style_text_font(state->caption, 0)->line_height;
        int spacing = lv_obj_get_style_text_line_space(state->caption, 0);
        int lines = LV_MAX(1, (132 + spacing) / (line_height + spacing));
        lv_obj_set_height(lv_obj_get_parent(state->caption), lines * (line_height + spacing) - spacing);
        bool known = model->agent >= ORB_AGENT_CLAUDE && model->agent <= ORB_AGENT_DOT;
        const char *name = model->agent == ORB_AGENT_CLAUDE ? "Claude" :
            model->agent == ORB_AGENT_CODEX ? "Codex" : "Dot";
        orb_kit_rim_text_set(state->header, known ? name : "Spoken reply", NULL);
        lv_obj_set_hidden(state->disc, !known);
        lv_color_t accent = model->agent == ORB_AGENT_CLAUDE ? ORB_KIT_CLAUDE :
            model->agent == ORB_AGENT_CODEX ? ORB_KIT_CODEX : ORB_KIT_DOT;
        lv_obj_set_style_bg_color(state->disc, accent, 0);
        orb_kit_disc_set_ink(state->disc, orb_kit_accent_ink(accent));
        orb_kit_disc_set_glyph(state->disc, model->agent == ORB_AGENT_CLAUDE ? ORB_ICON_PROMPT :
            model->agent == ORB_AGENT_CODEX ? ORB_ICON_CODE : ORB_ICON_CHAT);
    }
    const char *value = model->caption && *model->caption ? model->caption :
        model->buffering ? "Buffering…" : "Waiting for captions…";
    if (strcmp(lv_label_get_text(state->caption), value)) lv_label_set_text(state->caption, value);
    bool waiting = !model->caption || !*model->caption;
    /* Buffering and waiting-for-captions are one uninterrupted audio wait. */
    if (waiting && !state->waiting) state->wait_started = lv_tick_get();
    state->waiting = waiting;
    orb_kit_breathe_since(state->caption, waiting, state->wait_started);
}

static void destroy(orb_screen_instance_t *instance)
{
    /* Descendants and their callbacks are released by the framework root. */
    (void)instance;
}

static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    (void)instance; (void)context;
    return capacity >= sizeof(orb_screen_context_t);
}

static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    /* Playback state and pending commands never enter a saved context. */
    (void)instance; (void)context;
    return true;
}

static orb_back_result_t back(orb_screen_instance_t *instance)
{
    (void)instance;
    return ORB_BACK_CLOSE_LAYER;
}

static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{
    (void)instance; (void)binding;
    return false;
}

static void visibility(orb_screen_instance_t *instance, orb_visibility_t value)
{
    /* Hiding presentation neither stops playback nor discards its caption. */
    (void)instance; (void)value;
}

static void chrome(const orb_screen_instance_t *instance, orb_screen_chrome_t *value)
{
    (void)instance;
    *value = (orb_screen_chrome_t){.title = "Spoken reply"};
}

static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = ORB_APP_VOICE_SPOKEN_SCREEN_ID, .parent_id = ORB_SCREEN_INVALID_ID, .ops = &s_ops,
    .state_bytes = sizeof(spoken_state_t), .context_bytes = sizeof(orb_screen_context_t),
    .context_version = 1,
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(spoken_state_t),
        .context_bytes = sizeof(orb_screen_context_t), .declared = true},
    .subscriptions = ORB_DOMAIN_MODEL, .privacy = ORB_PRIVACY_PRIVATE,
    .always_on = ORB_AOD_HIDDEN, .feedback = ORB_FEEDBACK_SILENT, .nav_role = ORB_NAV_ROLE_SCREEN,
};

const orb_screen_descriptor_t *orb_app_voice_spoken_descriptor(void) { return &s_descriptor; }

static bool accept(void *context, const orb_intent_t *intent)
{
    orb_app_voice_spoken_t *view = context;
    return intent->kind == ORB_INTENT_APP_COMMAND &&
        intent->value.command.target == ORB_APP_VOICE_SPOKEN_SCREEN_ID &&
        intent->value.command.action == ORB_APP_VOICE_SPOKEN_STOP && orb_app_voice_spoken_stop(view);
}

orb_app_voice_spoken_t *orb_app_voice_spoken_create(bool preview,
    orb_app_voice_spoken_stop_cb_t stop, void *context)
{
    orb_app_voice_spoken_t *view = orb_memory_calloc(ORB_MEMORY_WORKSPACE, 1, sizeof(*view));
    if (!view) return NULL;
    view->preview = preview;
    view->stop = stop;
    view->stop_context = context;
    orb_screen_host_init(&view->host, &(orb_screen_host_config_t){
        .context = view, .intent = accept, .preview = preview, .defer_rim = true});
    return view;
}

void orb_app_voice_spoken_destroy(orb_app_voice_spoken_t *view)
{
    if (!view) return;
    orb_screen_destroy(&view->instance);
    orb_memory_free(view);
}

void orb_app_voice_spoken_set_stop(orb_app_voice_spoken_t *view,
    orb_app_voice_spoken_stop_cb_t stop, void *context)
{
    if (!view) return;
    view->stop = stop;
    view->stop_context = context;
}

bool orb_app_voice_spoken_present(orb_app_voice_spoken_t *view, lv_display_t *display,
    bool active, bool buffering, const char *caption, int agent)
{
    if (!view) return false;
    view->active = active;
    if (!active) {
        orb_screen_destroy(&view->instance);
        return true;
    }
    if (!display) return false;
    if (!orb_screen_alive(&view->instance) && !orb_screen_create(&view->instance, &view->host,
        &s_descriptor, lv_display_get_layer_top(display), NULL, view->preview)) return false;
    orb_app_voice_spoken_model_t model = {.caption = caption, .agent = agent, .buffering = buffering};
    orb_screen_model_t projection = {&model, sizeof(model)};
    return orb_screen_update(&view->instance, &(orb_screen_update_t){
        .revisions.model = ++view->revision, .model = &projection});
}

void orb_app_voice_spoken_show(orb_app_voice_spoken_t *view, bool visible)
{
    if (!view) return;
    orb_screen_visibility(&view->instance, visible ? ORB_VISIBILITY_ACTIVE : ORB_VISIBILITY_COVERED);
}

lv_obj_t *orb_app_voice_spoken_root(const orb_app_voice_spoken_t *view)
{
    return view ? orb_screen_root(&view->instance) : NULL;
}

bool orb_app_voice_spoken_stop(orb_app_voice_spoken_t *view)
{
    return view && !view->preview && view->active && view->stop && view->stop(view->stop_context);
}

size_t orb_app_voice_spoken_size(void) { return sizeof(orb_app_voice_spoken_t); }
