#include "orb_agents_copy.h"

#include "orb_fonts.h"

/* Provider identity and session state share one vocabulary across the shell,
 * the glance, the session list and usage, independent of workspace state. */
const char *agents_icon(orb_agent_t agent)
{
    return agent == ORB_AGENT_CLAUDE ? ORB_ICON_PROMPT :
           agent == ORB_AGENT_CODEX ? ORB_ICON_CODE : ORB_ICON_CHAT;
}

const char *agents_state_text(orb_session_state_t state)
{
    static const char *names[] = { "Working", "Needs you", "Idle", "Done", "Error" };
    return state <= ORB_SESSION_ERROR ? names[state] : "Unknown";
}

