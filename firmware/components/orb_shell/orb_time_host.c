#include "orb_time_host.h"

/* Service changes keep orb_time's own validation and ordering: the setter
 * runs now and its result returns to the presenter. */
static bool execute(const orb_intent_t *intent)
{
    orb_time_command_t command;
    return orb_app_time_command_from_intent(intent, &command) && orb_time_command_execute(&command);
}

/* Time's presenter has never required physical input for its actions (a
 * recorded finding); migration preserves that acceptance unchanged. */
static bool allow_synthetic(const orb_intent_t *intent)
{
    orb_time_command_t command;
    return orb_app_time_command_from_intent(intent, &command);
}

orb_app_host_t *orb_time_host_create(bool preview)
{
    return orb_app_host_create(preview, &(orb_app_host_config_t){
        .descriptor = orb_app_time_descriptor, .name = "Time", .intent = execute,
        .allow_synthetic = allow_synthetic, .rim_finish = orb_app_time_rotary_finish});
}

bool orb_time_host_present(orb_app_host_t **host, lv_obj_t *layer)
{
    orb_app_host_destroy(*host);
    *host = orb_time_host_create(false);
    if (orb_app_host_set_model(*host, &(orb_app_time_model_t){0}, sizeof(orb_app_time_model_t)) &&
        orb_app_host_attach(*host, layer)) return true;
    orb_app_host_destroy(*host);
    *host = NULL;
    return false;
}

bool orb_time_host_attach(orb_app_host_t *host, lv_obj_t *page, const orb_app_time_model_t *model,
    bool alarm_ringing, bool reminder_due)
{
    if (!model || !orb_app_host_set_model(host, model, sizeof(*model)) || !orb_app_host_attach(host, page))
        return false;
    orb_app_time_enter(orb_app_host_current(host), alarm_ringing, reminder_due);
    return true;
}
