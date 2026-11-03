"""Exercise actual radio policy and report publication without tasks or sockets."""

import os
import subprocess
import sys
from pathlib import Path

component = Path(__file__).resolve().parents[2]
work = Path(sys.argv[1] if len(sys.argv) > 1 else "build").resolve() / "radio"
cjson = Path(
    os.environ.get("CJSON_DIR", component.parents[1] / "managed_components/espressif__cjson/cJSON")
).resolve()
work.mkdir(parents=True, exist_ok=True)
(work / "freertos").mkdir(exist_ok=True)
(work / "freertos/FreeRTOS.h").write_text(
    "typedef int portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED 0\n"
    "#define taskENTER_CRITICAL(p) ((void)(p))\n#define taskEXIT_CRITICAL(p) ((void)(p))\n"
)
(work / "esp_timer.h").write_text("#include <stdint.h>\nint64_t esp_timer_get_time(void);\n")
(work / "orb_net.h").write_text(
    "#pragma once\n#include <stdbool.h>\n"
    "typedef enum {ORB_NET_POWER_INTERACTIVE,ORB_NET_POWER_BACKGROUND} orb_net_power_hint_t;\n"
    "orb_net_power_hint_t orb_net_get_power_hint(void);\n"
    "uint32_t orb_net_interactive_generation(void);\n"
    "void orb_net_on_power_hint(void (*cb)(void *),void *ctx);\n"
)
(work / "orb_link.h").write_text(
    "#pragma once\n#include <stdint.h>\ntypedef int esp_err_t;\n"
    "#define ESP_OK 0\n#define ESP_ERR_INVALID_ARG 1\n"
    "typedef struct {uint64_t messages_rx,messages_tx,bytes_rx,bytes_tx;"
    "uint32_t connects,reconnects;} orb_link_counters_t;\n"
    "esp_err_t orb_link_counters(orb_link_counters_t *out);\n"
    "void orb_link_set_sleep_eligible(bool eligible);\n"
)
text = (component / "orb_link.c").read_text()
report = text[text.index("static void send_report(void)") : text.index("/* ---- What the device")]
wait = text[
    text.index("        uint32_t delay = link_radio_wait_begin(") : text.index(
        "        link_radio_wait_end();"
    )
    + len("        link_radio_wait_end();")
]
prelude = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "link_radio.h"
#include "link_proto.h"
#include "orb_link.h"
#include "orb_net.h"
static unsigned checks,wakes,ping=15;
static int64_t now;
static bool bg,fail_write,fail_alloc,s_end,scheduled_wake;
static unsigned steps;
static uint32_t generation;
static void (*observer)(void *);
static void *observer_ctx;
static bool s_have_power;
static link_power_t s_power;
static int s_ws;
static int64_t s_report_at;
#define CHECK(c) do {checks++;if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while(0)
#define REPORT_MS 60000
#define US(ms) ((int64_t)(ms)*1000)
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2
int64_t esp_timer_get_time(void) {return now;}
orb_net_power_hint_t orb_net_get_power_hint(void) {return bg?ORB_NET_POWER_BACKGROUND:ORB_NET_POWER_INTERACTIVE;}
void orb_net_on_power_hint(void (*cb)(void *),void *ctx) {observer=cb;observer_ctx=ctx;}
static void wake(void) {wakes++;}
uint32_t orb_net_interactive_generation(void) {return generation;}
static void hint(bool background) {if(bg&&!background)generation++;bg=background;observer(observer_ctx);}
static uint32_t esp_random(void) {return 0;}
static void step(int64_t until) {
    steps++;
    if (scheduled_wake) {scheduled_wake=false;now+=1000;hint(false);}
    else now=until;
}
static int rssi(void) {return -50;}
static unsigned heap_caps_get_free_size(int cap) {(void)cap;return 10000;}
static unsigned uptime_s(void) {return (unsigned)(now/1000000);}
static bool link_crash_report(cJSON *report) {(void)report;return true;}
static void link_crash_report_sent(void) {}
static void esp_websocket_client_set_ping_interval_sec(int ws,unsigned interval) {(void)ws;ping=interval;}
static bool send_event(const char *name,cJSON *params) {
    CHECK(!strcmp(name,"state.report"));
    cJSON_Delete(params);
    return !fail_write;
}
static void *test_malloc(size_t bytes) {return fail_alloc ? NULL : malloc(bytes);}
"""
tests = r"""
static void accept(const char *caps) {
    cJSON *result=cJSON_Parse(caps);CHECK(result);
    link_radio_accept(result);cJSON_Delete(result);
}
int main(void) {
    cJSON_Hooks hooks={test_malloc,free};cJSON_InitHooks(&hooks);
    link_radio_init(wake);CHECK(observer);
    CHECK(link_radio_model_poll_ms()==50&&!link_radio_model_prompt());
    orb_link_set_sleep_eligible(true);CHECK(wakes==2);
    CHECK(link_radio_model_poll_ms()==1000&&!link_radio_model_prompt());
    orb_link_set_sleep_eligible(true);CHECK(wakes==2);
    orb_link_set_sleep_eligible(false);CHECK(wakes==3);
    CHECK(link_radio_model_poll_ms()==50&&link_radio_model_prompt());
    CHECK(!link_radio_model_prompt());
    orb_link_set_sleep_eligible(true);orb_link_set_sleep_eligible(false);
    orb_link_set_sleep_eligible(true); /* Quick wake cannot be lost. */
    CHECK(link_radio_model_prompt());
    orb_link_set_sleep_eligible(false);CHECK(link_radio_model_prompt());
    wakes=1;
    hint(true);CHECK(wakes==2);
    CHECK(link_radio_wait_begin(9,0)==300000);
    now=1000000;hint(false);CHECK(link_radio_wait_prompt());
    CHECK(!link_radio_wait_prompt());link_radio_wait_end();
    CHECK(link_radio_wait_begin(9,0)==30000);
    hint(true);now=29999000;hint(false);CHECK(!link_radio_wait_prompt());
    hint(true);now=31000000;hint(false);CHECK(link_radio_wait_prompt());
    link_radio_wait_end();
    /* OTA/voice use the same effective interactive hint as a user wake. */
    hint(true);link_radio_wait_begin(9,0);now=62000000;hint(false);
    CHECK(link_radio_wait_prompt());link_radio_wait_end();
    hint(true);now=100000000;steps=0;scheduled_wake=true;do_wait(9);
    CHECK(steps==1&&now==100001000); /* A queue wake cuts the real waiter short. */
    hint(true);now=101000000;steps=0;scheduled_wake=true;do_wait(9);
    CHECK(steps==2&&now==131001000); /* Throttled wake restores the30s ceiling. */
    hint(true);now=500000000;link_radio_wait_begin(9,0);
    bg=false;generation++;bg=true;observer(observer_ctx);
    CHECK(link_radio_wait_prompt()); /* Transient interactive survives sampling. */
    link_radio_wait_end();
    hint(true);link_radio_reset();CHECK(link_radio_ping_s()==15);
    cJSON *hello=cJSON_Parse("{\"caps\":[]}");CHECK(link_radio_hello(hello));
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(hello,"caps"))==1);
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(hello,"power_hint")),"background"));
    CHECK(cJSON_GetObjectItemCaseSensitive(hello,"ping_interval_s")->valueint==15);
    cJSON_Delete(hello);
    accept("{\"caps\":[]}");CHECK(!link_radio_negotiated()&&!link_radio_changed());
    send_report();CHECK(ping==15&&!s_end);
    accept("{\"caps\":[\"power_hint\",\"future\"]}");CHECK(link_radio_changed());
    CHECK(link_radio_ping_s()==15);
    /* Failed allocation cannot advertise or apply the slower cadence. */
    fail_alloc=true;send_report();fail_alloc=false;
    CHECK(s_end&&link_radio_ping_s()==15&&link_radio_changed());s_end=false;
    fail_write=true;send_report();fail_write=false;
    CHECK(s_end&&ping==15&&link_radio_changed());s_end=false;
    send_report();CHECK(ping==45&&!s_end&&!link_radio_changed());
    hint(false);CHECK(link_radio_changed());send_report();CHECK(ping==15&&!link_radio_changed());
    /* Reset is per transport: a subsequent older hub never inherits 45 s. */
    link_radio_reset();CHECK(link_radio_ping_s()==15&&!link_radio_negotiated());
    link_radio_rx(17);link_radio_rx(1031);link_radio_tx(23);link_radio_connect();link_radio_connect();
    orb_link_counters_t counts;CHECK(orb_link_counters(&counts)==ESP_OK);
    CHECK(counts.messages_rx==2&&counts.messages_tx==1&&counts.bytes_rx==1048&&counts.bytes_tx==23);
    CHECK(counts.connects==2&&counts.reconnects==1);
    CHECK(orb_link_counters(NULL)==ESP_ERR_INVALID_ARG);
    printf("link radio: %u checks passed\n",checks);
}
"""
source = work / "radio_test.c"
source.write_text(
    prelude + report + "static void do_wait(unsigned waits) {\n" + wait + "\n}\n" + tests
)
binary = work / "radio_test"
subprocess.run(
    [
        "clang",
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-deprecated-declarations",
        "-O1",
        "-g",
        "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all",
        f"-I{work}",
        f"-I{component}",
        f"-I{cjson}",
        str(source),
        str(component / "link_radio.c"),
        str(component / "link_backoff.c"),
        str(component / "link_proto.c"),
        str(cjson / "cJSON.c"),
        "-o",
        str(binary),
    ],
    check=True,
)
subprocess.run([str(binary)], check=True)
