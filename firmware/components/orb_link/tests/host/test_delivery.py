"""Exercise the actual link delivery callbacks with a fake request transport."""

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
text = (component / "orb_link.c").read_text()
delivery = text[
    text.index("/* ---- Reliable model delivery") : text.index("/* ---- Messages from the hub")
]
prelude = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "orb_model.h"
#include "link_v11.h"
#include "link_home.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT 1
#define REQUEST_MS 10000
static uint32_t link_radio_model_poll_ms(void) { return 50; }
#define US(ms) ((int64_t)(ms)*1000)
#define TAG "test"
static void warn(const char *tag,const char *format,...) { (void)tag; (void)format; }
#define ESP_LOGW warn
typedef void (*orb_link_reply_cb_t)(esp_err_t,const cJSON*,const cJSON*,void*);
static bool s_end, s_subscribe_pending, s_answer_pending, s_message_pending, s_home, cards=true;
static int64_t s_model_at, s_answer_at, now;
static orb_answer_t s_answer;
static orb_message_t s_message;
static bool s_nowplaying, s_media_pending, s_weather;
static orb_media_request_t s_media_request;
static unsigned checks, sent;
static struct { char method[32]; cJSON *body; orb_link_reply_cb_t cb; } requests[64];
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
static int64_t esp_timer_get_time(void) { return now; }
static bool orb_link_supports_cards(void) { return cards; }
static uint32_t orb_link_generation(void) { return 7; }
static const char *error_code(const cJSON *e) {
    const char *code=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e,"code"));
    return code ? code : "?";
}
static void send_request(const char *method,cJSON *body,int timeout,orb_link_reply_cb_t cb,void *ctx) {
    CHECK(sent<64 && timeout==REQUEST_MS && !ctx);
    strcpy(requests[sent].method,method); requests[sent].body=body; requests[sent++].cb=cb;
}
"""
tests = r"""
static void respond(unsigned i,esp_err_t status,const char *body) {
    cJSON *p=body ? cJSON_Parse(body) : NULL;
    CHECK(i<sent && requests[i].cb);
    requests[i].cb(status,status==ESP_OK?p:NULL,status==ESP_FAIL?p:NULL,NULL);
    cJSON_Delete(p);
}
static void queue_answer(const char *id) {
    orb_card_t card={.kind=ORB_CARD_APPROVAL,.options_count=2};
    strcpy(card.id,id); strcpy(card.title,"Allow operation?");
    strcpy(card.options[0].id,"allow"); strcpy(card.options[0].label,"Allow once");
    strcpy(card.options[1].id,"deny"); strcpy(card.options[1].label,"Deny");
    card.options[1].role=ORB_OPTION_CANCEL;
    CHECK(orb_model_upsert_card(&card));
    orb_answer_t answer={.option="allow"}; strcpy(answer.card,id);
    CHECK(orb_model_answer(&answer));
}
int main(void) {
    CHECK(orb_model_init());
    queue_answer("one"); pump_model(now);
    CHECK(sent==2 && !strcmp(requests[0].method,"sessions.subscribe") &&
          !strcmp(requests[1].method,"card.answer") && orb_model_pending_answers()==1);
    pump_model(now); CHECK(sent==2);
    respond(1,ESP_ERR_TIMEOUT,NULL); pump_model(now); CHECK(sent==2);
    now+=US(1000); pump_model(now); CHECK(sent==3);
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(requests[2].body,"option")),"allow"));
    respond(2,ESP_FAIL,"{\"code\":\"busy\"}");
    now+=US(1000); pump_model(now); CHECK(sent==4);
    respond(3,ESP_OK,"{\"card\":\"one\",\"answer\":{\"option\":\"deny\"},\"delivered\":false}");
    CHECK(orb_model_pending_answers()==0);
    orb_model_snapshot_t *view=calloc(1,sizeof(*view)); CHECK(view && orb_model_snapshot(view));
    CHECK(view->last_answer.accepted && !view->last_answer.delivered &&
          !strcmp(view->last_answer.answer.option,"deny"));
    now+=US(1000); pump_model(now); CHECK(sent==4);

    queue_answer("two"); pump_model(now); CHECK(sent==5);
    CHECK(orb_model_cancel_card("two") && orb_model_pending_answers()==1);
    respond(4,ESP_ERR_TIMEOUT,NULL);
    link_v11_reset(); s_subscribe_pending=false;
    CHECK(orb_model_sessions_chunk(NULL,0,true,false));
    now+=US(1000); pump_model(now); CHECK(sent==7 && !strcmp(requests[6].method,"card.answer"));
    respond(6,ESP_FAIL,"{\"code\":\"expired\"}"); CHECK(!orb_model_pending_answers());
    CHECK(orb_model_snapshot(view) && !view->last_answer.accepted && !strcmp(view->last_answer.error,"expired"));

    now+=US(1000); queue_answer("three"); pump_model(now); CHECK(sent==8);
    respond(7,ESP_OK,"{\"card\":\"other\",\"answer\":{\"option\":\"deny\"},\"delivered\":true}");
    CHECK(s_end && orb_model_pending_answers()==1);
    s_end=false; now+=US(1000); pump_model(now); CHECK(sent==9);
    respond(8,ESP_OK,"{\"card\":\"three\",\"answer\":{\"option\":\"allow\"},\"delivered\":true}");
    CHECK(!orb_model_pending_answers());

    orb_message_t message={.target="dot",.text="Confirmed words"};
    CHECK(orb_model_message(&message)); pump_model(now); CHECK(sent==10);
    CHECK(!strcmp(requests[9].method,"message.send"));
    respond(9,ESP_ERR_TIMEOUT,NULL);
    now+=US(10000); pump_model(now); CHECK(sent==10);
    CHECK(orb_model_snapshot(view) && !view->last_message.accepted &&
          !strcmp(view->last_message.error,"timeout") && !strcmp(view->last_message.message.text,message.text));
    cards=false; queue_answer("legacy"); pump_model(now); CHECK(sent==10);
    CHECK(orb_model_ack_answer("legacy"));
    cards=true; orb_model_set_connection(true,true);
    orb_prefs_patch_t patch={.sources_mask=1u<<ORB_AGENT_CODEX};
    CHECK(orb_model_prefs_request(&patch));
    unsigned pref=sent; pump_model(now); CHECK(sent==pref+1);
    CHECK(!strcmp(requests[pref].method,"prefs.set"));
    CHECK(!cJSON_HasObjectItem(requests[pref].body,"idle_minutes"));
    CHECK(orb_model_snapshot(view) && view->prefs_pending && view->prefs.sources[ORB_AGENT_CODEX]);
    respond(pref,ESP_OK,"{\"sources\":{\"claude\":true,\"codex\":false,\"dot\":true},\"idle_minutes\":10}");
    CHECK(orb_model_snapshot(view) && !view->prefs_pending && !view->prefs.sources[ORB_AGENT_CODEX]);
    patch=(orb_prefs_patch_t){.has_idle_minutes=true,.idle_minutes=30};
    CHECK(orb_model_prefs_request(&patch)); pref=sent; pump_model(now);
    respond(pref,ESP_ERR_TIMEOUT,NULL);
    CHECK(orb_model_snapshot(view) && !view->prefs_pending && view->prefs.idle_minutes==10 &&
          !strcmp(view->prefs_error,"timeout"));
    patch=(orb_prefs_patch_t){.has_temperature=true,.temperature=ORB_TEMPERATURE_F};
    CHECK(orb_model_prefs_request(&patch)); pref=sent; pump_model(now);
    CHECK(sent==pref+1 && !strcmp(requests[pref].method,"prefs.set"));
    cJSON *units=cJSON_GetObjectItemCaseSensitive(requests[pref].body,"units");
    CHECK(units && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(units,"temperature")),"F"));
    CHECK(cJSON_GetArraySize(requests[pref].body)==1 && cJSON_GetArraySize(units)==1);
    CHECK(orb_model_temperature()==ORB_TEMPERATURE_C);
    respond(pref,ESP_OK,"{\"sources\":{\"claude\":true,\"codex\":false,\"dot\":true},\"idle_minutes\":10,\"units\":{\"temperature\":\"F\",\"source\":\"owner\"}}");
    CHECK(orb_model_snapshot(view) && !view->prefs_pending && view->prefs.temperature_owner);
    CHECK(orb_model_temperature()==ORB_TEMPERATURE_F && !view->prefs.sources[ORB_AGENT_CODEX]);
    patch.temperature=ORB_TEMPERATURE_UNKNOWN;
    CHECK(orb_model_prefs_request(&patch)); pref=sent; pump_model(now);
    units=cJSON_GetObjectItemCaseSensitive(requests[pref].body,"units");
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(units,"temperature")),"auto"));
    respond(pref,ESP_OK,"{\"sources\":{\"claude\":true,\"codex\":false,\"dot\":true},\"idle_minutes\":10,\"units\":{\"temperature\":\"C\",\"source\":\"auto\"}}");
    CHECK(orb_model_snapshot(view) && !view->prefs_pending && !view->prefs.temperature_owner);
    CHECK(orb_model_temperature()==ORB_TEMPERATURE_C);
    CHECK(orb_model_idle_open());
    unsigned idle=sent; pump_model(now); CHECK(sent==idle+1);
    CHECK(!strcmp(requests[idle].method,"sessions.list") &&
          !cJSON_HasObjectItem(requests[idle].body,"cursor"));
    respond(idle,ESP_OK,"{\"sessions\":[],\"cursor\":\"page-two\"}");
    CHECK(orb_model_idle_more()); idle=sent; pump_model(now);
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(requests[idle].body,"cursor")),"page-two"));
    respond(idle,ESP_FAIL,"{\"code\":\"conflict\"}");
    idle=sent; pump_model(now); CHECK(sent==idle+1 && !cJSON_HasObjectItem(requests[idle].body,"cursor"));
    respond(idle,ESP_OK,"{\"sessions\":[],\"cursor\":null}");
    CHECK(!orb_model_idle_more());
    orb_model_idle_close();
    CHECK(orb_model_idle_open()); idle=sent; pump_model(now);
    orb_model_idle_close();
    respond(idle,ESP_OK,"{\"sessions\":[],\"cursor\":\"stale\"}");
    CHECK(!s_end);
    CHECK(orb_model_idle_open()); idle=sent; pump_model(now);
    respond(idle,ESP_OK,"{\"sessions\":[]}"); /* Malformed response fails closed. */
    CHECK(s_end);
    orb_model_idle_close();
    /* Home queues independently of agent capability and never replays failed
     * or ambiguous actuator requests after timeout/reconnect. */
    CHECK(orb_home_init());
    orb_home_room_t room={.id="rroom",.name="Living Room"};
    orb_home_accessory_t lamp={.id="lamp",.name="Floor Lamp",.room="rroom",.kind=ORB_HOME_LIGHT,
        .reachable=true,.writable=ORB_HOME_ON,.state={.present=ORB_HOME_ON,.on=true}};
    CHECK(orb_home_snapshot_chunk(&room,1,&lamp,1,NULL,0,true,false));
    orb_home_set_connection(true,true); s_home=true; cards=false;
    CHECK(orb_home_toggle("lamp")); unsigned home_sent=sent; pump_model(now);
    CHECK(sent==home_sent+1 && !strcmp(requests[home_sent].method,"home.toggle"));
    pump_model(now); CHECK(sent==home_sent+1);
    respond(home_sent,ESP_ERR_TIMEOUT,NULL); pump_model(now); CHECK(sent==home_sent+1);
    orb_home_snapshot_t *home=calloc(1,sizeof(*home)); CHECK(home && orb_home_snapshot(home));
    CHECK(home->accessories[0].state.on && !home->accessories[0].pending && home->error[0]);
    CHECK(orb_home_toggle("lamp")); home_sent=sent; pump_model(now);
    respond(home_sent,ESP_OK,"{\"id\":\"lamp\",\"state\":{\"on\":false}}");
    CHECK(orb_home_snapshot(home) && !home->accessories[0].state.on && !home->error[0]);
    free(home);
    /* Media is independent of cards; controls stay single-flight, consume
     * requests once, validate returned action, and never replay after loss. */
    s_end=false; s_home=false; s_nowplaying=true;
    orb_model_nowplaying_capability(true);
    orb_nowplaying_t media={.title="Track",.artist="Artist",.app="Music",
        .duration=200,.position=10,.state=ORB_MEDIA_PLAYING,.playing=true,.volume=65};
    CHECK(orb_model_set_nowplaying(&media));
    CHECK(orb_model_nowplaying_request(ORB_MEDIA_VOLUME,75)); unsigned media_sent=sent;
    pump_model(now); CHECK(sent==media_sent+1 && !strcmp(requests[media_sent].method,"nowplaying.control"));
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(requests[media_sent].body,"action")),"volume"));
    CHECK(cJSON_GetObjectItemCaseSensitive(requests[media_sent].body,"value")->valueint==75);
    pump_model(now); CHECK(sent==media_sent+1);
    CHECK(!orb_model_nowplaying_request(ORB_MEDIA_NEXT,0));
    respond(media_sent,ESP_ERR_TIMEOUT,NULL); now+=US(10000); pump_model(now);
    CHECK(sent==media_sent+1 && orb_model_snapshot(view) && !view->nowplaying_pending);
    CHECK(view->nowplaying.volume==65 && !strcmp(view->nowplaying_error,"timeout"));
    CHECK(orb_model_nowplaying_request(ORB_MEDIA_PAUSE,0)); media_sent=sent; pump_model(now);
    respond(media_sent,ESP_OK,"{\"accepted\":true,\"action\":\"pause\",\"snapshot\":{\"title\":\"Track\",\"artist\":\"Artist\",\"album\":\"\",\"duration\":200,\"position\":12,\"state\":\"paused\",\"playing\":false,\"app\":\"Music\",\"volume\":65}}");
    CHECK(orb_model_snapshot(view) && !view->nowplaying_pending && !view->nowplaying.playing && !s_end);
    CHECK(orb_model_nowplaying_request(ORB_MEDIA_NEXT,0)); media_sent=sent; pump_model(now);
    respond(media_sent,ESP_OK,"{\"accepted\":true,\"action\":\"pause\",\"snapshot\":{}}");
    CHECK(s_end && orb_model_snapshot(view) && !view->nowplaying_pending);
    s_end=false;
    CHECK(orb_model_nowplaying_request(ORB_MEDIA_PLAY,0));
    orb_model_set_connection(false,false); s_nowplaying=false; s_media_pending=false;
    media_sent=sent; pump_model(now); CHECK(sent==media_sent);
    CHECK(orb_model_snapshot(view) && !view->nowplaying_supported && !view->nowplaying_valid && !view->nowplaying_pending);
    orb_model_set_connection(true,true); orb_model_nowplaying_capability(true); s_nowplaying=true;
    CHECK(orb_model_set_nowplaying(&media)); pump_model(now); CHECK(sent==media_sent);
    /* Units work on an independently negotiated Weather link. Unnegotiated
     * peers retain their queued patch until a capable link can send it. */
    cards=s_home=s_nowplaying=s_weather=false;
    patch=(orb_prefs_patch_t){.has_temperature=true,.temperature=ORB_TEMPERATURE_F};
    CHECK(orb_model_prefs_request(&patch)); pref=sent;
    pump_model(now); CHECK(sent==pref);
    s_weather=true; orb_model_weather_capability(true);
    pump_model(now); CHECK(sent==pref+1 && !strcmp(requests[pref].method,"prefs.set"));
    respond(pref,ESP_OK,"{\"sources\":{\"claude\":true,\"codex\":false,\"dot\":true},\"idle_minutes\":10,\"units\":{\"temperature\":\"F\",\"source\":\"owner\"}}");
    CHECK(orb_model_temperature()==ORB_TEMPERATURE_F && orb_model_snapshot(view) && !view->prefs_pending);
    free(view);
    for(unsigned i=0;i<sent;i++) cJSON_Delete(requests[i].body);
    printf("link delivery: %u checks passed\n",checks);
}
"""
work.mkdir(parents=True, exist_ok=True)
source = work / "delivery.c"
source.write_text(prelude + delivery + tests)
binary = work / "delivery"
subprocess.run(
    [
        "clang",
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-unused-function",
        "-Wno-deprecated-declarations",
        "-O1",
        "-g",
        "-fsanitize=address,undefined",
        "-pthread",
        f"-I{component}",
        f"-I{model / 'include'}",
        f"-I{model.parent / 'orb_home_data/include'}",
        f"-I{cjson}",
        str(source),
        str(component / "link_v11.c"),
        str(component / "link_home.c"),
        str(model.parent / "orb_home_data/orb_home.c"),
        f"-I{model.parent / 'orb_memory/include'}",
        str(model.parent / "orb_memory/orb_memory.c"),
        str(model / "orb_model.c"),
        str(model / "orb_model_voice_notify.c"),
        str(cjson / "cJSON.c"),
        "-o",
        str(binary),
    ],
    check=True,
)
subprocess.run([str(binary)], check=True)
