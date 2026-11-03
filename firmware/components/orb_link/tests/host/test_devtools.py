"""Compile the production request router/admission and output path with fakes."""

import os
import subprocess
import sys
from pathlib import Path

component = Path(__file__).resolve().parents[2]
work = Path(sys.argv[1] if len(sys.argv) > 1 else "build").resolve()
cjson = Path(os.environ["CJSON_DIR"])
text = (component / "orb_link.c").read_text()
devtools = component.parent / "orb_devtools"
source = (devtools / "orb_devtools.c").read_text()
admission = source[
    source.index("static esp_err_t wifi_request(") : source.index("static void wifi_hello(")
]
router = text[text.index("static const char *code_for(") : text.index("static void on_pair_event(")]
capability = text[
    text.index("bool orb_link_supports_devtools(void)\n{") : text.index(
        "esp_err_t orb_link_unpair("
    )
]
event = text[text.index("    case MSG_EVENT: {") : text.index("    case MSG_AUDIO:")]
prelude = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_devtools_wire.h"
#include "link_proto.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NOT_FOUND 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_NO_MEM 4
#define ESP_ERR_TIMEOUT 5
#define ORB_LINK_ONLINE 5
#define EXT_MAX_MSG 4096
#define pdTRUE 1
#define MSG_EVENT 1
#define TAG "test"
#define ESP_LOGW(...) ((void)0)
#define taskENTER_CRITICAL(lock) ((void)(lock))
#define taskEXIT_CRITICAL(lock) ((void)(lock))
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
typedef struct {int state; char channel[16];} orb_link_status_t;
static orb_link_status_t s_status={ORB_LINK_ONLINE,"dev"};
static bool s_devtools=true;
static int s_lock,s_wifi_lock,s_executor=1,s_command_task=1;
static bool s_bench_active;
static uint32_t s_last_generation,s_last_stream,generation=7;
static struct {uint32_t stream,generation; int64_t deadline; char command[96]; bool pending;} s_wifi;
static int64_t now=1000;
static unsigned checks,held,releases,executions,callbacks,wakes;
static int last_callback;
static bool transport_result=true,s_up=true,s_peer_closed,s_end;
static char last_error[32];
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
static uint32_t orb_link_generation(void) { return generation; }
static int64_t esp_timer_get_time(void) { return now; }
static void xTaskNotifyGive(int task) { CHECK(task==s_command_task); wakes++; }
static int xSemaphoreTake(int sem,int wait) {(void)sem;(void)wait; if(held) return 0; held=1; return pdTRUE;}
static void xSemaphoreGive(int sem) {(void)sem;held=0;releases++;}
static const char *esp_err_to_name(int err) {(void)err;return "test";}
static void commit(const orb_link_status_t *st) {s_status=*st;}
static bool orb_link_supports_devtools(void);
typedef esp_err_t (*method_cb_t)(const cJSON*,cJSON*,void*);
typedef struct {const char *name;method_cb_t cb;void *ctx;} method_handler_t;
static method_handler_t s_methods[3];
static int s_n_methods=3;
static struct {bool reboot;} s_sess;
static void close_link(void) {CHECK(false);}
static void esp_restart(void) {CHECK(false);}
static void send_text(char *out) {
    CHECK(out!=NULL);
    cJSON *reply=cJSON_Parse(out);CHECK(reply!=NULL);
    const char *code=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(reply,"e"),"code"));
    snprintf(last_error,sizeof(last_error),"%s",code ? code : "");
    cJSON_Delete(reply);cJSON_free(out);
}
static void done_cb(esp_err_t err,const cJSON *r,const cJSON *e,void *ctx) {
    (void)r;(void)e;(void)ctx;callbacks++;last_callback=err;
}
typedef struct {int kind;int64_t deadline;cJSON *body;const char *name;void (*cb)(esp_err_t,const cJSON*,const cJSON*,void*);void *ctx;} fake_message_t;
static bool send_text_until(char *encoded,int64_t deadline) {
    bool ok=transport_result && deadline>now;cJSON_free(encoded);return ok;
}
static void send_event(const char *name,cJSON *body) {(void)name;cJSON_Delete(body);}
"""
tests = r"""
static esp_err_t admitted(const cJSON *params,cJSON *result,void *ctx) {
    esp_err_t err=wifi_request(params,result,ctx);
    if(err==ESP_OK) executions++;
    return err;
}
static esp_err_t config(const cJSON *params,cJSON *result,void *ctx) {(void)params;(void)result;(void)ctx;return ESP_OK;}
static void request(const char *name,const char *params) {
    cJSON *body=cJSON_Parse(params);
    link_msg_t request={.kind=LINK_REQ,.id=4,.name=name,.body=body};
    on_request(&request);cJSON_Delete(body);
}
static void process(fake_message_t *m,bool current) {
    switch(m->kind) {
EVENT_CASE
    }
}
int main(void) {
    s_methods[0]=(method_handler_t){"devtools.tap",admitted,"devtools.tap"};
    s_methods[1]=(method_handler_t){"devtools.shot",admitted,"devtools.shot"};
    s_methods[2]=(method_handler_t){"device.config",config,NULL};
    CHECK(orb_link_supports_devtools()==!!CONFIG_ORB_DEVTOOLS);
    request("devtools.tap","{\"stream\":1,\"args\":[12,20]}");
#if CONFIG_ORB_DEVTOOLS
    CHECK(!last_error[0] && executions==1 && held==1 && s_wifi.pending);
    CHECK(wakes==1);
    CHECK(s_wifi.generation==7 && s_wifi.stream==1 && !strcmp(s_wifi.command,"tap 12 20"));
    CHECK(s_wifi.deadline==now+ORB_DEVTOOLS_DEADLINE_US);
    request("devtools.tap","{\"stream\":2,\"args\":[12,20]}");CHECK(!strcmp(last_error,"busy") && executions==1);
    CHECK(wakes==1); /* Rejected requests cannot wake the command owner. */
    held=0;s_bench_active=true;
    request("devtools.tap","{\"stream\":2,\"args\":[12,20]}");CHECK(!strcmp(last_error,"busy") && !held);
    s_bench_active=false;
    request("devtools.tap","{\"stream\":1,\"args\":[12,20]}");CHECK(!strcmp(last_error,"bad_request"));
    request("devtools.tap","{\"stream\":2,\"args\":[false,20]}");CHECK(!strcmp(last_error,"bad_request") && !held);
    generation++;
    request("devtools.shot","{\"stream\":1,\"args\":[]}");CHECK(!last_error[0] && executions==2);
    held=0;
    request("devtools.unknown","{\"stream\":2,\"args\":[]}");CHECK(!strcmp(last_error,"unknown_method"));
    s_devtools=false;
    request("devtools.shot","{\"stream\":2,\"args\":[]}");CHECK(!strcmp(last_error,"unsupported"));
    s_devtools=true;
    request("device.config","{\"channel\":\"stable\"}");CHECK(!last_error[0] && !s_devtools && !strcmp(s_status.channel,"stable"));
    request("devtools.shot","{\"stream\":2,\"args\":[]}");CHECK(!strcmp(last_error,"unauthorized") && executions==2);
    request("device.config","{\"channel\":\"dev\"}");CHECK(!s_devtools);
    request("devtools.shot","{\"stream\":2,\"args\":[]}");CHECK(!strcmp(last_error,"unsupported"));
#else
    CHECK(!strcmp(last_error,"unsupported") && !executions && !held);
#endif
    s_devtools=true;snprintf(s_status.channel,sizeof(s_status.channel),"dev");
    for(unsigned scenario=0;scenario<6;scenario++) {
        fake_message_t m={.kind=MSG_EVENT,.deadline=now+100,.name="devtools.output",.body=cJSON_CreateObject(),.cb=done_cb};
        s_end=false;s_peer_closed=false;s_up=true;transport_result=true;
        if(scenario==1) m.deadline=now;
        if(scenario==2) transport_result=false;
        if(scenario==3) s_peer_closed=true;
        if(scenario==4) snprintf(s_status.channel,sizeof(s_status.channel),"stable");
        process(&m,scenario!=5);
        CHECK(callbacks==scenario+1);
        CHECK(last_callback==((scenario==0 && CONFIG_ORB_DEVTOOLS) ? ESP_OK : ESP_ERR_TIMEOUT));
        snprintf(s_status.channel,sizeof(s_status.channel),"dev");
    }
    s_status.state=0;
    request("devtools.shot","{\"stream\":3,\"args\":[]}");CHECK(!strcmp(last_error,"busy"));
    printf("devtools routing CONFIG=%d: %u checks passed\n",CONFIG_ORB_DEVTOOLS,checks);
    return 0;
}
""".replace("EVENT_CASE", event)
work.mkdir(parents=True, exist_ok=True)
for enabled in (0, 1):
    path = work / f"devtools-routing-{enabled}.c"
    path.write_text(
        prelude.replace(
            "static bool orb_link_supports_devtools(void);",
            "bool orb_link_supports_devtools(void);",
        )
        + admission
        + capability
        + router
        + tests
    )
    binary = path.with_suffix("")
    subprocess.run(
        [
            "clang",
            "-std=c11",
            "-O1",
            "-g",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-variable",
            "-Wno-deprecated-declarations",
            "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all",
            f"-DCONFIG_ORB_DEVTOOLS={enabled}",
            f"-I{devtools}",
            f"-I{component}",
            f"-I{cjson}",
            str(path),
            str(component / "link_proto.c"),
            str(cjson / "cJSON.c"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
