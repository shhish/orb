"""Compile boot hello negotiation from production without sockets or tasks."""

import os
import subprocess
import sys
from pathlib import Path

component = Path(__file__).resolve().parents[2]
work = Path(sys.argv[1] if len(sys.argv) > 1 else "build").resolve()
cjson = Path(os.environ["CJSON_DIR"])
text = (component / "orb_link.c").read_text()
hello = text[text.index("static void on_hello(") : text.index("/* ---- Reliable model delivery")]
devtools = (component.parent / "orb_devtools/orb_devtools.c").read_text()
offer = devtools[
    devtools.index("static void wifi_hello(") : devtools.index("static size_t reply_write(")
]
prelude = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "link_proto.h"
typedef int esp_err_t;
typedef int orb_prefs_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ORB_MEM_HELLO 0
#define ORB_LINK_ONLINE 5
#define REPORT_MS 60000
#define POWER_POLL_MS 5000
#define US(ms) ((int64_t)(ms)*1000)
#define TAG "test"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define taskENTER_CRITICAL(p) ((void)(p))
#define taskEXIT_CRITICAL(p) ((void)(p))
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
typedef struct {int state; char channel[16], hub_name[64], device_name[32];} orb_link_status_t;
static orb_link_status_t s_status;
static struct {bool unauthorized,progressed; int hub;} s_sess;
static bool s_end,s_cards,s_devtools,s_home,s_usage,s_nowplaying,s_weather,s_voice,s_voice_available;
static bool s_subscribe_pending;
static int s_lock;
static int64_t s_report_at,s_power_at,s_model_at,s_answer_at;
static unsigned checks,ended,radio_accepts,commits;
static const char *error_code(const cJSON *error) {(void)error;return "unauthorized";}
static const char *esp_err_to_name(int err) {(void)err;return "error";}
static void orb_mem_end(int owner) {(void)owner;ended++;}
static void link_radio_accept(const cJSON *result) {(void)result;radio_accepts++;}
static void orb_net_set_timezone(const char *tz,const char *posix) {(void)tz;(void)posix;}
static void orb_time_set_clock_default(bool hour24) {(void)hour24;}
static void set_clock(int64_t time_ms) {(void)time_ms;}
static void remember_address(const int *hub) {(void)hub;}
static int64_t esp_timer_get_time(void) {return 1000000;}
static bool link_v11_read_prefs(const cJSON *params,orb_prefs_t *out) {(void)params;(void)out;return true;}
static void orb_model_set_prefs(const orb_prefs_t *prefs) {(void)prefs;}
static unsigned orb_home_revision(void) {return 1;}
static void orb_model_set_connection(bool connected,bool synced) {(void)connected;(void)synced;}
static void orb_model_nowplaying_capability(bool enabled) {(void)enabled;}
static void orb_model_weather_capability(bool enabled) {(void)enabled;}
static void orb_home_set_connection(bool connected,bool synced) {(void)connected;(void)synced;}
static void commit(const orb_link_status_t *status) {s_status=*status;commits++;}
static esp_err_t orb_link_status(orb_link_status_t *status) {*status=s_status;return ESP_OK;}
"""
tests = r"""
static void accept(const char *local,const char *channel,bool echo) {
    memset(&s_status,0,sizeof(s_status));
    snprintf(s_status.channel,sizeof(s_status.channel),"%s",local);
    s_end=false;s_sess.progressed=false;
    cJSON *result=cJSON_CreateObject();CHECK(result);
    cJSON *device=cJSON_AddObjectToObject(result,"device");CHECK(device);
    CHECK(cJSON_AddStringToObject(device,"channel",channel));
    CHECK(cJSON_AddStringToObject(device,"name","Orb"));
    CHECK(cJSON_AddStringToObject(result,"hub_name","Test Hub"));
    CHECK(cJSON_AddStringToObject(result,"hub_version","test"));
    cJSON *caps=cJSON_AddArrayToObject(result,"caps");CHECK(caps);
    if(echo)CHECK(cJSON_AddItemToArray(caps,cJSON_CreateString("devtools")));
    on_hello(ESP_OK,result,NULL,NULL);
    CHECK(!s_end && s_status.state==ORB_LINK_ONLINE && s_sess.progressed);
    CHECK(s_devtools==(CONFIG_ORB_DEVTOOLS && echo && !strcmp(channel,"dev")));
    cJSON_Delete(result);
}
int main(void) {
#if CONFIG_ORB_DEVTOOLS
    /* Support is available before the first authoritative channel arrives. */
    const char *channels[]={"","stable","dev"};
    for(unsigned i=0;i<3;i++) {
        snprintf(s_status.channel,sizeof(s_status.channel),"%s",channels[i]);
        cJSON *request=cJSON_Parse("{\"caps\":[]}");CHECK(request);
        wifi_hello(request,NULL);
        const cJSON *caps=cJSON_GetObjectItemCaseSensitive(request,"caps");
        CHECK(cJSON_GetArraySize(caps)==1);
        CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetArrayItem(caps,0)),"devtools"));
        cJSON_Delete(request);
    }
#endif
    accept("","dev",true);
    accept("stable","dev",true);
    accept("dev","dev",true);
    accept("","stable",false);
    accept("dev","stable",true); /* A wrong echo still cannot grant access. */
    accept("","dev",false); /* An older hub may ignore this capability. */
    CHECK(ended==6 && radio_accepts==6 && commits==6);
    printf("boot hello CONFIG=%d: %u checks passed\n",CONFIG_ORB_DEVTOOLS,checks);
    return 0;
}
"""
work.mkdir(parents=True, exist_ok=True)
for enabled in (0, 1):
    source = work / f"hello-{enabled}.c"
    source.write_text(prelude + (offer if enabled else "") + hello + tests)
    output = work / f"hello-{enabled}"
    subprocess.run(
        [
            "clang",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-parameter",
            "-Wno-unused-function",
            "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all",
            f"-DCONFIG_ORB_DEVTOOLS={enabled}",
            f"-I{component}",
            f"-I{cjson}",
            str(source),
            str(component / "link_proto.c"),
            str(cjson / "cJSON.c"),
            "-Wno-deprecated-declarations",
            "-o",
            str(output),
        ],
        check=True,
    )
    subprocess.run([str(output)], check=True)
