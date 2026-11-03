#include "orb_play_host.h"
#include "orb_games.h"

/* Registry-only fixtures have no runtime/storage provider. */
__attribute__((weak)) void orb_games_progress(orb_game_kind_t kind, char *text, size_t capacity)
{
    (void)kind;
    if (text && capacity) text[0] = '\0';
}

static bool allow_synthetic(const orb_intent_t *intent)
{
    return intent->kind == ORB_INTENT_APP_COMMAND && intent->value.command.target == ORB_APP_PLAY_SCREEN_ID &&
        intent->value.command.action == ORB_APP_PLAY_LAUNCH &&
        (unsigned)intent->value.command.value < ORB_GAME_COUNT;
}

orb_app_host_t *orb_play_host_create(bool preview, void *context,
    bool (*command)(void *, orb_screen_instance_t *, const orb_intent_t *))
{
    return orb_app_host_create(preview, &(orb_app_host_config_t){
        .descriptor = orb_app_play_descriptor, .name = "Play", .model_bytes = sizeof(orb_app_play_model_t),
        .command = command, .allow_synthetic = allow_synthetic, .context = context,
        .rim_finish = orb_app_play_rotary_finish});
}

bool orb_play_host_attach(orb_app_host_t *host, lv_obj_t *page, bool system_input,
    bool (*input_allowed)(void *), void *context)
{
    orb_app_play_model_t model = {.system_input = system_input, .input_allowed = input_allowed, .context = context};
    for (unsigned i = 0; i < ORB_GAME_COUNT; i++) orb_games_progress(i, model.progress[i], sizeof(model.progress[i]));
    return orb_app_host_set_model(host, &model, sizeof(model)) && orb_app_host_attach(host, page);
}
