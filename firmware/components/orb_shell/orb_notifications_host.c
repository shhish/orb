#include "orb_notifications_host.h"
#include "orb_page.h"
#include "orb_shell.h"

_Static_assert((int)ORB_APP_NOTIFICATIONS_LIST == (int)ORB_SHELL_NOTIFICATIONS, "compatible Notifications route");
_Static_assert((int)ORB_APP_NOTIFICATIONS_DETAIL == (int)ORB_SHELL_NOTIFICATION_DETAIL, "compatible detail route");

struct orb_notifications_host {
    orb_notifications_view_t *view;
    orb_app_host_t *screens[2];
    bool preview, detail;
};

size_t orb_notifications_host_bytes(void)
{ return sizeof(orb_notifications_host_t) + orb_notifications_view_bytes(); }

orb_notifications_host_t *orb_notifications_host_create(bool preview)
{
    orb_notifications_host_t *host = orb_memory_calloc(ORB_MEMORY_WORKSPACE, 1, sizeof(*host));
    if (!host) return NULL;
    host->view = orb_notifications_view_create();
    if (!host->view) { orb_memory_free(host); return NULL; }
    host->preview = preview;
    return host;
}

void orb_notifications_host_destroy(orb_notifications_host_t *host)
{
    if (!host) return;
    orb_app_host_destroy(host->screens[0]);
    orb_app_host_destroy(host->screens[1]);
    orb_notifications_view_destroy(host->view);
    orb_memory_free(host);
}

void orb_notifications_host_reset(orb_notifications_host_t *host)
{ if (host) orb_notifications_view_reset(host->view); }

void orb_notifications_host_detach(orb_notifications_host_t *host, lv_obj_t *page)
{
    if (!host) return;
    orb_app_host_detach_page(host->screens[0], page);
    orb_app_host_detach_page(host->screens[1], page);
}

orb_screen_instance_t *orb_notifications_host_current(orb_notifications_host_t *host)
{ return host ? orb_app_host_current(host->screens[host->detail]) : NULL; }

static bool allow_synthetic(const orb_intent_t *intent)
{
    return intent->kind == ORB_INTENT_APP_COMMAND &&
        (intent->value.command.target == ORB_APP_NOTIFICATIONS_LIST ||
         intent->value.command.target == ORB_APP_NOTIFICATIONS_DETAIL);
}
static void rim_finish(orb_screen_instance_t *instance, bool commit) { (void)instance; (void)commit; }

orb_notifications_page_t orb_notifications_host_compose(orb_notifications_host_t *host,
    const orb_notifications_view_config_t *config, bool detail,
    bool (*command)(void *, orb_screen_instance_t *, const orb_intent_t *))
{
    if (!host || !config) return (orb_notifications_page_t){0};
    host->detail = detail;
    orb_app_host_t **slot = &host->screens[detail];
    if (!*slot) *slot = orb_app_host_create(host->preview, &(orb_app_host_config_t){
        .descriptor = detail ? orb_app_notifications_detail_descriptor : orb_app_notifications_list_descriptor,
        .name = "Notifications",
        .forget_on_detach = true, .command = command, .context = config->context,
        .allow_synthetic = allow_synthetic, .rim_finish = rim_finish});
    /* Admit the empty framework instance before touching shared scaffold.
     * A failed stage/commit leaves every original title/navigation node valid
     * for the direct composer, including a refused LVGL parent allocation. */
    if (!orb_app_host_attach(*slot, config->page))
        return detail ? orb_notifications_view_detail(host->view, config) : orb_notifications_view_list(host->view, config);
    orb_notifications_page_t page = orb_app_notifications_compose(orb_app_host_current(*slot), host->view, config);
    if (page.title) lv_obj_set_parent(page.title, config->page);
    return page;
}

bool orb_notifications_host_tick(orb_notifications_host_t *host,
    const orb_notification_history_t *history, bool detail, uint32_t now)
{ return host && orb_notifications_view_tick(host->view, history, detail, now); }
