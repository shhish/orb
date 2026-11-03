"""Replay exact fake-HomeRelay frames through the actual link dispatch code."""

import os
import subprocess
import sys
from pathlib import Path

component = Path(__file__).resolve().parents[2]
work = Path(sys.argv[1] if len(sys.argv) > 1 else "build").resolve()
cjson = Path(
    os.environ.get("CJSON_DIR", component.parents[1] / "managed_components/espressif__cjson/cJSON")
).resolve()
model = component.parent / "orb_model"
home = component.parent / "orb_home_data"
home_fixtures = component.parent / "orb_home/tests/fixtures"
text = (component / "orb_link.c").read_text()
home_delivery = text[
    text.index("static bool s_home_pending;") : text.index("static void on_nowplaying(")
]
responses = text[
    text.index("static void on_response(") : text.index("static const char *code_for(")
]
dispatch = text[
    text.index("static void on_event(") : text.index("/* ---- The WebSocket task's side ---- */")
]
prelude = r"""
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_model.h"
#include "link_proto.h"
#include "link_v11.h"
#include "link_home.h"
static bool s_radio=true;
static bool link_radio_negotiated(void) {return s_radio;}
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT 1
#define EXT_MAX_MSG 4096
#define MAX_PENDING 16
#define MAX_HANDLERS 16
#define ORB_LINK_PAIRING 2
#define ORB_LINK_ONLINE 5
#define TAG "test"
#define taskENTER_CRITICAL(p) ((void)(p))
#define taskEXIT_CRITICAL(p) ((void)(p))
typedef void (*orb_link_reply_cb_t)(esp_err_t,const cJSON*,const cJSON*,void*);
typedef void (*orb_link_event_cb_t)(const char*,const cJSON*,void*);
typedef struct {int id; orb_link_reply_cb_t cb; void *ctx;} pending_t;
static pending_t s_pending[MAX_PENDING];
static struct {int state;} s_status={ORB_LINK_ONLINE};
static struct {bool pairing;} s_sess;
typedef struct {const char *name; orb_link_event_cb_t cb; void *ctx;} event_handler_t;
static event_handler_t s_events[MAX_HANDLERS];
static int s_lock,s_n_events;
static bool s_end,s_rx_too_large,s_home=true,s_cards=true,s_usage,s_subscribe_pending;
static bool s_nowplaying, s_weather;
static unsigned checks,warnings,ota_events,ota_requests,unexpected_home_handlers;
static char log_text[4096];
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
static void warn(const char *tag,const char *format,...) {
    (void)tag; warnings++;
    va_list args; va_start(args,format);
    size_t at=strlen(log_text);
    vsnprintf(log_text+at,sizeof(log_text)-at,format,args);
    va_end(args);
}
#define ESP_LOGW warn
static bool orb_link_supports_cards(void) { return s_cards; }
static bool orb_link_supports_home(void) { return s_home; }
static bool orb_link_supports_weather(void) { return s_weather; }
typedef enum {ORB_MEM_MESSAGE_PARSE,ORB_MEM_SNAPSHOT_PARSE,ORB_MEM_CARD_SHOW} orb_mem_phase_t;
static bool orb_link_memory_begin_parse(void) {return true;}
static void orb_link_memory_end_parse(void) {}
static void orb_mem_begin(orb_mem_phase_t p) {(void)p;}
static void orb_mem_end(orb_mem_phase_t p) {(void)p;}
static void orb_mem_end_as(orb_mem_phase_t p,orb_mem_phase_t q) {(void)p;(void)q;}
static const char *error_code(const cJSON *e) {
    const char *code=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e,"code"));
    return code ? code : "internal";
}
static void on_request(const link_msg_t *m) {
    CHECK(m->name && !strcmp(m->name,"ota.offer") && cJSON_IsObject(m->body));
    ota_requests++;
}
static void on_pair_event(const char *name,const cJSON *p) {(void)name;(void)p;}
"""
tests = r"""
static void frame(const char *text) {on_text(text,strlen(text));}
static void ota(const char *name,const cJSON *p,void *ctx) {
    (void)ctx;
    CHECK(!strcmp(name,"ota.available") && cJSON_IsObject(p)); ota_events++;
}
static void unexpected_home(const char *name,const cJSON *p,void *ctx) {
    (void)name;(void)p;(void)ctx; unexpected_home_handlers++;
}
static unsigned fixture(const char *base,const char *name) {
    char path[2048],line[LINK_MAX_MSG+2];
    CHECK(snprintf(path,sizeof(path),"%s/%s.jsonl",base,name)<(int)sizeof(path));
    FILE *f=fopen(path,"r"); CHECK(f!=NULL);
    unsigned lines=0;
    while(fgets(line,sizeof(line),f)) {
        size_t n=strlen(line); CHECK(n && line[n-1]=='\n');
        on_text(line,n-1);
        if(s_end || s_rx_too_large) fprintf(stderr,"fixture %s line %u: %s\n",name,lines+1,log_text);
        CHECK(!s_end && !s_rx_too_large); lines++;
    }
    CHECK(!ferror(f)); fclose(f); return lines;
}
static void snapshot(const char *base) {
    CHECK(fixture(base,"snapshot")==3);
    orb_home_snapshot_t *h=calloc(1,sizeof(*h)); CHECK(h && orb_home_snapshot(h));
    CHECK(h->accessory_count==45 && h->room_count==9 && h->scene_count==12 &&
          h->online && h->synced && h->available);
    free(h);
}
static void pending(int id) {
    CHECK(orb_home_take_request(&s_home_request)); s_home_pending=true;
    s_pending[0]=(pending_t){.id=id,.cb=on_home};
}
static void oversized(const char *prefix,size_t size) {
    char *text=malloc(size+1); CHECK(text!=NULL);
    size_t n=strlen(prefix); CHECK(n+3<=size);
    memcpy(text,prefix,n); memset(text+n,'x',size-n-3);
    memcpy(text+size-3,"\"}}",4); on_text(text,size); free(text);
}
static void independent_traffic(void) {
    frame("{\"t\":\"evt\",\"m\":\"sessions.snapshot\",\"p\":{\"sessions\":[],\"reset\":true,\"more\":false}}");
    frame("{\"t\":\"evt\",\"m\":\"ota.available\",\"p\":{\"version\":\"test-only\"}}");
    frame("{\"t\":\"req\",\"id\":700,\"m\":\"ota.offer\",\"p\":{}}");
    CHECK(!s_end && !s_rx_too_large);
}
int main(int argc,char **argv) {
    CHECK(argc==2 && orb_home_init() && orb_model_init());
    s_events[s_n_events++]=(event_handler_t){.name="ota.available",.cb=ota};
    s_events[s_n_events++]=(event_handler_t){.name="home.snapshot",.cb=unexpected_home};
    orb_home_set_connection(true,false); orb_model_set_connection(true,false);
    frame("{\"t\":\"evt\",\"m\":\"sessions.snapshot\",\"p\":{\"sessions\":[],\"reset\":true,\"more\":false}}");
    snapshot(argv[1]);
    CHECK(!unexpected_home_handlers && !warnings);
    CHECK(fixture(argv[1],"updates")==2);
    orb_home_snapshot_t *h=calloc(1,sizeof(*h)); CHECK(h && orb_home_snapshot(h));
    CHECK(orb_home_find(h,"acc6ec49fcbbf")->state.hue==120 &&
          !orb_home_find(h,"a315febc1153f")->reachable);

    /* Byte-exact HomeBridge accepted states, scene completion and provider
     * error go through the same pending callback as real firmware requests. */
    char path[2048],line[LINK_MAX_MSG+2];
    CHECK(snprintf(path,sizeof(path),"%s/replies.jsonl",argv[1])<(int)sizeof(path));
    FILE *replies=fopen(path,"r"); CHECK(replies!=NULL);
    orb_home_state_t values={.present=ORB_HOME_ON|ORB_HOME_BRIGHTNESS|ORB_HOME_SAT,
        .on=true,.brightness=67,.sat=20};
    CHECK(orb_home_set("acc6ec49fcbbf",&values,false)); pending(101);
    CHECK(fgets(line,sizeof(line),replies)); frame(line);
    CHECK(orb_home_snapshot(h) && orb_home_find(h,"acc6ec49fcbbf")->state.brightness==70 &&
          !orb_home_find(h,"acc6ec49fcbbf")->pending && !s_pending[0].id);
    CHECK(orb_home_toggle("acc6ec49fcbbf")); pending(102);
    CHECK(fgets(line,sizeof(line),replies)); frame(line);
    CHECK(orb_home_snapshot(h) && !orb_home_find(h,"acc6ec49fcbbf")->state.on);
    CHECK(orb_home_scene("s297178749bb1")); pending(103);
    CHECK(fgets(line,sizeof(line),replies)); frame(line);
    CHECK(orb_home_snapshot(h) && !h->scene_pending && !h->error[0]);
    CHECK(orb_home_toggle("acc6ec49fcbbf")); pending(104);
    CHECK(fgets(line,sizeof(line),replies)); frame(line);
    CHECK(orb_home_snapshot(h) && !orb_home_find(h,"acc6ec49fcbbf")->state.on && h->error[0]);
    CHECK(!fgets(line,sizeof(line),replies)); fclose(replies);
    CHECK(!warnings && !s_end);
    CHECK(fixture(argv[1],"doorbell")==1);
    orb_model_snapshot_t *model=calloc(1,sizeof(*model)); CHECK(model && orb_model_snapshot(model));
    CHECK(model->card_count==1 && model->cards[0].kind==ORB_CARD_ATTENTION &&
          !strcmp(model->cards[0].title,"Front Door") && !model->cards[0].reply);
    CHECK(fixture(argv[1],"offline")==3 && orb_home_snapshot(h) && h->online && !h->available);
    CHECK(!orb_home_toggle("acc6ec49fcbbf") && !orb_home_scene("s297178749bb1"));
    CHECK(fixture(argv[1],"empty")==1 && orb_home_snapshot(h) && !h->accessory_count &&
          !h->room_count && !h->scene_count);
    snapshot(argv[1]);

    /* Invalid application content is counted once per frame and logged only
     * on the first rejection of each fixed message type. */
    frame("{\"t\":\"evt\",\"m\":\"home.snapshot\",\"p\":null}");
    CHECK(!s_end && s_home_rejections[0]==1 && warnings==1);
    frame("{\"t\":\"evt\",\"m\":\"home.snapshot\",\"p\":[]}");
    CHECK(!s_end && s_home_rejections[0]==2 && warnings==1);
    CHECK(orb_home_snapshot(h) && !h->synced && !orb_home_toggle("acc6ec49fcbbf"));
    independent_traffic(); CHECK(fixture(argv[1],"doorbell")==1);
    snapshot(argv[1]);
    frame("{\"t\":\"evt\",\"m\":\"home.update\",\"p\":{\"id\":\"acc6ec49fcbbf\",\"state\":{\"hue\":361}}}");
    CHECK(!s_end && s_home_rejections[1]==1 && warnings==2);
    frame("{\"t\":\"evt\",\"m\":\"home.update\",\"p\":{\"id\":\"acc6ec49fcbbf\",\"state\":{\"mode\":\"private-payload\"}}}");
    CHECK(!s_end && s_home_rejections[1]==2 && warnings==2);
    frame("{\"t\":\"evt\",\"m\":\"home.update\",\"p\":{\"id\":\"acc6ec49fcbbf\\u0000suffix\",\"state\":{\"on\":true}}}");
    CHECK(!s_end && s_home_rejections[1]==3 && warnings==2);
    snapshot(argv[1]);
    oversized("{\"t\":\"evt\",\"m\":\"home.snapshot\",\"p\":{\"padding\":\"",4097);
    CHECK(!s_end && !s_rx_too_large && s_home_rejections[0]==3);
    independent_traffic(); snapshot(argv[1]);
    CHECK(orb_home_set("acc6ec49fcbbf",&values,false)); pending(201);
    oversized("{\"t\":\"res\",\"id\":201,\"ok\":true,\"r\":{\"padding\":\"",4097);
    CHECK(!s_end && !s_home_pending && !s_pending[0].id && s_home_rejections[2]==1 && warnings==3);
    CHECK(orb_home_snapshot(h) && !h->synced && !orb_home_find(h,"acc6ec49fcbbf")->pending);
    snapshot(argv[1]); CHECK(orb_home_toggle("acc6ec49fcbbf")); pending(202);
    frame("{\"t\":\"res\",\"id\":202,\"ok\":true,\"r\":[]}");
    CHECK(!s_end && !s_home_pending && !s_pending[0].id && s_home_rejections[3]==1 && warnings==4);
    snapshot(argv[1]); CHECK(orb_home_toggle("acc6ec49fcbbf")); pending(204);
    frame("{\"t\":\"res\",\"id\":204,\"ok\":\"private-payload\"}");
    CHECK(!s_end && !s_home_pending && !s_pending[0].id && s_home_rejections[3]==2 && warnings==4);
    snapshot(argv[1]); CHECK(orb_home_scene("s297178749bb1")); pending(203);
    frame("{\"t\":\"res\",\"id\":203,\"ok\":true,\"r\":{\"id\":\"s297178749bb1\",\"accepted\":false}}");
    CHECK(!s_end && !s_home_pending && s_home_rejections[4]==1 && warnings==5);
    CHECK(!strstr(log_text,"private-payload") && !strstr(log_text,"acc6ec49fcbbf"));
    independent_traffic();

    /* Unnegotiated/future Home events stay safely ignored, including unknown
     * bodies. Oversized non-Home frames and corrupt JSON retain reconnect. */
    snapshot(argv[1]); unsigned before=warnings;
    frame("{\"t\":\"evt\",\"m\":\"home.future\",\"p\":null}");
    s_home=false; frame("{\"t\":\"evt\",\"m\":\"home.snapshot\",\"p\":null}"); s_home=true;
    CHECK(!s_end && warnings==before && orb_home_snapshot(h) && h->synced);
    oversized("{\"t\":\"evt\",\"m\":\"ota.available\",\"p\":{\"padding\":\"",4097);
    CHECK(s_end && s_rx_too_large); s_end=s_rx_too_large=false;
    frame("{\"t\":\"evt\",\"m\":\"home.snapshot\",\"p\":"); CHECK(s_end);
    CHECK(ota_events==3 && ota_requests==3 && !unexpected_home_handlers);
    /* Weather-only links receive replacements without a Cards/Home subscription.
     * Unnegotiated traffic is ignored; bad values never partly replace data. */
    s_end=s_rx_too_large=s_home=s_cards=false;
    orb_model_reset(); orb_model_set_connection(true,true);
    const char *weather="{\"t\":\"evt\",\"m\":\"weather.state\",\"p\":{\"configured\":true,\"available\":true,\"place\":\"New York\",\"unit\":\"F\",\"temperature\":72,\"condition\":\"clear\",\"is_day\":true,\"label\":\"Sunny\",\"high\":75,\"low\":58,\"hourly\":[],\"updated_at\":1793221200,\"stale\":false}}";
    frame(weather);
    CHECK(orb_model_snapshot(model) && !model->weather_valid && !s_end);
    s_weather=true; orb_model_weather_capability(true);
    frame(weather);
    CHECK(!s_end && orb_model_snapshot(model) && model->weather_valid &&
          model->weather.temperature==72 && model->weather.revision==1);
    frame("{\"t\":\"evt\",\"m\":\"weather.state\",\"p\":{\"configured\":true,\"temperature\":999}}");
    CHECK(s_end && orb_model_snapshot(model) && model->weather.temperature==72 && model->weather.revision==1);
    s_end=false;
    oversized("{\"t\":\"evt\",\"m\":\"weather.state\",\"p\":{\"padding\":\"",4097);
    CHECK(s_end && s_rx_too_large && orb_model_snapshot(model) && model->weather.temperature==72);
    s_end=s_rx_too_large=false;
    frame("{\"t\":\"evt\",\"m\":\"weather.state\",\"p\":");
    CHECK(s_end && orb_model_snapshot(model) && model->weather.stale && model->weather.temperature==72);
    const char *duplicates[]={
        "{\"t\":\"evt\",\"t\":\"evt\",\"m\":\"weather.state\",\"p\":{}}",
        "{\"t\":\"evt\",\"m\":\"weather.state\",\"m\":\"ota.available\",\"p\":{}}",
        "{\"t\":\"evt\",\"m\":\"weather.state\",\"p\":{},\"p\":{}}",
        "{\"t\":\"evt\",\"m\":\"weather.state\",\"p\":{},\"id\":1,\"id\":2}"
    };
    for(size_t i=0;i<sizeof(duplicates)/sizeof(*duplicates);i++) {
        s_end=false;
        orb_model_set_connection(true,true); orb_model_weather_capability(true);
        frame(weather); CHECK(!s_end);
        frame(duplicates[i]);
        CHECK(s_end && orb_model_snapshot(model) && model->weather.stale &&
              model->weather.temperature==72 && model->weather.updated_at==1793221200);
    }
    s_cards=s_home=s_weather=false;s_end=s_rx_too_large=false;
    oversized("{\"t\":\"evt\",\"m\":\"power.batch\",\"p\":{\"padding\":\"",EXT_MAX_MSG+1);
    CHECK(s_end&&s_rx_too_large); /* power_hint alone still has a4096-byte bound. */
    s_end=s_rx_too_large=false;s_radio=false;
    unsigned before_unknown=ota_events;
    frame("{\"t\":\"evt\",\"m\":\"power.batch\",\"p\":{\"events\":[{\"m\":\"ota.available\",\"p\":{}}]}}");
    CHECK(!s_end&&ota_events==before_unknown); /* Unnegotiated extension is ignored. */
    s_radio=true;
    s_end=false;
    unsigned before_batch=ota_events;
    frame("{\"t\":\"evt\",\"m\":\"power.batch\",\"p\":{\"events\":[{\"m\":\"ota.available\",\"p\":{}},{\"m\":\"future.event\",\"p\":{}}]}}");
    CHECK(!s_end&&ota_events==before_batch+1);
    before_batch=ota_events;
    frame("{\"t\":\"evt\",\"m\":\"power.batch\",\"p\":{\"events\":[{\"m\":\"ota.available\",\"p\":{}},{\"m\":\"power.batch\",\"p\":{}}]}}");
    CHECK(s_end&&ota_events==before_batch); /* Validate all children before dispatch. */
    s_end=false;
    frame("{\"t\":\"evt\",\"m\":\"power.batch\",\"p\":{\"events\":[{\"m\":\"ota.available\",\"p\":{},\"t\":\"req\"}]}}");
    CHECK(s_end&&ota_events==before_batch);
    free(h); free(model); printf("Home dispatch: %u checks passed\n",checks);
}
"""
work.mkdir(parents=True, exist_ok=True)
source = work / "home_dispatch.c"
source.write_text(prelude + home_delivery + responses + dispatch + tests)
binary = work / "home_dispatch"
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
        "-pthread",
        f"-I{component}",
        f"-I{model / 'include'}",
        f"-I{home / 'include'}",
        f"-I{cjson}",
        str(source),
        str(component / "link_proto.c"),
        str(component / "link_v11.c"),
        str(component / "link_home.c"),
        str(home / "orb_home.c"),
        f"-I{component.parent / 'orb_memory/include'}",
        str(component.parent / "orb_memory/orb_memory.c"),
        str(model / "orb_model.c"),
        str(model / "orb_model_voice_notify.c"),
        str(cjson / "cJSON.c"),
        "-o",
        str(binary),
    ],
    check=True,
)
subprocess.run([str(binary), str(home_fixtures)], check=True)
