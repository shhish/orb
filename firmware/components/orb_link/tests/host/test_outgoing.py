"""Exercise actual outgoing API admission and ownership across reconnects."""

import os
import subprocess
import sys
from pathlib import Path

component = Path(__file__).resolve().parents[2]
work = Path(sys.argv[1] if len(sys.argv) > 1 else "build").resolve()
cjson = Path(
    os.environ.get("CJSON_DIR", component.parents[1] / "managed_components/espressif__cjson/cJSON")
).resolve()
text = (component / "orb_link.c").read_text()
outgoing = text[
    text.index("static esp_err_t post_outgoing(") : text.index("/* Reliable output acknowledges")
]
notification = text[
    text.index("/* Reliable output acknowledges") : text.index(
        "bool orb_link_supports_devtools(void)\n{"
    )
]
transport = text[
    text.index("static bool send_text_until(") : text.index("/* Fails a request here,")
]
prelude = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
static unsigned radio_tx_messages;
static size_t radio_tx_bytes;
static void link_radio_tx(size_t bytes) {radio_tx_messages++;radio_tx_bytes+=bytes;}
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_TIMEOUT 4
#define NAME_LEN 32
#define REQUEST_MS 10000
#define US(ms) ((int64_t)(ms)*1000)
#define pdTRUE 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define ORB_LINK_ONLINE 5
typedef enum {MSG_REQUEST,MSG_EVENT,MSG_AUDIO} msg_kind_t;
typedef void (*orb_link_reply_cb_t)(esp_err_t,const cJSON*,const cJSON*,void*);
typedef void (*orb_link_audio_cb_t)(const uint8_t*,size_t,uint32_t,void*);
static orb_link_audio_cb_t s_audio_cb;
static void *s_audio_ctx;
typedef struct {
    msg_kind_t kind;
    uint32_t conn;
    char *text;
    size_t len;
    char name[NAME_LEN];
    cJSON *body;
    int timeout_ms;
    orb_link_reply_cb_t cb;
    void *ctx;
    int64_t deadline;
} msg_t;
static int s_lock,s_queue;
static uint32_t s_conn=7;
static struct {int state;} s_status={ORB_LINK_ONLINE};
static bool s_cards=true,s_voice=true,s_voice_available,s_home,s_weather;
static bool fail_alloc,reconnect_on_unlock,s_devtools=true;
static bool orb_link_supports_devtools(void) {return s_devtools;}
static unsigned checks,queued,deleted,audio_allocs,audio_frees;
static unsigned text_frees,transport_calls;
static bool s_up=true,s_peer_closed,s_end;
static int s_ws=123,transport_wait,transport_result=-2;
static cJSON *last_deleted;
static msg_t queue[32];
static int64_t now=123456;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
static void unlock(void) {
    if(reconnect_on_unlock) { reconnect_on_unlock=false; ++s_conn; }
}
#define taskENTER_CRITICAL(p) ((void)(p))
#define taskEXIT_CRITICAL(p) do { (void)(p); unlock(); } while(0)
static int64_t esp_timer_get_time(void) { return now; }
static int xQueueSend(int q,const msg_t *m,int wait) {
    CHECK(q==s_queue && wait==0);
    if(queued==32) return 0;
    queue[queued++]=*m;
    return pdTRUE;
}
static void *heap_caps_malloc(size_t n,int caps) {
    CHECK(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(fail_alloc) return NULL;
    void *p=malloc(n); CHECK(p!=NULL); ++audio_allocs; return p;
}
static void audio_free(void *p) { CHECK(p!=NULL); ++audio_frees; free(p); }
#define ORB_MEMORY_NETWORK 1
static void *orb_memory_alloc(int owner, size_t n) {
    CHECK(owner == ORB_MEMORY_NETWORK);
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void orb_memory_free(void *p) { audio_free(p); }
static void delete_body(cJSON *body) {
    ++deleted; last_deleted=body; cJSON_Delete(body);
}
#define free audio_free
#define cJSON_Delete delete_body
esp_err_t orb_link_send_audio_on(uint32_t generation,const void *frame,size_t len);
#define SEND_MS 5000
#define EXT_MAX_MSG 4096
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
static int esp_websocket_client_send_text(int ws,const char *bytes,size_t n,int wait) {
    CHECK(ws==s_ws && bytes!=NULL && n==strlen(bytes));
    ++transport_calls; transport_wait=wait;
    return transport_result==-2 ? (int)n : transport_result;
}
static void text_free(void *p) { CHECK(p!=NULL); ++text_frees; cJSON_free(p); }
#define cJSON_free text_free
"""
tests = r"""
#undef free
#undef cJSON_Delete
#undef cJSON_free
static void clear_queue(void) {
    for(unsigned i=0;i<queued;i++) {
        if(queue[i].text) audio_free(queue[i].text);
        cJSON_Delete(queue[i].body);
    }
    queued=0;
}
static void response(esp_err_t err,const cJSON *r,const cJSON *e,void *ctx) {
    (void)err; (void)r; (void)e; (void)ctx;
}
static cJSON *params(void) {
    cJSON *p=cJSON_CreateObject(); CHECK(p!=NULL);
    CHECK(cJSON_AddNumberToObject(p,"stream",1)); return p;
}
static void test_requests(void) {
    cJSON *p=params();
    unsigned n=deleted;
    CHECK(orb_link_request_on(6,"voice.end",p,10000,response,(void*)1)==ESP_ERR_INVALID_STATE);
    CHECK(deleted==n+1 && last_deleted==p && !queued);
    p=params(); n=deleted;
    CHECK(orb_link_request_on(0,"voice.end",p,10000,NULL,NULL)==ESP_ERR_INVALID_ARG);
    CHECK(deleted==n+1 && last_deleted==p && !queued);
    p=params(); n=deleted;
    CHECK(orb_link_request_on(7,NULL,p,0,NULL,NULL)==ESP_ERR_INVALID_ARG);
    CHECK(deleted==n+1 && last_deleted==p);
    p=params(); n=deleted;
    CHECK(orb_link_request("01234567890123456789012345678901",p,0,NULL,NULL)==ESP_ERR_INVALID_ARG);
    CHECK(deleted==n+1 && last_deleted==p);
    s_status.state=0; p=params(); n=deleted;
    CHECK(orb_link_request("voice.end",p,0,NULL,NULL)==ESP_ERR_INVALID_STATE);
    CHECK(deleted==n+1 && last_deleted==p);
    s_status.state=ORB_LINK_ONLINE;
    p=params(); n=deleted;
    CHECK(orb_link_request_on(7,"voice.end",p,0,response,(void*)1)==ESP_OK);
    CHECK(queued==1 && deleted==n && queue[0].body==p && queue[0].conn==7);
    CHECK(queue[0].kind==MSG_REQUEST && queue[0].cb==response && queue[0].ctx==(void*)1);
    CHECK(queue[0].deadline==now+US(REQUEST_MS));
    now+=US(50); CHECK(queue[0].deadline==now+US(REQUEST_MS-50));
    CHECK(orb_link_request("device.ping",NULL,23,NULL,NULL)==ESP_OK);
    CHECK(queue[1].deadline==now+US(23) && queue[1].conn==7);
    CHECK(orb_link_request("device.ping",NULL,-1,NULL,NULL)==ESP_OK);
    CHECK(queue[2].deadline==now+US(REQUEST_MS));
    CHECK(orb_link_notify("state.report",NULL)==ESP_OK);
    CHECK(queue[3].kind==MSG_EVENT && queue[3].conn==7);
    clear_queue();
    /* Reconnection after the coherent admission snapshot cannot retag an old
     * request to the new connection, even before queue insertion happens. */
    reconnect_on_unlock=true;
    CHECK(orb_link_request_on(7,"voice.end",params(),0,NULL,NULL)==ESP_OK);
    CHECK(s_conn==8 && queue[0].conn==7);
    clear_queue();
    for(unsigned i=0;i<32;i++) CHECK(orb_link_request("device.ping",NULL,0,NULL,NULL)==ESP_OK);
    p=params(); n=deleted;
    CHECK(orb_link_request("device.ping",p,0,NULL,NULL)==ESP_ERR_NO_MEM);
    CHECK(deleted==n+1 && last_deleted==p && queued==32);
    clear_queue();
}
static void test_developer_deadlines(void) {
    unsigned n=deleted;
    CHECK(orb_link_devtools_notify_until(s_conn,"devtools.output",params(),now,response,NULL)==ESP_ERR_TIMEOUT);
    CHECK(deleted==n+1 && !queued);
    CHECK(orb_link_devtools_notify_until(s_conn,"devtools.output",params(),now-1,response,NULL)==ESP_ERR_TIMEOUT);
    CHECK(deleted==n+2 && !queued);
    CHECK(orb_link_devtools_notify_until(s_conn,"devtools.output",params(),now+US(234),response,NULL)==ESP_OK);
    CHECK(queued==1 && queue[0].deadline==now+US(234));
    CHECK(queue[0].conn==s_conn && queue[0].cb==response && queue[0].kind==MSG_EVENT);
    CHECK(orb_link_devtools_notify_until(s_conn,"devtools.output",params(),now+US(10000),response,NULL)==ESP_OK);
    CHECK(queued==2 && queue[1].deadline==now+US(5000));
    CHECK(orb_link_devtools_notify_on(s_conn,"devtools.done",params(),response,NULL)==ESP_OK);
    CHECK(queued==3 && queue[2].deadline==now+US(5000));
    n=deleted;
    CHECK(orb_link_devtools_notify_until(s_conn-1,"devtools.output",params(),now+US(234),response,NULL)==ESP_ERR_INVALID_STATE);
    CHECK(deleted==n+1 && queued==3);
    s_devtools=false;
    CHECK(orb_link_devtools_notify_until(s_conn,"devtools.output",params(),now+US(234),response,NULL)==ESP_ERR_INVALID_STATE);
    CHECK(deleted==n+2 && queued==3);
    s_devtools=true;
    clear_queue();
}
static void test_audio(void) {
    uint8_t frame[1033]={1,0,1};
    CHECK(orb_link_send_audio_on(s_conn,NULL,9)==ESP_ERR_INVALID_ARG);
    CHECK(orb_link_send_audio_on(s_conn,frame,7)==ESP_ERR_INVALID_ARG);
    CHECK(orb_link_send_audio_on(s_conn,frame,8)==ESP_ERR_INVALID_ARG);
    CHECK(orb_link_send_audio_on(s_conn,frame,10)==ESP_ERR_INVALID_ARG);
    CHECK(orb_link_send_audio_on(s_conn,frame,1033)==ESP_ERR_INVALID_ARG);
    frame[0]=2; CHECK(orb_link_send_audio_on(s_conn,frame,9)==ESP_ERR_INVALID_ARG);
    frame[0]=1; frame[2]=0;
    CHECK(orb_link_send_audio_on(s_conn,frame,9)==ESP_ERR_INVALID_ARG);
    frame[2]=1;
    CHECK(orb_link_send_audio_on(0,frame,9)==ESP_ERR_INVALID_STATE);
    CHECK(orb_link_send_audio_on(s_conn-1,frame,9)==ESP_ERR_INVALID_STATE);
    s_voice=false; CHECK(orb_link_send_audio_on(s_conn,frame,9)==ESP_ERR_INVALID_STATE);
    s_voice=true; s_status.state=0;
    CHECK(orb_link_send_audio_on(s_conn,frame,9)==ESP_ERR_INVALID_STATE);
    s_status.state=ORB_LINK_ONLINE;
    CHECK(!queued && !audio_allocs);
    fail_alloc=true;
    CHECK(orb_link_send_audio_on(s_conn,frame,1031)==ESP_ERR_NO_MEM);
    CHECK(!queued && !audio_allocs);
    fail_alloc=false;
    CHECK(orb_link_send_audio_on(s_conn,frame,1031)==ESP_OK);
    CHECK(queued==1 && queue[0].kind==MSG_AUDIO && queue[0].conn==s_conn && queue[0].len==1031);
    CHECK(!memcmp(queue[0].text,frame,1031));
    frame[7]=42; CHECK((uint8_t)queue[0].text[7]!=frame[7]);
    CHECK(orb_link_request_on(s_conn,"voice.end",params(),0,NULL,NULL)==ESP_OK);
    CHECK(queued==2 && queue[0].kind==MSG_AUDIO && queue[1].kind==MSG_REQUEST);
    CHECK(!strcmp(queue[1].name,"voice.end"));
    clear_queue(); CHECK(audio_frees==audio_allocs);
    uint32_t previous=s_conn; reconnect_on_unlock=true;
    CHECK(orb_link_send_audio_on(previous,frame,9)==ESP_OK);
    CHECK(s_conn==previous+1 && queue[0].conn==previous);
    clear_queue();
    /* The convenience wrapper reads the old generation before admission and
     * rejects when a reconnect happens between those two snapshots. */
    reconnect_on_unlock=true;
    CHECK(orb_link_send_audio(frame,9)==ESP_ERR_INVALID_STATE);
    CHECK(!queued && audio_frees==audio_allocs);
    for(unsigned i=0;i<32;i++) CHECK(orb_link_send_audio(frame,9)==ESP_OK);
    unsigned allocations=audio_allocs,frees=audio_frees;
    CHECK(orb_link_send_audio(frame,9)==ESP_ERR_NO_MEM);
    CHECK(audio_allocs==allocations+1 && audio_frees==frees+1 && queued==32);
    clear_queue(); CHECK(audio_allocs==audio_frees);
}
static char *wire_text(size_t n) {
    char *p=cJSON_malloc(n+1); CHECK(p!=NULL);
    memset(p,'x',n); p[n]='\0'; return p;
}
static void test_transport_deadlines(void) {
    unsigned calls=transport_calls,frees=text_frees;
    unsigned messages=radio_tx_messages;size_t bytes=radio_tx_bytes;
    send_text_until(NULL,now); CHECK(transport_calls==calls && text_frees==frees);
    send_text_until(wire_text(20),now);
    CHECK(transport_calls==calls && text_frees==++frees && !s_end);
    send_text_until(wire_text(20),now-1);
    CHECK(transport_calls==calls && text_frees==++frees && !s_end);
    /* A sub-millisecond remainder must never be rounded into a longer send. */
    send_text_until(wire_text(20),now+999);
    CHECK(transport_calls==calls && text_frees==++frees);
    send_text_until(wire_text(20),now+US(1));
    CHECK(transport_calls==++calls && text_frees==++frees && transport_wait==1 && !s_end);
    send_text_until(wire_text(20),now+US(234));
    CHECK(transport_calls==++calls && text_frees==++frees && transport_wait==234);
    send_text_until(wire_text(20),now+US(10000));
    CHECK(transport_calls==++calls && text_frees==++frees && transport_wait==SEND_MS);
    send_text(wire_text(20));
    CHECK(transport_calls==++calls && text_frees==++frees && transport_wait==SEND_MS);
    transport_result=19;
    send_text_until(wire_text(20),now+US(5));
    CHECK(transport_calls==++calls && text_frees==++frees && s_end);
    s_end=false; transport_result=-1;
    send_text_until(wire_text(20),now+US(5));
    CHECK(transport_calls==++calls && text_frees==++frees && s_end);
    s_end=false; transport_result=-2;
    send_text_until(wire_text(EXT_MAX_MSG+1),now+US(5000));
    CHECK(transport_calls==calls && text_frees==++frees && s_end);
    s_end=false; s_cards=false;
    send_text_until(wire_text(EXT_MAX_MSG+1),now+US(5000));
    CHECK(transport_calls==++calls && text_frees==++frees && !s_end);
    s_cards=true; s_up=false;
    send_text_until(wire_text(20),now+US(5));
    CHECK(transport_calls==calls && text_frees==++frees && !s_end);
    s_up=true; s_peer_closed=true;
    send_text_until(wire_text(20),now+US(5));
    CHECK(transport_calls==calls && text_frees==++frees && !s_end);
    s_peer_closed=false;
    CHECK(radio_tx_messages==messages+5&&radio_tx_bytes==bytes+4*20+EXT_MAX_MSG+1);
}
int main(void) {
    CHECK(orb_link_online() && orb_link_supports_cards() && orb_link_supports_voice());
    CHECK(orb_link_generation()==7);
    orb_link_set_voice_available(false); CHECK(!s_voice_available);
    orb_link_set_voice_available(true); CHECK(s_voice_available);
    test_requests(); test_audio(); test_transport_deadlines(); test_developer_deadlines();
    printf("link outgoing: %u checks passed\n",checks);
}
"""
work.mkdir(parents=True, exist_ok=True)
source = work / "outgoing.c"
source.write_text(prelude + outgoing + notification + transport + tests)
binary = work / "outgoing"
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
        f"-I{cjson}",
        str(source),
        str(cjson / "cJSON.c"),
        "-o",
        str(binary),
    ],
    check=True,
)
subprocess.run([str(binary)], check=True)
