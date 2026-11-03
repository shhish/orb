#pragma once

#include "orb_app_host.h"
#include "orb_app_notifications.h"

/* App-lifetime presenter plus optional framework instances. Admission failure
 * uses the same composer and the caller's original direct input callback.
 * One page is composed at a time; detach/reset/delete it before composing
 * another. Reset also retires age handles when admission used direct fallback.
 * Model/history/config pointers are borrowed only for compose or tick. */
typedef struct orb_notifications_host orb_notifications_host_t;
size_t orb_notifications_host_bytes(void);
orb_notifications_host_t *orb_notifications_host_create(bool preview);
void orb_notifications_host_destroy(orb_notifications_host_t *host);
void orb_notifications_host_reset(orb_notifications_host_t *host);
void orb_notifications_host_detach(orb_notifications_host_t *host, lv_obj_t *page);
orb_screen_instance_t *orb_notifications_host_current(orb_notifications_host_t *host);
orb_notifications_page_t orb_notifications_host_compose(orb_notifications_host_t *host,
    const orb_notifications_view_config_t *config, bool detail,
    bool (*command)(void *, orb_screen_instance_t *, const orb_intent_t *));
bool orb_notifications_host_tick(orb_notifications_host_t *host,
    const orb_notification_history_t *history, bool detail, uint32_t now);
