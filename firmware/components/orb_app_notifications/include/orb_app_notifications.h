#pragma once

#include "orb_notifications_view.h"
#include "orb_screen.h"

enum { ORB_APP_NOTIFICATIONS_LIST = 6, ORB_APP_NOTIFICATIONS_DETAIL = 25 };

const orb_screen_descriptor_t *orb_app_notifications_list_descriptor(void);
const orb_screen_descriptor_t *orb_app_notifications_detail_descriptor(void);
/* Called once after admission, at the shell's full-render opportunity.
 * Config/model/history are borrowed only for this synchronous composition. */
orb_notifications_page_t orb_app_notifications_compose(orb_screen_instance_t *instance,
    orb_notifications_view_t *view, const orb_notifications_view_config_t *config);
orb_notifications_page_t orb_app_notifications_page(orb_screen_instance_t *instance);
lv_obj_t *orb_app_notifications_action(orb_screen_instance_t *instance, unsigned action);
