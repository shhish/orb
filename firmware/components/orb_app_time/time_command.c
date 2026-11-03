/* Time service changes as typed, value-only screen intents. The host maps
 * each back to the unchanged orb_time setter and returns its result. */
#include "orb_app_time.h"
#include "time_command.h"
#include "orb_input.h"

bool time_command_send(void *context, const orb_time_command_t *command)
{
    if (!command) return false;
    /* Time's rim stays with the shell's own handoff, so even its value edits
     * arrive here as presenter actions. */
    orb_intent_t intent = {.kind = ORB_INTENT_APP_COMMAND, .origin = ORB_INPUT_TOUCH,
        .physical = !orb_input_active_is_synthetic()};
    intent.value.command.target = (uint64_t)command->at;
    intent.value.command.action = command->kind;
    intent.value.command.value = (int32_t)command->a;
    intent.value.command.extra = (int32_t)command->b;
    return orb_screen_emit(context, &intent);
}

bool orb_app_time_command_from_intent(const orb_intent_t *intent, orb_time_command_t *command)
{
    if (!intent || !command || intent->kind != ORB_INTENT_APP_COMMAND ||
        intent->value.command.action < ORB_TIME_COMMAND_TIMER_SET ||
        intent->value.command.action > ORB_TIME_COMMAND_PERSIST_RETRY) return false;
    *command = (orb_time_command_t){.kind = (orb_time_command_kind_t)intent->value.command.action,
        .at = (int64_t)intent->value.command.target, .a = (uint32_t)intent->value.command.value,
        .b = (uint32_t)intent->value.command.extra};
    return true;
}
