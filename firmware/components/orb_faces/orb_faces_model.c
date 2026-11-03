#include "orb_faces.h"
#include <math.h>
#include <string.h>

void orb_faces_snapshot_model(orb_faces_snapshot_t *out, const orb_model_snapshot_t *model)
{
    if (!out) return;
    out->usage_percent[0] = out->usage_percent[1] = -1;
    out->usage_stale[0] = out->usage_stale[1] = false;
    out->working = out->sessions = 0;
    if (!model) { out->online = false; return; }
    out->online = model->online;
    unsigned priority[2] = {0, 0};
    for (size_t i = 0; i < model->usage.count; i++) {
        const orb_usage_window_t *window = &model->usage.windows[i];
        unsigned provider = (unsigned)window->provider;
        if (provider > 1 || !isfinite(window->used_percent)) continue;
        bool five_hour = !strcmp(window->label, "5 hours") || !strcmp(window->label, "5h") ||
            !strcmp(window->label, "5-hour");
        /* Model-specific quotas are not the account quota. Only the primary
         * window or its documented all-model weekly fallback belong here. */
        bool stale = window->stale || model->usage.unknown[provider] || !model->online;
        unsigned rank = five_hour ? 2 : !strcmp(window->label, "week") ? 1 : 0;
        /* Prefer a current fallback over a stale primary, but keep the last
         * account quota when all windows are stale instead of inventing "—". */
        if (rank && !stale) rank += 2;
        if (rank > priority[provider]) {
            out->usage_percent[provider] = (int)lround(fmax(0., fmin(100., window->used_percent)));
            out->usage_stale[provider] = stale;
            priority[provider] = rank;
        }
    }
    out->sessions = (unsigned)model->session_count;
    for (size_t i = 0; i < model->session_count; i++)
        out->working += model->sessions[i].state == ORB_SESSION_BUSY;
}

