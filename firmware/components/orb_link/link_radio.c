#include "link_radio.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "orb_link.h"
#include "orb_net.h"
#include "link_backoff.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static link_retry_t s_retry;
static void (*s_wake)(void);
static uint32_t s_interactive_generation;
static bool s_sleep_eligible, s_model_prompt;
static orb_link_counters_t s_counters;
/* Negotiation and last report are owned by the link task. */
static bool s_negotiated, s_reported_background, s_pending_background, s_report_dirty;

static bool background(void)
{
    return orb_net_get_power_hint() == ORB_NET_POWER_BACKGROUND;
}

void orb_link_set_sleep_eligible(bool eligible)
{
    taskENTER_CRITICAL(&s_lock);
    bool changed = s_sleep_eligible != eligible;
    if (changed && !eligible) s_model_prompt = true;
    s_sleep_eligible = eligible;
    void (*wake)(void) = s_wake;
    taskEXIT_CRITICAL(&s_lock);
    if (changed && wake) wake();
}

uint32_t link_radio_model_poll_ms(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool eligible = s_sleep_eligible;
    taskEXIT_CRITICAL(&s_lock);
    return eligible ? 1000 : 50;
}

bool link_radio_model_prompt(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool prompt = s_model_prompt;
    s_model_prompt = false;
    taskEXIT_CRITICAL(&s_lock);
    return prompt;
}

static void on_hint(void *ctx)
{
    (void)ctx;
    taskENTER_CRITICAL(&s_lock);
    uint32_t generation = orb_net_interactive_generation();
    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;
    if (generation != s_interactive_generation) {
        /* A quick wake/sleep pair must still cut short an existing backoff. */
        link_retry_hint(&s_retry, true, now_ms);
        link_retry_hint(&s_retry, false, now_ms);
        s_interactive_generation = generation;
    }
    link_retry_hint(&s_retry, background(), now_ms);
    void (*wake)(void) = s_wake;
    taskEXIT_CRITICAL(&s_lock);
    if (wake) wake();
}

void link_radio_init(void (*wake)(void))
{
    bool bg = background();
    uint32_t generation = orb_net_interactive_generation();
    taskENTER_CRITICAL(&s_lock);
    s_wake = wake;
    s_retry.background = bg;
    s_interactive_generation = generation;
    taskEXIT_CRITICAL(&s_lock);
    orb_net_on_power_hint(on_hint, NULL);
    on_hint(NULL);
}

void link_radio_reset(void)
{
    s_negotiated = false;
    s_reported_background = false;
    s_report_dirty = false;
}

bool link_radio_hello(cJSON *hello)
{
    cJSON *caps = cJSON_GetObjectItemCaseSensitive(hello, "caps");
    cJSON *cap = cJSON_CreateString("power_hint");
    if (!cJSON_AddItemToArray(caps, cap)) { cJSON_Delete(cap); return false; }
    return cJSON_AddStringToObject(hello, "power_hint", background() ? "background" : "interactive") &&
           cJSON_AddNumberToObject(hello, "ping_interval_s", 15);
}

void link_radio_accept(const cJSON *result)
{
    const cJSON *caps = cJSON_GetObjectItemCaseSensitive(result, "caps"), *cap;
    s_negotiated = false;
    cJSON_ArrayForEach(cap, caps) {
        const char *name = cJSON_GetStringValue(cap);
        if (name && !strcmp(name, "power_hint")) s_negotiated = true;
    }
    /* A background hello used 15 s until negotiation completed. Report 45 s
     * before relaxing the local cadence, so the hub adjusts its deadline first. */
    s_reported_background = false;
    s_report_dirty = true;
}

bool link_radio_changed(void)
{
    return s_negotiated && (s_report_dirty || s_reported_background != background());
}

bool link_radio_report(cJSON *report)
{
    if (!s_negotiated) return true;
    s_pending_background = background();
    return cJSON_AddStringToObject(report, "power_hint", s_pending_background ? "background" : "interactive") &&
           cJSON_AddNumberToObject(report, "ping_interval_s", s_pending_background ? 45 : 15);
}

void link_radio_report_sent(void)
{
    s_reported_background = s_pending_background;
    s_report_dirty = false;
}

unsigned link_radio_ping_s(void)
{
    return s_negotiated && s_reported_background ? 45 : 15;
}

bool link_radio_negotiated(void)
{
    return s_negotiated;
}

uint32_t link_radio_wait_begin(unsigned attempt, uint32_t random)
{
    taskENTER_CRITICAL(&s_lock);
    link_retry_wait(&s_retry, true);
    bool bg = s_retry.background;
    taskEXIT_CRITICAL(&s_lock);
    return link_backoff_power_ms(attempt, random, bg);
}

bool link_radio_wait_prompt(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool prompt = link_retry_take_prompt(&s_retry);
    taskEXIT_CRITICAL(&s_lock);
    return prompt;
}

int64_t link_radio_wait_until(int64_t until, int64_t now)
{
    /* A throttled wake still restores the interactive ceiling. Repeated wakes
     * can only shorten an existing deadline, never push it farther out. */
    int64_t limit = now + 30000000;
    return !background() && until > limit ? limit : until;
}

void link_radio_wait_end(void)
{
    taskENTER_CRITICAL(&s_lock);
    link_retry_wait(&s_retry, false);
    taskEXIT_CRITICAL(&s_lock);
}

void link_radio_rx(size_t bytes)
{
    taskENTER_CRITICAL(&s_lock);
    s_counters.messages_rx++;
    s_counters.bytes_rx += bytes;
    taskEXIT_CRITICAL(&s_lock);
}

void link_radio_tx(size_t bytes)
{
    taskENTER_CRITICAL(&s_lock);
    s_counters.messages_tx++;
    s_counters.bytes_tx += bytes;
    taskEXIT_CRITICAL(&s_lock);
}

void link_radio_connect(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_counters.connects++;
    if (s_counters.connects > 1) s_counters.reconnects++;
    taskEXIT_CRITICAL(&s_lock);
}

esp_err_t orb_link_counters(orb_link_counters_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&s_lock);
    *out = s_counters;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}
