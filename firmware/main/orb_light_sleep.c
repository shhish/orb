#include "orb_light_sleep.h"

#include <stdatomic.h>
#include "driver/gpio.h"
#include "hal/gpio_ll.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_private/pm_impl.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "lightsleep";
static esp_pm_lock_handle_t s_lock;
static orb_light_sleep_lock_t s_policy_lock;
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static orb_light_sleep_status_t s_status = {.reason = ORB_LIGHT_SLEEP_DISABLED};
static atomic_bool s_allowed, s_failed;

/* A fault veto is independent of PM lock ownership. Retry only from the
 * service owner, once per deadline; persistent faults settle at one per 30 s. */
static void record_fault(orb_light_sleep_fault_t fault, esp_err_t err)
{
    atomic_store(&s_allowed, false);
    atomic_store(&s_failed, true);
    portENTER_CRITICAL(&s_status_lock);
    s_status.eligible = s_status.wake_ready = false;
    s_status.reason = ORB_LIGHT_SLEEP_WAKE_FAILED;
    s_status.fault = fault;
    s_status.last_error = err;
    s_status.failures++;
    uint32_t delay = s_status.retry_delay_ms;
    s_status.retry_delay_ms = delay ? (delay < 16000 ? delay * 2 : 30000) : 1000;
    s_status.retry_at_us = esp_timer_get_time() + (int64_t)s_status.retry_delay_ms * 1000;
    portEXIT_CRITICAL(&s_status_lock);
}

static esp_err_t take_awake_lock(void)
{
    esp_err_t err = ESP_OK;
    if (!s_lock) err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "orb_awake", &s_lock);
    if (err == ESP_OK && !s_policy_lock.held) {
        err = esp_pm_lock_acquire(s_lock);
        if (err == ESP_OK) s_policy_lock.held = true;
    }
    return err;
}

static esp_err_t configure_pm(bool allow_sleep)
{
    esp_pm_config_t config = {.max_freq_mhz = 240, .min_freq_mhz = 240,
                             .light_sleep_enable = allow_sleep};
#if CONFIG_ORB_DYNAMIC_FREQUENCY_SCALING
    config.min_freq_mhz = 80;
#endif
    return esp_pm_configure(&config);
}

#if CONFIG_ORB_LIGHT_SLEEP
static atomic_bool s_gpio_armed;
static bool s_arm_requested, s_inputs_ready;
static bool s_skip_registered, s_enter_registered, s_exit_registered;
static void (*s_wake_inputs)(void);
static const struct {
    gpio_num_t pin;
    gpio_int_type_t wake, edge;
} s_inputs[] = {
    {GPIO_NUM_11, GPIO_INTR_LOW_LEVEL, GPIO_INTR_NEGEDGE},
    {GPIO_NUM_3, GPIO_INTR_HIGH_LEVEL, GPIO_INTR_ANYEDGE},
    {GPIO_NUM_0, GPIO_INTR_LOW_LEVEL, GPIO_INTR_ANYEDGE},
};

static bool contact_active(void)
{
    return !gpio_get_level(GPIO_NUM_11) || gpio_get_level(GPIO_NUM_3) || !gpio_get_level(GPIO_NUM_0);
}

static bool restore_edges(void)
{
    bool ok = true;
    for (unsigned i = 0; i < sizeof(s_inputs) / sizeof(s_inputs[0]); i++) {
        bool edge_ready = gpio_set_intr_type(s_inputs[i].pin, s_inputs[i].edge) == ESP_OK;
        ok &= edge_ready;
        ok &= gpio_sleep_sel_dis(s_inputs[i].pin) == ESP_OK;
        /* Never unmask an interrupt that might still be a held level. The
         * fault veto keeps the CPU awake for recovery and ordinary polls. */
        if (edge_ready) ok &= gpio_intr_enable(s_inputs[i].pin) == ESP_OK;
        else (void)gpio_intr_disable(s_inputs[i].pin);
    }
    return ok;
}

