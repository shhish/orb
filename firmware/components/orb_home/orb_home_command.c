#include "orb_home_command.h"

bool orb_home_command_execute(const orb_home_command_t *command)
{
    if (!command || !command->id) return false;
    switch (command->kind) {
    case ORB_HOME_COMMAND_LEVEL:
    case ORB_HOME_COMMAND_SET: return orb_home_set(command->id, &command->values, command->deliberate_unlock);
    case ORB_HOME_COMMAND_TOGGLE: return orb_home_toggle(command->id);
    case ORB_HOME_COMMAND_SCENE: return orb_home_scene(command->id);
    case ORB_HOME_COMMAND_ROOM_OFF: return orb_home_room_off(command->id);
    }
    return false;
}

bool orb_home_command(const orb_home_commands_t *port, const orb_home_command_t *command)
{
    return port && port->send ? port->send(port->context, command) : orb_home_command_execute(command);
}
