/* Now Playing actions shared by the Design 2.0 Stack. The shared level
 * editor owns volume; no retired transport layout or hidden rotary owner. */
enum { ACT_MEDIA_PREVIOUS = 580, ACT_MEDIA_TOGGLE, ACT_MEDIA_NEXT };

static bool nowplaying_selected(const orb_workspace_t *w)
{
    return w->surface == ORB_SHELL_STACK && !w->card[0] && stack_selected(w).kind == ORB_STACK_NOWPLAYING;
}

static bool nowplaying_clicked(orb_workspace_t *w, int action)
{
    if (action < ACT_MEDIA_PREVIOUS || action > ACT_MEDIA_NEXT) return false;
    if (!nowplaying_selected(w)) return true;
    if (!lv_indev_active() || !orb_input_require_physical(w->root)) {
        snprintf(w->notice, sizeof(w->notice), "Control on Orb"); render(w); return true;
    }
    orb_media_action_t command = action == ACT_MEDIA_PREVIOUS ? ORB_MEDIA_PREVIOUS :
        action == ACT_MEDIA_NEXT ? ORB_MEDIA_NEXT :
        w->model->nowplaying.playing ? ORB_MEDIA_PAUSE : ORB_MEDIA_PLAY;
    if (!orb_model_nowplaying_request(command, 0))
        snprintf(w->notice, sizeof(w->notice), "Control unavailable");
    else w->notice[0] = '\0';
    return true;
}