static bool prepare_wake_inputs(void)
{
    bool ok = true;
    for (unsigned i = 0; i < sizeof(s_inputs) / sizeof(s_inputs[0]); i++)
        ok &= gpio_intr_disable(s_inputs[i].pin) == ESP_OK;
    for (unsigned i = 0; i < sizeof(s_inputs) / sizeof(s_inputs[0]); i++)
        ok &= gpio_wakeup_enable(s_inputs[i].pin, s_inputs[i].wake) == ESP_OK;
    return ok;
}

/* Also called from both idle hooks, even if no actual sleep follows. It must
 * stay read-only: masking inputs here could leave them masked indefinitely. */
static bool skip_sleep(void)
{
    /* IDF also consults this veto before dropping its per-core CPU_MAX lock.
     * Ineligible states already hold NO_LIGHT_SLEEP; vetoing them here would
     * accidentally prevent ordinary idle DFS, including while on USB. */
    /* Faults retain an independent veto even if a PM lock/config API failed. */
    if (atomic_load(&s_failed)) return true;
    if (!atomic_load(&s_allowed)) return false;
    return atomic_load(&s_gpio_armed) || contact_active();
}

static esp_err_t sleep_entering(int64_t expected_us, void *arg)
{
    (void)expected_us;
    (void)arg;
    /* All fallible gpio_wakeup_enable calls already succeeded while the awake
     * lock was held. IDF shares INTR_TYPE between wake and the normal ISR;
     * only these deterministic register writes remain at actual sleep entry.
     * Keep the GPIO wake-enable bits armed continuously. */
    for (unsigned i = 0; i < sizeof(s_inputs) / sizeof(s_inputs[0]); i++) {
        gpio_ll_intr_disable(&GPIO, s_inputs[i].pin);
        gpio_ll_set_intr_type(&GPIO, s_inputs[i].pin, s_inputs[i].wake);
    }
    atomic_store(&s_gpio_armed, true);
    return ESP_OK;
}

static esp_err_t sleep_exited(int64_t slept_us, void *arg)
{
    (void)arg;
    bool slept = slept_us > 0 && esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED;
    bool sample = s_wake_inputs && (contact_active() ||
        (slept && (esp_sleep_get_wakeup_causes() & (1u << ESP_SLEEP_WAKEUP_GPIO))));
    /* Preserve the first physical sample before touching shared GPIO state. */
    if (sample) s_wake_inputs();
    if (atomic_load(&s_gpio_armed)) {
        bool ok = restore_edges();
        atomic_store(&s_gpio_armed, false);
        if (!ok) record_fault(ORB_LIGHT_SLEEP_FAULT_RESTORE, ESP_FAIL);
    }
    /* A release can land while edge delivery is masked. Always re-sample even
     * if every input is now idle, so a short tap cannot turn into a held PWR. */
    if (s_wake_inputs && (sample || contact_active())) s_wake_inputs();
    /* IDF clears its wake flag on rejected attempts. The PM callback duration
     * includes sleep entry/exit overhead, exactly as IDF's own slept_us does. */
    if (slept) {
        portENTER_CRITICAL(&s_status_lock);
        s_status.entries++;
        s_status.slept_us += (uint64_t)slept_us;
        portEXIT_CRITICAL(&s_status_lock);
    }
    return ESP_OK;
}

static esp_err_t register_callbacks(void)
{
    esp_err_t err = ESP_OK;
    if (!s_skip_registered) {
        err = esp_pm_register_skip_light_sleep_callback(skip_sleep);
        s_skip_registered = err == ESP_OK;
    }
    /* IDF can install enter then fail allocating exit in a combined call.
     * Track each registration so recovery never duplicates or leaks a node. */
    if (err == ESP_OK && !s_enter_registered) {
        esp_pm_sleep_cbs_register_config_t cb = {.enter_cb = sleep_entering};
        err = esp_pm_light_sleep_register_cbs(&cb);
        s_enter_registered = err == ESP_OK;
    }
    if (err == ESP_OK && !s_exit_registered) {
        esp_pm_sleep_cbs_register_config_t cb = {.exit_cb = sleep_exited};
        err = esp_pm_light_sleep_register_cbs(&cb);
        s_exit_registered = err == ESP_OK;
    }
    return err;
}

