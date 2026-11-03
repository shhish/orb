#include "orb_home_host.h"
#include "orb_shell.h"

#include <string.h>

/* Writes keep the service's own bounded queue, validation and ordering: the
 * request runs now and its acceptance returns to the presenter. */
static bool execute(const orb_intent_t *intent)
{
    orb_home_command_t command;
    return orb_app_home_command_from_intent(intent, &command) && orb_home_command_execute(&command);
}

orb_app_host_t *orb_home_host_create(bool preview, const orb_home_host_shell_t *shell)
{
    orb_home_host_shell_t hooks = shell ? *shell : (orb_home_host_shell_t){0};
    return orb_app_host_create(preview, &(orb_app_host_config_t){
        .descriptor = orb_app_home_descriptor, .name = "Home", .intent = execute, .rim_finish = orb_app_home_rotary_finish, .transition = hooks.transition,
        .nav = hooks.nav, .input = hooks.input, .context = hooks.context});
}

bool orb_home_host_attach(orb_app_host_t *host, lv_obj_t *page, const orb_app_home_model_t *model)
{
    if (!model || !orb_app_host_set_model(host, model, sizeof(*model))) return false;
    bool attached = orb_app_host_attach(host, page);
    orb_app_home_model_t retained = *model;
    retained.fixture = NULL;
    (void)orb_app_host_set_model(host, &retained, sizeof(retained));
    if (attached) orb_app_home_arrival_prepare(orb_app_host_current(host));
    return attached;
}

void orb_home_host_environment(orb_app_host_t *host, unsigned hour, bool contrast)
{
    size_t size;
    const void *current = orb_app_host_model(host, &size);
    if (!orb_app_host_current(host) || size != sizeof(orb_app_home_model_t)) return;
    orb_app_home_model_t model;
    memcpy(&model, current, sizeof(model));
    model.hour = hour;
    model.contrast = contrast;
    if (orb_app_host_set_model(host, &model, sizeof(model))) orb_app_host_environment(host);
}

void orb_home_host_departure(orb_app_host_t *host, orb_screen_id_t destination,
                             bool ordinary, bool *fresh_entry)
{
    /* Sheets and voice restore their interrupted Home context. A committed
     * ordinary place makes the next Home entry start at its list top. */
    if (!ordinary || !host || !fresh_entry || destination == ORB_SHELL_HOME ||
        destination == ORB_SHELL_STACK || destination == ORB_SHELL_NOTIFICATIONS ||
        destination == ORB_SHELL_CONTROL_CENTER || destination == ORB_SHELL_VOICE) return;
    *fresh_entry = true;
}

void orb_home_host_enter(orb_app_host_t *host, bool *fresh_entry)
{
    if (!fresh_entry || !*fresh_entry) return;
    orb_app_home_enter(orb_app_host_current(host));
    *fresh_entry = false;
}
