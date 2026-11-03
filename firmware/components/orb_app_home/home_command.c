/* Home writes as typed, value-only screen intents. The presenter has already
 * applied its physical-input and availability guards; the host maps the
 * intent back to the unchanged service request and returns its result. */
#include "orb_app_home.h"
#include "home_command.h"
#include "orb_input.h"

#include <stdio.h>
#include <string.h>

/* Every LEVEL/SET command names exactly one writable characteristic. */
static bool field_value(const orb_home_state_t *values, float *value)
{
    switch (values->present) {
    case ORB_HOME_ON: *value = values->on; return true;
    case ORB_HOME_BRIGHTNESS: *value = values->brightness; return true;
    case ORB_HOME_HUE: *value = values->hue; return true;
    case ORB_HOME_SAT: *value = values->sat; return true;
    case ORB_HOME_CT: *value = values->ct; return true;
    case ORB_HOME_TARGET: *value = values->target; return true;
    case ORB_HOME_MODE: *value = (float)values->mode; return true;
    case ORB_HOME_LOCKED: *value = values->locked; return true;
    case ORB_HOME_SPEED: *value = values->speed; return true;
    case ORB_HOME_POSITION: *value = values->position; return true;
    default: return false;
    }
}

static bool field_state(uint32_t field, float value, orb_home_state_t *values)
{
    *values = (orb_home_state_t){.present = field};
    switch (field) {
    case ORB_HOME_ON: values->on = value != 0; return true;
    case ORB_HOME_BRIGHTNESS: values->brightness = value; return true;
    case ORB_HOME_HUE: values->hue = value; return true;
    case ORB_HOME_SAT: values->sat = value; return true;
    case ORB_HOME_CT: values->ct = value; return true;
    case ORB_HOME_TARGET: values->target = value; return true;
    case ORB_HOME_MODE: values->mode = (orb_home_mode_t)(int)value; return true;
    case ORB_HOME_LOCKED: values->locked = value != 0; return true;
    case ORB_HOME_SPEED: values->speed = value; return true;
    case ORB_HOME_POSITION: values->position = value; return true;
    default: return false;
    }
}

static bool carries_values(orb_home_command_kind_t kind)
{
    return kind == ORB_HOME_COMMAND_LEVEL || kind == ORB_HOME_COMMAND_SET;
}

bool home_command_send(void *context, const orb_home_command_t *command)
{
    orb_screen_instance_t *instance = context;
    if (!command || !command->id || strlen(command->id) >= ORB_INTENT_ID_SIZE) return false;
    /* Only a dial value may also arrive through the rim lease; discrete
     * writes, unlock and scene runs stay touch actions. */
    orb_intent_t intent = {.kind = command->kind == ORB_HOME_COMMAND_LEVEL ?
        ORB_INTENT_HOME_SET_LEVEL : ORB_INTENT_HOME_COMMAND,
        .origin = ORB_INPUT_TOUCH, .physical = !orb_input_active_is_synthetic()};
    snprintf(intent.value.home.id, sizeof(intent.value.home.id), "%s", command->id);
    intent.value.home.action = command->kind;
    intent.value.home.deliberate = command->deliberate_unlock;
    if (carries_values(command->kind)) {
        intent.value.home.field = command->values.present;
        if (!field_value(&command->values, &intent.value.home.value)) return false;
    }
    return orb_screen_emit(instance, &intent);
}

bool orb_app_home_command_from_intent(const orb_intent_t *intent, orb_home_command_t *command)
{
    if (!intent || !command || (intent->kind != ORB_INTENT_HOME_SET_LEVEL && intent->kind != ORB_INTENT_HOME_COMMAND) ||
        !memchr(intent->value.home.id, 0, sizeof(intent->value.home.id))) return false;
    orb_home_command_kind_t kind = (orb_home_command_kind_t)intent->value.home.action;
    if (kind < ORB_HOME_COMMAND_LEVEL || kind > ORB_HOME_COMMAND_ROOM_OFF ||
        (intent->kind == ORB_INTENT_HOME_SET_LEVEL) != (kind == ORB_HOME_COMMAND_LEVEL)) return false;
    *command = (orb_home_command_t){.kind = kind, .id = intent->value.home.id,
        .deliberate_unlock = intent->value.home.deliberate};
    return !carries_values(kind) || field_state(intent->value.home.field, intent->value.home.value, &command->values);
}
