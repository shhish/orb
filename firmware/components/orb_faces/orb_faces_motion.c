#include "orb_faces_private.h"
#include "orb_kit.h"
#include "orb_kit_motion.h"
#include "orb_kit_effects.h"
#include "orb_kit_controls_motion.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void orb_faces_rolling_text(orb_faces_t *v, lv_obj_t *label, const char *value)
{
    if (!label) return;
    orb_kit_value_set_rolling(label, value, NULL);
    if (!v->active || v->covered || v->always_on || v->building || v->mode != ORB_FACES_FACE)
        orb_kit_motion_finish(label);
}

void orb_faces_rolling_static(orb_faces_t *v, lv_obj_t *label, char *storage, size_t capacity, const char *value)
{
    if (!label || !strcmp(storage, value)) return;
    orb_faces_rolling_text(v, label, value);
    snprintf(storage, capacity, "%s", value);
}

void orb_faces_set_active(orb_faces_t *v, bool active)
{
    if (!v) return;
    bool waking = active && !v->active && v->wake_values && !v->always_on && !v->covered;
    if (!active && v->active && !v->always_on && !v->covered) {
        for (unsigned i = 0; i < 3; i++)
            if (v->activity[i]) v->lit_activity[i] = (int)lroundf(orb_kit_ring_get_presented_value(v->activity[i]));
        memcpy(v->lit_activity_known, v->activity_known, sizeof(v->activity_known));
        for (unsigned slot = 0; slot < 4; slot++)
            for (unsigned provider = 0; provider < ORB_FACE_PROVIDER_COUNT; provider++) {
                face_form_t *form = &v->forms[slot][provider];
                if (form->ring) v->lit_forms[slot][provider] = (int)lroundf(orb_kit_ring_get_presented_value(form->ring));
                v->lit_forms_known[slot][provider] = form->known;
            }
        v->wake_values = true;
    }
    v->active = active;
    orb_kit_motion_set_active(v->root, active && !v->covered && !v->always_on);
    orb_faces_seconds_policy(v);
    if (waking) {
        /* Quiet updates are final while dark. The last lit presentation is a
         * separate value, so wake follows only the world that changed. */
        for (unsigned i = 0; i < 3; i++) {
            orb_face_complication_t data;
            orb_faces_complication(v, (orb_face_provider_t)v->prefs.activity_slot[i], &data);
            if (!v->activity[i] || !v->lit_activity_known[i] || data.percent < 0 || v->lit_activity[i] == v->activity_target[i]) continue;
            orb_kit_ring_set_value(v->activity[i], v->lit_activity[i], false);
            orb_kit_ring_set_value(v->activity[i], v->activity_target[i], true);
        }
        for (unsigned slot = 0; slot < 4; slot++)
            for (unsigned provider = 0; provider < ORB_FACE_PROVIDER_COUNT; provider++) {
                face_form_t *form = &v->forms[slot][provider];
                orb_face_complication_t data;
                orb_faces_complication(v, (orb_face_provider_t)provider, &data);
                if (!form->ring || !v->lit_forms_known[slot][provider] || data.percent < 0 || !lv_obj_is_visible(form->root) ||
                    v->lit_forms[slot][provider] == form->target) continue;
                orb_kit_ring_set_value(form->ring, v->lit_forms[slot][provider], false);
                orb_kit_ring_set_value(form->ring, form->target, true);
            }
        v->wake_values = false;
    }
    if (!active) {
        orb_kit_motion_finish(v->root);
        /* Settle kit-owned ring timers too; pausing only our seconds timer
         * leaves a value animation running underneath the dimmed surface. */
        for (unsigned i = 0; i < 3; i++) if (v->activity[i])
            orb_kit_ring_set_value(v->activity[i], v->activity_target[i], false);
        for (unsigned slot = 0; slot < 4; slot++)
            for (unsigned provider = 0; provider < ORB_FACE_PROVIDER_COUNT; provider++) {
                face_form_t *form = &v->forms[slot][provider];
                if (form->ring) orb_kit_ring_set_value(form->ring, form->target, false);
            }
    }
}


void orb_faces_set_covered(orb_faces_t *v, bool covered)
{
    if (!v) return;
    v->covered = covered;
    v->wake_values = false;
    orb_kit_motion_set_active(v->root, !covered && v->active && !v->always_on);
    if (covered) orb_kit_motion_finish(v->root);
    orb_faces_seconds_policy(v);
}
