#include "voice_internal.h"

orb_voice_flow_t *orb_voice_flow_create(void)
{ return orb_memory_calloc(ORB_MEMORY_WORKSPACE, 1, sizeof(orb_voice_flow_t)); }
void orb_voice_flow_destroy(orb_voice_flow_t *flow) { orb_memory_free(flow); }
size_t orb_voice_flow_bytes(void) { return sizeof(orb_voice_flow_t); }
bool orb_voice_flow_pending(const orb_voice_flow_t *flow) { return flow && flow->voice_pending; }
bool orb_voice_flow_held(const orb_voice_flow_t *flow) { return flow && flow->mic_held; }
uint32_t orb_voice_flow_sent_at(const orb_voice_flow_t *flow) { return flow ? flow->voice_sent_at : 0; }
void orb_voice_flow_depart(orb_voice_flow_t *flow) { if (flow) flow->voice_sent_at = 0; }
void orb_voice_flow_retry(orb_voice_flow_t *flow)
{ if (flow) { flow->voice_retry = true; flow->voice_sent_at = 0; } }
void orb_voice_flow_sent(orb_voice_flow_t *flow, uint32_t now) { if (flow) flow->voice_sent_at = now; }
void orb_voice_flow_touch(orb_voice_flow_t *flow, bool held)
{ if (flow) { flow->mic_held = held; if (held) flow->voice_touch = true; } }
void orb_voice_flow_boot(orb_voice_flow_t *flow) { if (flow) flow->voice_touch = false; }
void orb_voice_flow_poll(orb_voice_flow_t *flow, orb_voice_phase_t phase)
{ if (flow && flow->voice_pending && flow->command_phase != phase) flow->voice_pending = false; }

bool orb_app_voice_supported(const orb_app_voice_model_t *w, const char *target, const char *card_id)
{
    if (target && !strcmp(target, "time") && (!card_id || !card_id[0])) return true;
    if (card_id && card_id[0]) {
        bool reply = false;
        for (size_t i = 0; i < w->model->card_count; i++)
            if (!strcmp(card_id, w->model->cards[i].id))
                reply = w->model->cards[i].reply && !w->model->cards[i].answered;
        /* Card text answers use their explicit reply grant. can_message only
         * governs a new session message, not this bounded card response. */
        return reply;
    }
    for (size_t i = 0; i < w->model->session_count; i++)
        if (!strcmp(target, w->model->sessions[i].id))
            return w->model->prefs.sources[w->model->sessions[i].agent] && w->model->sessions[i].can_message;
    /* A freshly paged idle session carries its explicit capability. Invalidate
     * it at link loss; formerly focused sessions never inherit that grant. */
    if (w->selection_external && w->selection && !strcmp(target, w->selection->id))
        return w->model->prefs.sources[w->selection->agent] && w->selection->can_message;
    /* Dot needs an advertised subscription, not just its source toggle. */
    return false;
}

const char *orb_voice_review_blocked(bool ready, bool available, bool session, const char *target, bool text)
{
    bool dot = !target[0] || !strcmp(target, "dot");
    return !ready ? "Reconnect your Mac to send" :
        !available ? (session ? "This session is read-only" : dot ? "Dot isn’t connected" :
                      "Can’t send to an ended session") :
        !text ? "Hold BOOT to talk" : NULL;
}

bool orb_voice_flow_command(orb_voice_flow_t *flow, const orb_app_voice_model_t *projection,
    orb_voice_action_t action, const orb_voice_command_port_t *port)
{
    if (!flow || !projection || !projection->model || !port || !port->request) return false;
    const orb_model_snapshot_t *model = projection->model;
    if (action == ORB_VOICE_CONFIRM && (!port->physical || !port->physical(port->context))) return false;
    if (action != ORB_VOICE_CANCEL && action != ORB_VOICE_STOP && !(model->online && model->synced)) {
        if (port->notice) port->notice(port->context, model->online ? "Waiting for Mac sync" : "Voice needs your Mac");
        return false;
    }
    if (action == ORB_VOICE_CONFIRM && !orb_app_voice_supported(projection, model->voice.target, model->voice.card)) return false;
    if (!port->request(port->context, action)) {
        if (port->notice) port->notice(port->context, "Voice queue full. Try again.");
        return false;
    }
    if (port->notice) port->notice(port->context, "");
    /* Only confirmation latches: a short START/STOP can coalesce before poll. */
    flow->voice_pending = action == ORB_VOICE_CONFIRM;
    flow->command_phase = model->voice.phase;
    return true;
}

bool orb_app_voice_speech_agent(const orb_model_snapshot_t *model, const orb_voice_state_t *voice, orb_agent_t *agent)
{
    if (!model || !voice || !agent || !model->online || !model->synced || !voice->stream || !voice->target[0]) return false;
    /* Playback identity follows its explicit request, independent of navigation. */
    if (!strcmp(voice->target, "dot")) { *agent = ORB_AGENT_DOT; return true; }
    for (unsigned i = 0; i < model->session_count; i++)
        if (!strcmp(voice->target, model->sessions[i].id)) { *agent = model->sessions[i].agent; return true; }
    return false;
}