static esp_err_t retain_pins(void)
{
    /* Keep the accepted active pin state. In particular CS/reset stay high,
     * open-drain I2C stays pulled up and the idle PA stays low. Do not change
     * PMU rails or isolate panel/touch power while they must retain state. */
    static const gpio_num_t pins[] = {
        GPIO_NUM_12, GPIO_NUM_38, GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7,
        GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_14, GPIO_NUM_15, GPIO_NUM_46,
        GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_16, GPIO_NUM_45,
        GPIO_NUM_11, GPIO_NUM_3, GPIO_NUM_0,
    };
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        esp_err_t err = gpio_sleep_sel_dis(pins[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static bool setup(bool arm)
{
    orb_light_sleep_fault_t fault = ORB_LIGHT_SLEEP_FAULT_LOCK;
    esp_err_t err = take_awake_lock();
    if (err == ESP_OK) {
        fault = ORB_LIGHT_SLEEP_FAULT_CALLBACK;
        err = register_callbacks();
    }
    orb_light_sleep_status_t status;
    orb_light_sleep_status(&status);
    if (err == ESP_OK && !status.configured) {
        fault = ORB_LIGHT_SLEEP_FAULT_CONFIG;
        err = configure_pm(true);
        portENTER_CRITICAL(&s_status_lock);
        s_status.configured = err == ESP_OK;
        portEXIT_CRITICAL(&s_status_lock);
    }
    if (err == ESP_OK && arm) {
        fault = ORB_LIGHT_SLEEP_FAULT_WAKE;
        err = s_inputs_ready && s_wake_inputs ? retain_pins() : ESP_ERR_INVALID_STATE;
        if (err == ESP_OK) {
            bool armed = prepare_wake_inputs();
            bool restored = restore_edges();
            /* Recovery may follow a masked release after a restore fault. */
            s_wake_inputs();
            if (!armed || !restored) err = ESP_FAIL;
        }
        if (err == ESP_OK) err = esp_sleep_enable_gpio_wakeup();
        if (err == ESP_OK) err = esp_sleep_enable_wifi_wakeup();
    }
    if (err != ESP_OK) {
        record_fault(fault, err);
        ESP_LOGW(TAG, "sleep setup failed (%s): %s; retry scheduled",
                 orb_light_sleep_fault_name(fault), esp_err_to_name(err));
        return false;
    }
    portENTER_CRITICAL(&s_status_lock);
    s_status.wake_ready = arm;
    s_status.reason = ORB_LIGHT_SLEEP_WAKE_UNREADY;
    if (arm) {
        s_status.recoveries += atomic_load(&s_failed);
        s_status.fault = ORB_LIGHT_SLEEP_FAULT_NONE;
        s_status.retry_at_us = 0;
        s_status.retry_delay_ms = 0;
    }
    portEXIT_CRITICAL(&s_status_lock);
    if (arm) atomic_store(&s_failed, false);
    return true;
}

static void retry_setup(void)
{
    if (!s_arm_requested || !atomic_load(&s_failed)) return;
    portENTER_CRITICAL(&s_status_lock);
    bool due = esp_timer_get_time() >= s_status.retry_at_us;
    if (due) s_status.retries++;
    portEXIT_CRITICAL(&s_status_lock);
    if (due) (void)setup(true);
}
#endif

void orb_light_sleep_init(void)
{
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
#if CONFIG_ORB_LIGHT_SLEEP
    if (s_mutex && setup(false)) return;
    /* Before a skip veto exists, boot must leave IDF automatic sleep off.
     * Recovery later reconfigures while holding orb_awake and the fault veto. */
    (void)configure_pm(false);
#else
    if (take_awake_lock() != ESP_OK) ESP_LOGW(TAG, "policy lock unavailable");
    (void)configure_pm(false);
#endif
}

void orb_light_sleep_arm(bool inputs_ready, void (*wake_inputs)(void))
{
#if CONFIG_ORB_LIGHT_SLEEP
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_arm_requested = true;
    s_inputs_ready = inputs_ready;
    s_wake_inputs = wake_inputs;
    orb_light_sleep_status_t status;
    orb_light_sleep_status(&status);
    if (!status.wake_ready && !atomic_load(&s_failed)) (void)setup(true);
    xSemaphoreGive(s_mutex);
#else
    (void)inputs_ready;
    (void)wake_inputs;
#endif
}

static bool apply_reason(orb_light_sleep_reason_t reason)
{
    bool allowed = reason == ORB_LIGHT_SLEEP_ALLOWED;
    /* Revoke the callback gate before acquiring the lock; publish permission
     * only after the matching release. Both paths fail closed. */
    if (!allowed) atomic_store(&s_allowed, false);
    if (atomic_load(&s_failed)) {
        allowed = false;
        reason = ORB_LIGHT_SLEEP_WAKE_FAILED;
    } else {
        orb_light_sleep_lock_t next = s_policy_lock;
        orb_light_sleep_lock_action_t action = orb_light_sleep_lock_step(&next, reason);
        esp_err_t err = ESP_OK;
        if (action == ORB_LIGHT_SLEEP_LOCK_ACQUIRE)
            err = s_lock ? esp_pm_lock_acquire(s_lock) : ESP_ERR_INVALID_STATE;
        if (action == ORB_LIGHT_SLEEP_LOCK_RELEASE)
            err = s_lock ? esp_pm_lock_release(s_lock) : ESP_ERR_INVALID_STATE;
        if (err == ESP_OK) s_policy_lock = next;
        else {
            record_fault(ORB_LIGHT_SLEEP_FAULT_LOCK, err);
            allowed = false;
            reason = ORB_LIGHT_SLEEP_WAKE_FAILED;
        }
    }
    atomic_store(&s_allowed, allowed);
    portENTER_CRITICAL(&s_status_lock);
    s_status.eligible = allowed;
    s_status.reason = reason;
    portEXIT_CRITICAL(&s_status_lock);
    return allowed;
}

bool orb_light_sleep_update(orb_light_sleep_inputs_t inputs)
{
    if (!s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool contact = false;
#if CONFIG_ORB_LIGHT_SLEEP
    retry_setup();
    contact = contact_active();
#endif
    orb_light_sleep_status_t status;
    orb_light_sleep_status(&status);
    orb_light_sleep_reason_t reason = orb_light_sleep_policy(inputs, status.configured,
        status.wake_ready, atomic_load(&s_failed), status.usb_override, contact);
    bool allowed = apply_reason(reason);
    xSemaphoreGive(s_mutex);
    return allowed;
}

void orb_light_sleep_status(orb_light_sleep_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    if (atomic_load(&s_failed)) {
        out->eligible = false;
        out->reason = ORB_LIGHT_SLEEP_WAKE_FAILED;
    }
}

const char *orb_light_sleep_reason_name(orb_light_sleep_reason_t reason)
{
    static const char *const names[] = {
        "eligible", "disabled", "wake_unready", "wake_failed", "power_unknown",
        "not_sleep", "panel_on", "inhibitor", "usb", "voice", "ota", "alarm",
        "benchmark", "game", "contact_held",
    };
    return (unsigned)reason < sizeof(names) / sizeof(names[0]) ? names[reason] : "unknown";
}

const char *orb_light_sleep_fault_name(orb_light_sleep_fault_t fault)
{
    static const char *const names[] = {"none", "lock", "callback", "config", "wake", "restore"};
    return (unsigned)fault < sizeof(names) / sizeof(names[0]) ? names[fault] : "unknown";
}

#if CONFIG_ORB_DEVTOOLS
bool orb_light_sleep_set_usb_override(bool allowed)
{
    if (!s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!allowed) (void)apply_reason(ORB_LIGHT_SLEEP_USB);
    portENTER_CRITICAL(&s_status_lock);
    s_status.usb_override = allowed;
    portEXIT_CRITICAL(&s_status_lock);
    xSemaphoreGive(s_mutex);
    return true;
}
#endif
