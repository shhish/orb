#include "orb_screen_private.h"

uint64_t orb_screen_event(orb_screen_host_t *host)
{
    if (!host || host->event_sequence == UINT64_MAX) return 0;
    size_t slot = host->event_sequence % ORB_SCREEN_FEEDBACK_HISTORY;
    /* Never evict an outstanding semantic event. A full ledger applies
     * backpressure instead of allowing a replay or losing a late completion. */
    if (host->event_ids[slot] && !host->event_consumed[slot]) return 0;
    uint64_t event = ++host->event_sequence;
    host->event_ids[slot] = event;
    host->event_consumed[slot] = false;
    return event;
}

bool orb_screen_feedback(orb_screen_instance_t *instance, uint64_t event,
                         orb_feedback_role_t role, orb_input_origin_t origin, bool committed)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    if (!data || !event) return false;
    orb_screen_host_t *host = data->host;
    size_t slot = (event - 1) % ORB_SCREEN_FEEDBACK_HISTORY;
    if (host->event_ids[slot] != event || host->event_consumed[slot]) return false;
    /* A cancelled/silent event is retired as well. Later model reconciliation
     * cannot turn its old preview ID into a new audible action. */
    host->event_consumed[slot] = true;
    if (data->destroying || data->preview || data->silent || !committed ||
        data->visibility != ORB_VISIBILITY_ACTIVE || role < ORB_CUE_OPEN || role > ORB_CUE_ERROR ||
        (origin != ORB_INPUT_TOUCH && origin != ORB_INPUT_CROWN && origin != ORB_INPUT_BUTTON) ||
        data->descriptor->feedback == ORB_FEEDBACK_SILENT) return false;
    if (host->config.cue) host->config.cue(host->config.context, role);
    return true;
}

bool orb_screen_emit(orb_screen_instance_t *instance, const orb_intent_t *intent)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    if (!data || !intent || data->destroying || data->preview || data->silent ||
        data->visibility != ORB_VISIBILITY_ACTIVE || !data->host->config.intent ||
        intent->kind < ORB_INTENT_NAVIGATE || intent->kind > ORB_INTENT_HOME_COMMAND) return false;
    orb_intent_t copy = *intent;
    if (data->rim_callback) {
        copy.origin = data->origin;
        copy.physical = data->physical;
        if (copy.kind != ORB_INTENT_HOME_SET_LEVEL && copy.kind != ORB_INTENT_SETTING_SET) return false;
    }
    if (copy.origin != ORB_INPUT_TOUCH && copy.origin != ORB_INPUT_CROWN &&
        copy.origin != ORB_INPUT_BUTTON) return false;
    if (copy.origin == ORB_INPUT_CROWN && copy.kind != ORB_INTENT_HOME_SET_LEVEL &&
        copy.kind != ORB_INTENT_SETTING_SET) return false;
    if (copy.kind != ORB_INTENT_NAVIGATE && !copy.physical &&
        (!data->host->config.allow_synthetic ||
         !data->host->config.allow_synthetic(data->host->config.context, &copy))) {
        if (data->host->config.synthetic_refused)
            data->host->config.synthetic_refused(data->host->config.context, &copy);
        return false;
    }
    return data->host->config.intent(data->host->config.context, &copy);
}
