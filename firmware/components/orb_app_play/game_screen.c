/* The runtime supplies its existing UI construction/cleanup at a port. This
 * descriptor adds explicit lifetime ownership without scheduling a frame or
 * reading the main-owned simulation. */
#include "orb_app_play.h"
#include "orb_games_presentation.h"
#include "orb_memory.h"

#include <string.h>

typedef struct {
    orb_screen_host_t host;
    orb_screen_instance_t instance;
    orb_games_view_port_t view;
    orb_screen_model_t projection;
} game_presentation_t;

typedef struct { orb_games_view_port_t view; } game_state_t;

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    game_state_t *state = orb_screen_state(instance);
    orb_screen_host_t *host = orb_screen_host(instance);
    orb_screen_update_t projection = {0};
    /* A running presentation cannot borrow a live runtime for a preview.
     * Seeded previews remain the existing disposable miniature presenter. */
    if (orb_screen_visibility_get(instance) != ORB_VISIBILITY_ACTIVE || !host->config.project ||
        !host->config.project(host->config.context, ORB_APP_PLAY_GAME_SCREEN_ID, &projection) ||
        !projection.model || !projection.model->data || projection.model->size != sizeof(state->view)) return false;
    memcpy(&state->view, projection.model->data, sizeof(state->view));
    if (!state->view.create || !state->view.destroy) {
        memset(&state->view, 0, sizeof(state->view));
        return false;
    }
    lv_obj_t *root = orb_screen_root(instance);
    lv_obj_set_clickable(root, false);
    /* The old game root consumed its own contacts. Keep the wrapper inert. */
    return state->view.create(root, state->view.context);
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *value)
{
    /* Published rendering copies and their exact timer/TE clock remain owned
     * by the runtime; framework revisions never schedule a second producer. */
    (void)instance; (void)value;
}

static void destroy(orb_screen_instance_t *instance)
{
    game_state_t *state = orb_screen_state(instance);
    if (state->view.destroy) state->view.destroy(state->view.context);
    memset(&state->view, 0, sizeof(state->view));
}

static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    (void)instance; (void)context;
    /* Simulation/pause state is retained in main, never serialized or replayed. */
    return capacity >= sizeof(*context);
}

static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    (void)instance; (void)context;
    return true;
}

static orb_back_result_t back(orb_screen_instance_t *instance)
{
    /* BOOT's runtime pause/exit mailbox remains the shell's physical policy. */
    (void)instance;
    return ORB_BACK_LEAVE_SCREEN;
}

static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{
    (void)instance; (void)binding;
    return false;
}

static void visibility(orb_screen_instance_t *instance, orb_visibility_t value)
{
    /* Covering never changes pause ownership here. Main applies the shell's
     * existing lock/sheet/voice pause request exactly once. */
    (void)instance; (void)value;
}

static void chrome(const orb_screen_instance_t *instance, orb_screen_chrome_t *value)
{
    (void)instance;
    *value = (orb_screen_chrome_t){.title = "Play"};
}

static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = ORB_APP_PLAY_GAME_SCREEN_ID, .parent_id = ORB_APP_PLAY_SCREEN_ID, .ops = &s_ops,
    .state_bytes = sizeof(game_state_t), .context_bytes = sizeof(orb_screen_context_t), .context_version = 1,
    .memory = {.owner = ORB_MEMORY_GAMES, .state_bytes = sizeof(game_state_t),
        .context_bytes = sizeof(orb_screen_context_t), .declared = true},
    .privacy = ORB_PRIVACY_PRIVATE, .always_on = ORB_AOD_HIDDEN,
    .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN,
};

const orb_screen_descriptor_t *orb_app_play_game_descriptor(void) { return &s_descriptor; }

static bool project(void *context, orb_screen_id_t id, orb_screen_update_t *update)
{
    game_presentation_t *presentation = context;
    if (id != ORB_APP_PLAY_GAME_SCREEN_ID) return false;
    presentation->projection = (orb_screen_model_t){&presentation->view, sizeof(presentation->view)};
    *update = (orb_screen_update_t){.model = &presentation->projection};
    return true;
}

static void *open(lv_obj_t *parent, const orb_games_view_port_t *view)
{
    if (!parent || !view || !view->create || !view->destroy) return NULL;
    game_presentation_t *presentation = orb_memory_calloc(ORB_MEMORY_GAMES, 1, sizeof(*presentation));
    if (!presentation) return NULL;
    presentation->view = *view;
    orb_screen_host_init(&presentation->host, &(orb_screen_host_config_t){
        .context = presentation, .project = project, .defer_rim = true});
    if (!orb_screen_create(&presentation->instance, &presentation->host, &s_descriptor, parent, NULL, false)) {
        /* Entered-create failure already ran the descriptor's cleanup. */
        orb_memory_free(presentation);
        return NULL;
    }
    return presentation;
}

static void close(void *context)
{
    game_presentation_t *presentation = context;
    if (!presentation) return;
    orb_screen_destroy(&presentation->instance);
    orb_memory_free(presentation);
}

bool orb_app_play_init(void)
{
    static const orb_games_presentation_t s_port = {.open = open, .close = close};
    return orb_games_set_presentation(&s_port);
}
