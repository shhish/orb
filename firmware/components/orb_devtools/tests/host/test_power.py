"""Exercise the production RAM-only command/report and remote parser without I/O."""

import ast
import subprocess
import tempfile
from pathlib import Path

COMPONENT = Path(__file__).resolve().parents[2]
ROOT = COMPONENT.parents[2]
source = (COMPONENT / "orb_devtools_power.c").read_text()
fixture = r"""
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CONFIG_PM_ENABLE 1
#define CONFIG_ORB_DEVTOOLS 1
#define ESP_OK 0
#include "orb_devtools_power.h"
#include "orb_light_sleep.h"
typedef int esp_err_t;
typedef struct { uint64_t frames; } orb_display_counters_t;
typedef struct { int64_t at_us; unsigned pct; } orb_board_brightness_write_t;
typedef struct { int min_freq_mhz, max_freq_mhz; bool light_sleep_enable; } esp_pm_config_t;
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define heap_caps_malloc(size, caps) malloc(size)
#define heap_caps_free(pointer) free(pointer)
static char s_output[8192];
static size_t s_used;
static unsigned s_sets, s_dumps;
static bool s_override, s_fault;
static int64_t s_now = 500000;
static int64_t esp_timer_get_time(void) { return s_now; }
static int output(const char *format, ...) {
    va_list args; va_start(args, format);
    int size = vsnprintf(s_output + s_used, sizeof(s_output) - s_used, format, args);
    va_end(args); assert(size >= 0 && (size_t)size < sizeof(s_output) - s_used);
    s_used += size; return size;
}
static bool reply(const char *format, ...) {
    va_list args; va_start(args, format);
    int size = vsnprintf(s_output + s_used, sizeof(s_output) - s_used, format, args);
    va_end(args); assert(size >= 0 && (size_t)size < sizeof(s_output) - s_used);
    s_used += size; return true;
}
static void orb_display_counters(orb_display_counters_t *out) { out->frames = 123; }
static bool orb_display_panel_ready(bool awake) { return !awake; }
static unsigned orb_board_motion_mode(void) { return 3; }
static unsigned orb_power_board_poll_period(void) { return 100; }
static size_t orb_board_brightness_history(orb_board_brightness_write_t *out, size_t n) {
    assert(n == 48); out[0] = (orb_board_brightness_write_t){1000000, 0}; return 1;
}
void orb_light_sleep_status(orb_light_sleep_status_t *out) {
    *out = (orb_light_sleep_status_t){.eligible=true, .wake_ready=true,
        .reason=s_fault ? ORB_LIGHT_SLEEP_WAKE_FAILED : ORB_LIGHT_SLEEP_ALLOWED,
        .entries=4321, .slept_us=987654321, .fault=s_fault ? ORB_LIGHT_SLEEP_FAULT_RESTORE : ORB_LIGHT_SLEEP_FAULT_NONE,
        .last_error=s_fault ? -1 : 0, .failures=s_fault ? 3 : 0, .retries=s_fault ? 2 : 0,
        .recoveries=s_fault ? 1 : 0, .retry_at_us=s_fault ? 2500000 : 0};
    if (s_fault) out->eligible = out->wake_ready = false;
}
const char *orb_light_sleep_reason_name(orb_light_sleep_reason_t reason) {
    return reason == ORB_LIGHT_SLEEP_ALLOWED ? "eligible" : "wake_failed";
}
const char *orb_light_sleep_fault_name(orb_light_sleep_fault_t fault) {
    return fault == ORB_LIGHT_SLEEP_FAULT_NONE ? "none" : "restore";
}
bool orb_light_sleep_set_usb_override(bool allowed) { s_sets++; s_override = allowed; return true; }
static int esp_pm_get_configuration(esp_pm_config_t *out) {
    *out = (esp_pm_config_t){80,240,true}; return ESP_OK;
}
static int esp_pm_dump_locks(FILE *out) { assert(out == stdout); s_dumps++; return ESP_OK; }
static const char *esp_err_to_name(int err) { (void)err; return "stub"; }
"""
fixture += source[source.index("void orb_devtools_power_report(") :]
fixture += r"""
int main(void) {
    assert(!s_override);
    assert(orb_devtools_lightsleep_command("lightsleep usb on", reply));
    assert(s_override && s_sets == 1);
    assert(orb_devtools_lightsleep_command("lightsleep usb off", reply));
    assert(!s_override && s_sets == 2);
    const char *bad[] = {"lightsleep usb on extra", "lightsleep usb 1", "lightsleep battery on",
                         "lightsleep usb on\n", "lightsleep usb", "lightsleep  usb on"};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++)
        assert(!orb_devtools_lightsleep_command(bad[i], reply));
    assert(s_sets == 2);
    orb_devtools_power_report(stdout, output);
    assert(s_dumps == 1);
    assert(strstr(s_output,"sleep_eligible=1 sleep_block=eligible wake_ready=1"));
    assert(strstr(s_output,"sleep_entries=4321 sleep_us=987654321"));
    assert(strstr(s_output,"min_mhz=80 max_mhz=240 light_sleep=1"));
    assert(strstr(s_output,"poll_ms=100 frames_since_report=123 frames_total=123"));
    assert(strstr(s_output,"sleep_fault=none sleep_error=0 sleep_failures=0 sleep_retries=0 sleep_recoveries=0 sleep_retry_ms=0"));
    s_fault = true; s_used = 0; s_output[0] = 0;
    orb_devtools_power_report(stdout, output);
    assert(strstr(s_output,"sleep_eligible=0 sleep_block=wake_failed wake_ready=0"));
    assert(strstr(s_output,"sleep_fault=restore sleep_error=-1 sleep_failures=3 sleep_retries=2 sleep_recoveries=1 sleep_retry_ms=2000"));
    s_now = 3000000; s_used = 0; s_output[0] = 0;
    orb_devtools_power_report(stdout, output);
    assert(strstr(s_output,"sleep_retry_ms=0"));
    puts("devtools power: strict RAM-only USB command and measured-residency receipt passed");
}
"""
with tempfile.TemporaryDirectory(prefix="orb-devtools-power-") as temporary:
    directory = Path(temporary)
    path = directory / "power.c"
    path.write_text(fixture)
    binary = directory / "power"
    subprocess.run(
        [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-I",
            str(COMPONENT),
            "-I",
            str(COMPONENT.parent / "orb_power/include"),
            str(path),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)

# Compile only the pure command parser: importing the transport would load serial.
module = ast.parse((ROOT / "scripts/orb_dev_remote.py").read_text())
parser = next(
    item
    for item in module.body
    if isinstance(item, ast.FunctionDef) and item.name == "command_timeout"
)
namespace = {}
exec(compile(ast.Module(body=[parser], type_ignores=[]), "remote_parser", "exec"), namespace)
for enabled in ("on", "off"):
    assert namespace["command_timeout"](f"lightsleep usb {enabled}") == 10
for bad in (
    "lightsleep usb 1",
    "lightsleep battery on",
    "lightsleep usb on extra",
    "lightsleep  usb on",
):
    try:
        namespace["command_timeout"](bad)
    except ValueError:
        pass
    else:
        raise AssertionError(bad)
