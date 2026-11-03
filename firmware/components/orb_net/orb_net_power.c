#include "orb_net.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "orb_net_priv.h"
#include "orb_net_util.h"

static portMUX_TYPE s_power_lock = portMUX_INITIALIZER_UNLOCKED;
static orb_net_power_hint_t s_hint = ORB_NET_POWER_INTERACTIVE;
static bool s_provisioning;
static unsigned s_work;
static uint32_t s_interactive_generation;
static orb_net_power_cb_t s_observer;
static void *s_observer_ctx;
static orb_net_counters_t s_counters;
static bool s_joined;
static int s_applied = -1;   /* the net task alone calls esp_wifi_set_ps */

static orb_net_power_hint_t effective(void)
{
    return orb_net_power_background(s_hint == ORB_NET_POWER_BACKGROUND, s_provisioning, s_work)
               ? ORB_NET_POWER_BACKGROUND : ORB_NET_POWER_INTERACTIVE;
}

/* Called with the lock held so a transient interactive hint cannot be lost. */
static void changed(orb_net_power_hint_t before)
{
    orb_net_power_cb_t cb;
    void *ctx;
    bool notify = before != effective();
    if (notify && effective() == ORB_NET_POWER_INTERACTIVE) {
        s_interactive_generation++;
    }
    cb = s_observer;
    ctx = s_observer_ctx;
    portEXIT_CRITICAL(&s_power_lock);
    if (notify) {
        orb_net_power_wake();
        if (cb) {
            cb(ctx);
        }
    }
}

void orb_net_set_power_hint(orb_net_power_hint_t hint)
{
    if (hint != ORB_NET_POWER_INTERACTIVE && hint != ORB_NET_POWER_BACKGROUND) {
        return;
    }
    portENTER_CRITICAL(&s_power_lock);
    orb_net_power_hint_t before = effective();
    s_hint = hint;
    changed(before);
}

orb_net_power_hint_t orb_net_get_power_hint(void)
{
    portENTER_CRITICAL(&s_power_lock);
    orb_net_power_hint_t hint = effective();
    portEXIT_CRITICAL(&s_power_lock);
    return hint;
}

uint32_t orb_net_interactive_generation(void)
{
    portENTER_CRITICAL(&s_power_lock);
    uint32_t generation = s_interactive_generation;
    portEXIT_CRITICAL(&s_power_lock);
    return generation;
}

void orb_net_set_work_active(unsigned work, bool active)
{
    work &= ORB_NET_WORK_OTA | ORB_NET_WORK_VOICE;
    portENTER_CRITICAL(&s_power_lock);
    orb_net_power_hint_t before = effective();
    s_work = active ? s_work | work : s_work & ~work;
    changed(before);
}

void orb_net_on_power_hint(orb_net_power_cb_t cb, void *ctx)
{
    portENTER_CRITICAL(&s_power_lock);
    s_observer = cb;
    s_observer_ctx = ctx;
    portEXIT_CRITICAL(&s_power_lock);
}

void orb_net_power_provisioning(bool active)
{
    portENTER_CRITICAL(&s_power_lock);
    orb_net_power_hint_t before = effective();
    s_provisioning = active;
    changed(before);
}

void orb_net_power_apply(void)
{
    orb_net_power_hint_t hint = orb_net_get_power_hint();
    if (s_applied == (int)hint) {
        return;
    }
    esp_err_t err = esp_wifi_set_ps(hint == ORB_NET_POWER_BACKGROUND ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM);
    if (err == ESP_OK) {
        s_applied = hint;
    } else {
        ESP_LOGW("net", "modem policy: %s", esp_err_to_name(err));
    }
}

void orb_net_count_scan(void)
{
    portENTER_CRITICAL(&s_power_lock);
    s_counters.scans++;
    portEXIT_CRITICAL(&s_power_lock);
}

void orb_net_count_join(void)
{
    portENTER_CRITICAL(&s_power_lock);
    if (s_joined) {
        s_counters.reconnects++;
    }
    s_joined = true;
    portEXIT_CRITICAL(&s_power_lock);
}

void orb_net_get_counters(orb_net_counters_t *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_power_lock);
    *out = s_counters;
    portEXIT_CRITICAL(&s_power_lock);
}
