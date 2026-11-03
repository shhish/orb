#pragma once

#include "orb_app_host.h"
#include "orb_app_home.h"

/* Home on the shared single-route host. The shell supplies its iris
 * presentation, navigation and pointer; Home writes run here, through the
 * service's unchanged request calls. */
typedef struct {
    void (*transition)(void *context, const lv_area_t *token, bool reverse, bool before);
    orb_nav_t *nav;
    lv_indev_t *const *input;
    void *context;
} orb_home_host_shell_t;

orb_app_host_t *orb_home_host_create(bool preview, const orb_home_host_shell_t *shell);
/* model->fixture is borrowed by creation only; later projections omit it. */
bool orb_home_host_attach(orb_app_host_t *host, lv_obj_t *page, const orb_app_home_model_t *model);
void orb_home_host_environment(orb_app_host_t *host, unsigned hour, bool contrast);
/* Only ordinary committed routes make the next Home entry fresh. */
void orb_home_host_departure(orb_app_host_t *host, orb_screen_id_t destination,
                             bool ordinary, bool *fresh_entry);
void orb_home_host_enter(orb_app_host_t *host, bool *fresh_entry);
