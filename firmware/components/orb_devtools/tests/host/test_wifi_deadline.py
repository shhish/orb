"""Drive the production Wi-Fi completion wait against explicit/global deadlines."""

import subprocess
import sys
from pathlib import Path

component = Path(__file__).resolve().parents[2]
work = Path(sys.argv[1] if len(sys.argv) > 1 else "build").resolve()
source = (component / "orb_devtools.c").read_text()
wait = source[
    source.index("static void wifi_written(") : source.index("static bool wifi_text_until(")
]
prelude = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"
#define ESP_OK 0
#define pdMS_TO_TICKS(ms) (ms)
typedef int esp_err_t;
static int s_output_done, s_wifi_lock;
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
static struct {uint32_t generation,ticket,delivered;int64_t deadline;bool delivery_ok,failed;} s_wifi;
static int64_t now, ack_at, posted_deadline;
static uint32_t pending_ticket;
static unsigned checks, sends;
static bool fail_write;
#define CHECK(c) do {checks++;if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
static int64_t esp_timer_get_time(void) {return now;}
static bool wifi_current(void) {return now<s_wifi.deadline;}
static void xSemaphoreGive(int sem) {(void)sem;}
static void wifi_written(esp_err_t err,const cJSON *result,const cJSON *error,void *ctx);
static int xSemaphoreTake(int sem,int ms) {
    (void)sem;
    /* A zero-tick poll still costs CPU time on the real monotonic clock. */
    now+=ms ? ms*1000 : 1;
    if(pending_ticket && now>=ack_at) {
        uint32_t ticket=pending_ticket;pending_ticket=0;
        wifi_written(fail_write ? -1 : ESP_OK,NULL,NULL,(void *)(uintptr_t)ticket);
    }
    return 1;
}
static esp_err_t orb_link_devtools_notify_until(uint32_t generation,const char *name,cJSON *params,int64_t deadline_us,
    void (*callback)(esp_err_t,const cJSON *,const cJSON *,void *),void *ctx) {
    (void)callback;CHECK(generation==7);CHECK(name!=NULL);CHECK(deadline_us>now);cJSON_Delete(params);
    sends++;pending_ticket=(uint32_t)(uintptr_t)ctx;posted_deadline=deadline_us;
    return ESP_OK;
}
"""
tests = r"""
static void reset(void) {
    now=1000000;ack_at=INT64_MAX;posted_deadline=0;pending_ticket=0;sends=0;fail_write=false;
    s_wifi.generation=7;s_wifi.deadline=now+90000000;s_wifi.failed=false;
}
int main(void) {
    reset();int64_t deadline=now+75000;
    CHECK(!wifi_event_until("devtools.output",cJSON_CreateObject(),deadline));
    CHECK(now==deadline && sends==1);
    CHECK(posted_deadline==deadline);
    reset();s_wifi.deadline=now+90000;deadline=s_wifi.deadline;
    CHECK(!wifi_event_until("devtools.output",cJSON_CreateObject(),now+2000000));
    CHECK(now==deadline && sends==1);
    CHECK(posted_deadline==deadline);
    reset();s_wifi.deadline=now+90000;deadline=s_wifi.deadline;
    CHECK(!wifi_event("devtools.done",cJSON_CreateObject()));
    CHECK(now==deadline && sends==1 && posted_deadline==deadline);
    reset();deadline=now+6000001;
    CHECK(!wifi_event("devtools.done",cJSON_CreateObject()));
    CHECK(now==deadline && sends==1);
    reset();ack_at=now+35000;
    CHECK(wifi_event_until("devtools.output",cJSON_CreateObject(),now+75000));
    CHECK(now<1075000 && sends==1 && !s_wifi.failed);
    reset();ack_at=now+35000;fail_write=true;
    CHECK(!wifi_event_until("devtools.output",cJSON_CreateObject(),now+75000));
    CHECK(s_wifi.failed && sends==1);
    reset();deadline=now;
    CHECK(!wifi_event_until("devtools.output",cJSON_CreateObject(),deadline));
    CHECK(now==deadline && !sends);
    printf("devtools Wi-Fi deadlines: %u checks passed (receipt, stream, event, delivery failure)\n",checks);
    return 0;
}
"""
work.mkdir(parents=True, exist_ok=True)
test = work / "test_wifi_deadline.c"
test.write_text(prelude + wait + tests)
cjson = component.parents[1] / "managed_components/espressif__cjson/cJSON"
binary = work / "test_wifi_deadline"
subprocess.run(
    [
        "clang",
        "-std=c11",
        "-O1",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-deprecated-declarations",
        "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all",
        f"-I{cjson}",
        str(test),
        str(cjson / "cJSON.c"),
        "-o",
        str(binary),
    ],
    check=True,
)
subprocess.run([str(binary)], check=True)
