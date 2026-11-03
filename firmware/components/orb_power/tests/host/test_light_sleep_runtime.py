#!/usr/bin/env python3
"""Run the real automatic-sleep adapter against deterministic IDF/GPIO seams."""

import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
STUB = r"""
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#ifndef CONFIG_ORB_LIGHT_SLEEP
#define CONFIG_ORB_LIGHT_SLEEP 1
#endif
#ifndef CONFIG_ORB_DYNAMIC_FREQUENCY_SCALING
#define CONFIG_ORB_DYNAMIC_FREQUENCY_SCALING 1
#endif
#ifndef CONFIG_ORB_DEVTOOLS
#define CONFIG_ORB_DEVTOOLS 1
#endif
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_PM_NO_LIGHT_SLEEP 2
#define ESP_SLEEP_WAKEUP_UNDEFINED 0
#define ESP_SLEEP_WAKEUP_GPIO 7
#define ESP_SLEEP_WAKEUP_TIMER 4
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define portMAX_DELAY 0xffffffffu
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
typedef int esp_err_t;
typedef int gpio_num_t;
typedef int gpio_int_type_t;
typedef int portMUX_TYPE;
typedef int StaticSemaphore_t;
typedef void *SemaphoreHandle_t;
typedef void *esp_pm_lock_handle_t;
typedef struct { int max_freq_mhz, min_freq_mhz; bool light_sleep_enable; } esp_pm_config_t;
typedef esp_err_t (*sleep_callback_t)(int64_t, void *);
typedef struct { sleep_callback_t enter_cb, exit_cb; } esp_pm_sleep_cbs_register_config_t;
enum { GPIO_INTR_NEGEDGE = 2, GPIO_INTR_ANYEDGE, GPIO_INTR_LOW_LEVEL, GPIO_INTR_HIGH_LEVEL };
extern int GPIO;
int64_t esp_timer_get_time(void);
esp_err_t esp_pm_configure(const void *config);
esp_err_t esp_pm_lock_create(int type, int arg, const char *name, esp_pm_lock_handle_t *lock);
esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t lock);
esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t lock);
esp_err_t esp_pm_register_skip_light_sleep_callback(bool (*callback)(void));
esp_err_t esp_pm_light_sleep_register_cbs(esp_pm_sleep_cbs_register_config_t *callbacks);
esp_err_t esp_sleep_enable_gpio_wakeup(void);
esp_err_t esp_sleep_enable_wifi_wakeup(void);
int esp_sleep_get_wakeup_cause(void);
uint32_t esp_sleep_get_wakeup_causes(void);
const char *esp_err_to_name(esp_err_t error);
int gpio_get_level(gpio_num_t pin);
esp_err_t gpio_wakeup_enable(gpio_num_t pin, gpio_int_type_t type);
esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t type);
esp_err_t gpio_sleep_sel_dis(gpio_num_t pin);
esp_err_t gpio_intr_enable(gpio_num_t pin);
esp_err_t gpio_intr_disable(gpio_num_t pin);
void gpio_ll_intr_disable(int *device, uint32_t pin);
void gpio_ll_set_intr_type(int *device, uint32_t pin, gpio_int_type_t type);
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage);
void xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t timeout);
void xSemaphoreGive(SemaphoreHandle_t mutex);
"""
FIXTURE = r"""
#include "stub.h"
#include <assert.h>
#include <string.h>
#include "orb_light_sleep.c"

int GPIO;
static int levels[49], types[49], operations, notifications, lock_depth, wake_arm_calls;
static int acquire_calls, release_calls, config_calls, enter_registrations, exit_registrations;
static int samples[64];
static int64_t now_us;
static bool enabled[49], retained[49], irq_enabled[49];
static bool fail_config, fail_gpio, fail_gpio_source, fail_wifi, fail_callback, fail_exit;
static bool fail_restore, fail_retain, fail_create, fail_acquire, fail_release;
static bool release_during_restore, press_during_restore;
static int release_pin, cause;
static bool (*skip_callback)(void);
static esp_pm_sleep_cbs_register_config_t callbacks;
static esp_pm_config_t last_config;

int64_t esp_timer_get_time(void) { return now_us; }
esp_err_t esp_pm_configure(const void *value) {
    config_calls++;
    const esp_pm_config_t *config = value;
    if (config->light_sleep_enable && fail_config) return ESP_FAIL;
    last_config = *config;
    return ESP_OK;
}
esp_err_t esp_pm_lock_create(int type, int arg, const char *name, esp_pm_lock_handle_t *lock) {
    (void)type; (void)arg; (void)name;
    if (fail_create) return ESP_FAIL;
    *lock = &lock_depth; return ESP_OK;
}
esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t lock) {
    assert(lock && lock_depth == 0); acquire_calls++;
    if (fail_acquire) return ESP_FAIL;
    lock_depth++; return ESP_OK;
}
esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t lock) {
    assert(lock && lock_depth == 1); release_calls++;
    if (fail_release) return ESP_FAIL;
    lock_depth--; return ESP_OK;
}
esp_err_t esp_pm_register_skip_light_sleep_callback(bool (*callback)(void)) {
    if (fail_callback) return ESP_FAIL;
    assert(!skip_callback); skip_callback = callback; return ESP_OK;
}
esp_err_t esp_pm_light_sleep_register_cbs(esp_pm_sleep_cbs_register_config_t *value) {
    if (value->enter_cb) {
        assert(!callbacks.enter_cb);
        callbacks.enter_cb = value->enter_cb; enter_registrations++;
    }
    /* Match IDF's nontransactional combined registration failure. */
    if (value->exit_cb) {
        if (fail_exit) return ESP_FAIL;
        assert(!callbacks.exit_cb);
        callbacks.exit_cb = value->exit_cb; exit_registrations++;
    }
    return ESP_OK;
}
esp_err_t esp_sleep_enable_gpio_wakeup(void) { return fail_gpio_source ? ESP_FAIL : ESP_OK; }
esp_err_t esp_sleep_enable_wifi_wakeup(void) { return fail_wifi ? ESP_FAIL : ESP_OK; }
int esp_sleep_get_wakeup_cause(void) { return cause; }
uint32_t esp_sleep_get_wakeup_causes(void) { return 1u << cause; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "stub"; }
int gpio_get_level(gpio_num_t pin) { return levels[pin]; }
esp_err_t gpio_wakeup_enable(gpio_num_t pin, gpio_int_type_t type) {
    operations++; wake_arm_calls++;
    if (fail_gpio && pin == 3) return ESP_FAIL;
    enabled[pin] = true; types[pin] = type; return ESP_OK;
}
esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t type) {
    operations++;
    if (fail_restore) return ESP_FAIL;
    types[pin] = type;
    if (release_during_restore) levels[release_pin] = release_pin == 0;
    if (press_during_restore) levels[3] = 1;
    return ESP_OK;
}
esp_err_t gpio_sleep_sel_dis(gpio_num_t pin) {
    if (fail_retain) return ESP_FAIL;
    retained[pin] = true; return ESP_OK;
}
esp_err_t gpio_intr_enable(gpio_num_t pin) { operations++; irq_enabled[pin] = true; return ESP_OK; }
esp_err_t gpio_intr_disable(gpio_num_t pin) { operations++; irq_enabled[pin] = false; return ESP_OK; }
void gpio_ll_intr_disable(int *device, uint32_t pin) {
    (void)device; operations++; irq_enabled[pin] = false;
}
void gpio_ll_set_intr_type(int *device, uint32_t pin, gpio_int_type_t type) {
    (void)device; operations++; types[pin] = type;
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) { return storage; }
void xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t timeout) { (void)mutex; (void)timeout; }
void xSemaphoreGive(SemaphoreHandle_t mutex) { (void)mutex; }

static void wake_inputs(void) {
    assert(notifications < 64);
    samples[notifications++] = (levels[3] ? 1 : 0) | (!levels[0] ? 2 : 0);
}
static orb_light_sleep_inputs_t asleep(void) {
    return (orb_light_sleep_inputs_t){.power = {.state = ORB_POWER_SLEEP},
                                    .power_valid = true, .panel_off = true};
}
#if CONFIG_ORB_LIGHT_SLEEP
static void assert_edges(void) {
    assert(types[0] == GPIO_INTR_ANYEDGE && types[3] == GPIO_INTR_ANYEDGE);
    assert(types[11] == GPIO_INTR_NEGEDGE);
    assert(irq_enabled[0] && irq_enabled[3] && irq_enabled[11]);
}
static void clear_failures(void) {
    fail_config = fail_gpio = fail_gpio_source = fail_wifi = fail_callback = fail_exit = false;
    fail_restore = fail_retain = fail_create = fail_acquire = fail_release = false;
}
static orb_light_sleep_status_t status(void) {
    orb_light_sleep_status_t value; orb_light_sleep_status(&value); return value;
}
static void recover(void) {
    orb_light_sleep_status_t before = status();
    assert(before.fault != ORB_LIGHT_SLEEP_FAULT_NONE && !before.eligible);
    assert(before.retry_delay_ms == 1000 && before.retry_at_us == now_us + 1000000);
    int work = operations + acquire_calls + release_calls + config_calls;
    now_us = before.retry_at_us - 1;
    for (int i = 0; i < 100; i++) assert(!orb_light_sleep_update(asleep()));
    assert(work == operations + acquire_calls + release_calls + config_calls);
    assert(status().retries == before.retries);
    now_us++;
    clear_failures();
    assert(orb_light_sleep_update(asleep()));
    assert(lock_depth == 0 && !skip_callback());
    assert_edges();
    orb_light_sleep_status_t after = status();
    assert(after.retries == before.retries + 1 && after.recoveries == before.recoveries + 1);
    assert(after.fault == ORB_LIGHT_SLEEP_FAULT_NONE && after.retry_at_us == 0);
    assert(after.retry_delay_ms == 0 && after.failures == before.failures);
    assert(enter_registrations == 1 && exit_registrations == 1);
}
#endif

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *scenario = argv[1];
    levels[0] = levels[11] = 1;
    fail_config = !strcmp(scenario, "config");
    fail_gpio = !strcmp(scenario, "gpio");
    fail_gpio_source = !strcmp(scenario, "gpio_source");
    fail_wifi = !strcmp(scenario, "wifi") || !strcmp(scenario, "persistent");
    fail_callback = !strcmp(scenario, "callback");
    fail_exit = !strcmp(scenario, "exit_callback");
    fail_retain = !strcmp(scenario, "retain");
    fail_create = !strcmp(scenario, "create");
    fail_acquire = !strcmp(scenario, "init_acquire");
    orb_light_sleep_init();
    assert(!atomic_load(&s_allowed));
    assert(!orb_light_sleep_update(asleep()));
    orb_light_sleep_arm(strcmp(scenario, "input") != 0, wake_inputs);
#if !CONFIG_ORB_LIGHT_SLEEP
    assert(!last_config.light_sleep_enable && !skip_callback && !callbacks.enter_cb);
    assert(!orb_light_sleep_update(asleep()));
    return 0;
#else
    if (strcmp(scenario, "normal") && strcmp(scenario, "acquire") && strcmp(scenario, "release") &&
        strcmp(scenario, "tap_pwr") && strcmp(scenario, "tap_boot") &&
        strcmp(scenario, "late_press") && strcmp(scenario, "persistent")) {
        assert(!orb_light_sleep_update(asleep()));
        assert(!atomic_load(&s_allowed));
        if (!strcmp(scenario, "input")) orb_light_sleep_arm(true, wake_inputs);
        recover();
        return 0;
    }
    if (!strcmp(scenario, "persistent")) {
        const uint32_t delays[] = {1000,2000,4000,8000,16000,30000,30000};
        for (unsigned i = 0; i < sizeof(delays)/sizeof(delays[0]); i++) {
            orb_light_sleep_status_t before = status();
            assert(before.retry_delay_ms == delays[i]);
            now_us = before.retry_at_us - 1;
            int work = operations;
            for (int j = 0; j < 100; j++) assert(!orb_light_sleep_update(asleep()));
            assert(operations == work && status().retries == i);
            now_us++;
            assert(!orb_light_sleep_update(asleep()));
            assert(status().retries == i + 1 && skip_callback());
            assert(lock_depth == 1);
        }
        clear_failures();
        now_us = status().retry_at_us;
        assert(orb_light_sleep_update(asleep()));
        assert(status().recoveries == 1 && lock_depth == 0);
        return 0;
    }
    assert(lock_depth == 1 && !skip_callback()); /* preserve idle DFS while ineligible */
    assert_edges();
    assert(enabled[0] && enabled[3] && enabled[11]);
    assert(retained[1] && retained[2] && retained[12] && retained[14] && retained[15] && retained[46]);
    notifications = 0;
    if (!strcmp(scenario, "release")) {
        fail_release = true;
        int config_before = config_calls;
        assert(!orb_light_sleep_update(asleep()));
        assert(lock_depth == 1 && s_policy_lock.held && skip_callback());
        assert(config_calls == config_before); /* no heavyweight configure fault path */
        recover();
        return 0;
    }
    assert(orb_light_sleep_update(asleep()) && lock_depth == 0);
    if (!strcmp(scenario, "acquire")) {
        fail_acquire = true;
        orb_light_sleep_inputs_t usb = asleep(); usb.power.usb = true;
        int config_before = config_calls;
        assert(!orb_light_sleep_update(usb));
        assert(lock_depth == 0 && !s_policy_lock.held && skip_callback());
        assert(config_calls == config_before);
        recover();
        return 0;
    }
    int before = operations;
    for (int i = 0; i < 100; i++) assert(!skip_callback());
    assert(operations == before);
    int arms_before = wake_arm_calls;
    callbacks.enter_cb(100000, NULL);
    assert(skip_callback()); /* actual cross-core armed veto, not a zeroed model */
    assert(wake_arm_calls == arms_before);
    assert(!irq_enabled[0] && !irq_enabled[3] && !irq_enabled[11]);
    assert(types[0] == GPIO_INTR_LOW_LEVEL && types[3] == GPIO_INTR_HIGH_LEVEL);
    if (!strcmp(scenario, "tap_pwr") || !strcmp(scenario, "tap_boot")) {
        release_pin = !strcmp(scenario, "tap_pwr") ? 3 : 0;
        levels[release_pin] = release_pin == 3;
        release_during_restore = true;
        cause = ESP_SLEEP_WAKEUP_GPIO;
        callbacks.exit_cb(50000, NULL);
        assert_edges();
        assert(notifications == 2 && samples[0] == (release_pin == 3 ? 1 : 2) && samples[1] == 0);
        assert(!skip_callback());
        return 0;
    }
    if (!strcmp(scenario, "late_press")) {
        cause = ESP_SLEEP_WAKEUP_TIMER; press_during_restore = true;
        callbacks.exit_cb(90000, NULL);
        assert_edges();
        assert(notifications == 1 && samples[0] == 1 && skip_callback());
        return 0;
    }
    cause = ESP_SLEEP_WAKEUP_TIMER;
    callbacks.exit_cb(90000, NULL);
    assert_edges(); assert(notifications == 0);
    assert(status().entries == 1 && status().slept_us == 90000);
    callbacks.enter_cb(100000, NULL);
    levels[11] = 0; cause = ESP_SLEEP_WAKEUP_GPIO;
    callbacks.exit_cb(50000, NULL);
    assert_edges(); assert(notifications == 2);
    for (int i = 0; i < 100; i++) assert(skip_callback());
    assert(!orb_light_sleep_update(asleep()) && lock_depth == 1);
    levels[11] = 1;
    assert(orb_light_sleep_update(asleep()) && lock_depth == 0);
    callbacks.enter_cb(100000, NULL);
    cause = ESP_SLEEP_WAKEUP_UNDEFINED;
    callbacks.exit_cb(100, NULL);
    assert_edges(); assert(status().entries == 2 && status().slept_us == 140000);
    callbacks.enter_cb(1, NULL); callbacks.exit_cb(0, NULL);
    assert_edges();
    callbacks.enter_cb(1, NULL); levels[3] = 1;
    callbacks.exit_cb(0, NULL);
    assert_edges(); assert(notifications == 4);
    assert(skip_callback()); levels[3] = 0;
    levels[0] = 0; assert(skip_callback()); levels[0] = 1;
    orb_light_sleep_inputs_t usb = asleep(); usb.power.usb = true;
    assert(!orb_light_sleep_update(usb) && lock_depth == 1);
    assert(!skip_callback());
#if CONFIG_ORB_DEVTOOLS
    assert(orb_light_sleep_set_usb_override(true));
    assert(orb_light_sleep_update(usb) && lock_depth == 0);
    assert(orb_light_sleep_set_usb_override(false) && lock_depth == 1);
#endif
    assert(!orb_light_sleep_update(usb));
    assert(orb_light_sleep_update(asleep()));
    callbacks.enter_cb(100000, NULL);
    levels[3] = 1;
    fail_restore = true; cause = ESP_SLEEP_WAKEUP_GPIO;
    callbacks.exit_cb(50000, NULL);
    assert(!irq_enabled[0] && !irq_enabled[3] && !irq_enabled[11]);
    assert(skip_callback());
    assert(!orb_light_sleep_update(asleep()) && lock_depth == 0);
    assert(status().reason == ORB_LIGHT_SLEEP_WAKE_FAILED);
    levels[3] = 0; /* released while restoration fault kept IRQs masked */
    recover();
    assert(samples[notifications - 1] == 0);
    assert(status().entries == 3 && status().slept_us == 190000);
    return 0;
#endif
}
"""


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="orb-light-sleep-") as directory:
        build = Path(directory)
        pins = "\n".join(f"#define GPIO_NUM_{pin} {pin}" for pin in range(49))
        (build / "stub.h").write_text(STUB + pins + "\n")
        for name in (
            "driver/gpio.h",
            "hal/gpio_ll.h",
            "esp_log.h",
            "esp_pm.h",
            "esp_private/pm_impl.h",
            "esp_sleep.h",
            "esp_timer.h",
            "freertos/FreeRTOS.h",
            "freertos/semphr.h",
        ):
            path = build / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "stub.h"\n')
        fixture = build / "test.c"
        fixture.write_text(FIXTURE)
        binary = build / "test"
        command = [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-variable",
            "-I",
            str(build),
            "-I",
            str(ROOT / "main"),
            "-I",
            str(ROOT / "components/orb_power/include"),
            str(fixture),
            "-o",
            str(binary),
        ]
        for options in (
            [],
            ["-DCONFIG_ORB_DEVTOOLS=0"],
            ["-DCONFIG_ORB_LIGHT_SLEEP=0", "-DCONFIG_ORB_DEVTOOLS=0"],
            ["-DCONFIG_ORB_DYNAMIC_FREQUENCY_SCALING=0"],
        ):
            subprocess.run(command + options, check=True)
            for scenario in (
                "normal",
                "config",
                "gpio",
                "gpio_source",
                "wifi",
                "callback",
                "exit_callback",
                "input",
                "retain",
                "create",
                "init_acquire",
                "acquire",
                "release",
                "persistent",
                "tap_pwr",
                "tap_boot",
                "late_press",
            ):
                subprocess.run([str(binary), scenario], check=True)


if __name__ == "__main__":
    main()
