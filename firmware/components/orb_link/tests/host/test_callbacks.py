"""Isolated actual-callback fault injection; no ESP runtime or sockets."""

import subprocess
import sys
from pathlib import Path

component = Path(__file__).resolve().parents[2]
work = Path(sys.argv[1] if len(sys.argv) > 1 else "build").resolve()
work.mkdir(parents=True, exist_ok=True)
text = (component / "orb_link.c").read_text()
callbacks = text[
    text.index("static void observe_ws(") : text.index("/* ---- The link task ---- */")
]
prelude = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "link_control.h"
static void link_radio_rx(size_t bytes) {(void)bytes;}
#define LINK_MAX_MSG 16384
#define EXT_MAX_MSG 4096
#define pdTRUE 1
#define taskENTER_CRITICAL(p) ((void)(p))
#define taskEXIT_CRITICAL(p) ((void)(p))
#define ESP_LOGW(...) ((void)0)
#define WEBSOCKET_EVENT_CONNECTED 1
#define WEBSOCKET_EVENT_DISCONNECTED 2
#define WEBSOCKET_EVENT_CLOSED 3
#define WEBSOCKET_EVENT_DATA 4
#define WS_TRANSPORT_OPCODES_TEXT 1
#define WS_TRANSPORT_OPCODES_BINARY 2
#define WS_TRANSPORT_OPCODES_CONT 0
#define MSG_WS_STATE 1
#define MSG_WS_TEXT 2
#define MSG_WS_BINARY 3
typedef const char *esp_event_base_t;
typedef struct { int kind; uint32_t conn, sequence; char *text; size_t len; } msg_t;
typedef struct { int op_code, payload_offset, data_len, payload_len; bool fin; char *data_ptr; } esp_websocket_event_data_t;
static const char *TAG = "test";
static int s_lock, s_queue;
static link_control_t s_control;
static uint32_t s_conn, s_handled_frame;
static bool s_up, s_end, s_peer_closed, s_rx_too_large, s_cards, s_home, s_voice, s_weather;
static bool orb_link_supports_cards(void) { return s_cards; }
static bool orb_link_supports_home(void) { return s_home; }
static bool orb_link_supports_weather(void) { return s_weather; }
static bool orb_link_supports_voice(void) { return s_voice; }
static char *s_rx;
static size_t s_rx_len;
static bool s_rx_open, s_rx_skip;
static bool s_rx_binary_open;
static size_t s_rx_binary_len;
static msg_t queue[32];
static unsigned count, checks, delivered;
static bool fail_alloc;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
static int xQueueSend(int q, const msg_t *m, int wait)
{
    (void)q; CHECK(wait == 0);
    if(count == 32) return 0;
    queue[count++] = *m; return pdTRUE;
}
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static struct { void *pointer; size_t bytes; } allocations[64];
static size_t owner_bytes;
static void *heap_caps_realloc(void *p, size_t n, int caps) {
    CHECK(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    unsigned slot=0;
    while(slot<64 && (p ? allocations[slot].pointer!=p : allocations[slot].pointer!=NULL)) slot++;
    CHECK(slot<64);
    size_t old=allocations[slot].bytes;
    if(fail_alloc || n>384u*1024u-owner_bytes+old) return NULL;
    void *grown=realloc(p,n); CHECK(grown!=NULL);
    allocations[slot].pointer=grown; allocations[slot].bytes=n;
    owner_bytes=owner_bytes-old+n;
    return grown;
}
#define ORB_MEMORY_NETWORK 1
static char *orb_link_memory_rx_resize(char *p, size_t n) {
    return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void orb_link_memory_rx_free(void *p) {
    if(!p) return;
    unsigned slot=0;
    while(slot<64 && allocations[slot].pointer!=p) slot++;
    CHECK(slot<64 && owner_bytes>=allocations[slot].bytes);
    owner_bytes-=allocations[slot].bytes;
    allocations[slot].pointer=NULL; allocations[slot].bytes=0;
    free(p);
}
/* Binary reassembly shares the network owner's accounting with text frames. */
static void *orb_memory_realloc(int owner, void *p, size_t n) {
    CHECK(owner == ORB_MEMORY_NETWORK);
    return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void orb_memory_free(void *p) { orb_link_memory_rx_free(p); }
"""
tests = r"""
#undef realloc
static void cleanup(void)
{
    orb_link_memory_rx_free(s_rx); s_rx=NULL;
    for(unsigned i=0;i<count;i++) orb_link_memory_rx_free(queue[i].text);
    count=0;
    CHECK(owner_bytes==0);
}
static void start(uint32_t epoch)
{
    cleanup(); s_conn=epoch; s_control=(link_control_t){.conn=epoch};
    s_handled_frame=delivered=0; s_up=s_end=s_peer_closed=s_rx_too_large=s_cards=s_home=s_voice=s_weather=false;
    s_rx_len=0; s_rx_open=s_rx_skip=fail_alloc=false;
    s_rx_binary_len=0; s_rx_binary_open=false;
    on_ws_event((void *)(uintptr_t)epoch,NULL,WEBSOCKET_EVENT_CONNECTED,NULL);
    sync_ws_state(); CHECK(s_up && !s_end);
    count=0; /* Consume the advisory UP wake. */
}
static void data(const char *text, int op, bool fin)
{
    esp_websocket_event_data_t d={.op_code=op,.payload_len=(int)strlen(text),.data_len=(int)strlen(text),.fin=fin,.data_ptr=(char *)text};
    on_ws_event((void *)(uintptr_t)s_conn,NULL,WEBSOCKET_EVENT_DATA,&d);
}
static void drain(void)
{
    while(count) {
        msg_t m=queue[0]; memmove(queue,queue+1,(--count)*sizeof(*queue));
        sync_ws_state();
        if((m.kind==MSG_WS_TEXT || m.kind==MSG_WS_BINARY) && m.conn==s_conn) {
            if(s_up && !s_end) delivered++;
            s_handled_frame=m.sequence;
        }
        orb_link_memory_rx_free(m.text); sync_ws_state();
    }
}
int main(void)
{
    (void)TAG;
    for(uint32_t epoch=1;epoch<=100;epoch++) {
        start(epoch);
        for(unsigned i=0;i<32;i++) data("accepted final event",1,true);
        CHECK(count==32);
        on_ws_event((void *)(uintptr_t)epoch,NULL,WEBSOCKET_EVENT_DISCONNECTED,NULL);
        sync_ws_state(); CHECK(s_peer_closed && !s_end);
        drain(); CHECK(delivered==32 && s_end && !s_up);
        start(epoch+1000);
        on_ws_event((void *)(uintptr_t)epoch,NULL,WEBSOCKET_EVENT_CLOSED,NULL);
        sync_ws_state(); CHECK(!s_end && s_up);
        for(unsigned i=0;i<33;i++) data("state delta",1,true);
        sync_ws_state(); CHECK(s_end && !s_up && s_control.lost_frame);
        drain(); CHECK(delivered==0);
    }
    start(2000);
    data("abc",1,false);
    esp_websocket_event_data_t stale={.op_code=1,.payload_len=3,.data_len=3,.fin=false,.data_ptr="OLD"};
    on_ws_event((void *)(uintptr_t)1999,NULL,WEBSOCKET_EVENT_DATA,&stale);
    CHECK(s_rx_len==3 && !memcmp(s_rx,"abc",3));
    stale.op_code=0; stale.fin=true;
    on_ws_event((void *)(uintptr_t)1999,NULL,WEBSOCKET_EVENT_DATA,&stale);
    CHECK(s_rx_len==3 && !memcmp(s_rx,"abc",3) && count==0);
    data("def",0,true);
    CHECK(count==1 && queue[0].len==6 && !strcmp(queue[0].text,"abcdef"));
    drain(); CHECK(delivered==1);
    data("prefix",1,false); fail_alloc=true; data("suffix",0,true);
    sync_ws_state(); CHECK(s_end && s_control.lost_frame && !s_rx);
    start(2001);
    char oversized[LINK_MAX_MSG+2]; memset(oversized,'x',sizeof(oversized)-1); oversized[sizeof(oversized)-1]=0;
    data(oversized,1,true); sync_ws_state(); CHECK(!s_end && !s_control.lost_frame);
    data("valid after oversized",1,true); drain(); CHECK(delivered==1);
    start(3000); s_cards=true;
    char ext[EXT_MAX_MSG+2]; memset(ext,'x',sizeof(ext)-1); ext[sizeof(ext)-1]=0;
    data(ext,1,true); sync_ws_state(); CHECK(s_end && s_rx_too_large);
    start(3001); s_cards=true;
    data(ext,2,true); sync_ws_state(); CHECK(s_end && s_rx_too_large);
    start(3002); s_cards=true;
    data("prefix",1,false); data(ext,0,true);
    sync_ws_state(); CHECK(s_end && s_rx_too_large);
    start(3003); s_cards=true;
    char exact[EXT_MAX_MSG+1]; memset(exact,'x',sizeof(exact)-1); exact[sizeof(exact)-1]=0;
    data(exact,1,true); drain(); CHECK(delivered==1 && !s_end);
    start(3500); s_weather=true;
    data(oversized,1,true); sync_ws_state(); CHECK(s_end && s_rx_too_large);
    start(3501); s_weather=true;
    data("valid weather envelope",1,true); drain(); CHECK(delivered==1 && !s_end);
    start(4000); s_cards=s_home=true;
    data(ext,1,true); drain(); CHECK(delivered==1 && !s_end && !s_rx_too_large);
    start(4001); s_home=true;
    data(ext,2,true); sync_ws_state(); CHECK(s_end && s_rx_too_large);
    start(4002); s_home=true;
    data(oversized,1,true); sync_ws_state(); CHECK(s_end && s_rx_too_large);
    start(4003); s_home=true;
    data("prefix",1,false); data(ext,0,true); drain();
    CHECK(delivered==1 && !s_end && !s_rx_too_large);
    char half[EXT_MAX_MSG/2+1]; memset(half,'x',sizeof(half)-1); half[sizeof(half)-1]=0;
    char half_over[EXT_MAX_MSG/2+2]; memset(half_over,'x',sizeof(half_over)-1); half_over[sizeof(half_over)-1]=0;
    start(4004); s_home=true;
    data(half,2,false); data("ping",9,true); data(half,0,true); sync_ws_state();
    CHECK(!s_end && !s_rx_too_large && !s_rx_binary_open && !s_rx_binary_len && !count);
    data("valid after binary",1,true); drain(); CHECK(delivered==1);
    start(4005); s_home=true;
    data(half,2,false); data(half_over,0,true); sync_ws_state();
    CHECK(s_end && s_rx_too_large && !s_rx);
    start(4006); s_home=true;
    data("binary prefix",2,false); data(ext,0,true); sync_ws_state();
    CHECK(s_end && s_rx_too_large && !s_rx);
    start(4007); s_home=true;
    data("text prefix",1,false); data("interleaved binary",2,true); sync_ws_state();
    CHECK(s_end && !s_rx_too_large);
    start(4008); s_home=true;
    data("binary prefix",2,false); data("interleaved text",1,true); sync_ws_state();
    CHECK(s_end && !s_rx_too_large);
    start(4009); s_cards=true;
    data(half,2,false); data(half_over,0,true); sync_ws_state();
    CHECK(s_end && s_rx_too_large);
    start(4010); s_home=true;
    esp_websocket_event_data_t chunk={.op_code=2,.payload_len=EXT_MAX_MSG,
        .data_len=EXT_MAX_MSG/2,.fin=true,.data_ptr=half};
    on_ws_data(s_conn,&chunk); CHECK(s_rx_binary_open && s_rx_binary_len==EXT_MAX_MSG/2);
    chunk.payload_offset=EXT_MAX_MSG/2; on_ws_data(s_conn,&chunk); sync_ws_state();
    CHECK(!s_end && !s_rx_binary_open && !s_rx_binary_len && !count);
    start(4012); s_voice=true;
    data("voice.begin",1,true);
    data("audio header",2,false); data("PCM continuation",0,true);
    data("voice.end",1,true);
    CHECK(count==3 && queue[1].kind==MSG_WS_BINARY && queue[1].len==28);
    CHECK(!memcmp(queue[1].text,"audio headerPCM continuation",28));
    drain(); CHECK(delivered==3 && !s_end);
    start(4013); s_voice=true;
    char audio_large[1033]; memset(audio_large,'a',1032); audio_large[1032]=0;
    data(audio_large,2,true); sync_ws_state(); CHECK(s_end && s_rx_too_large);
    start(4011); s_home=true;
    data("orphaned continuation",0,true); sync_ws_state(); CHECK(s_end && !s_rx_too_large);
    /* Legacy classification can queue larger text than negotiated cards.
     * Owner pressure closes for a fresh snapshot before its 32-slot queue can
     * exceed the 384 KiB byte ceiling; queued owners retire exactly once. */
    start(5000); oversized[LINK_MAX_MSG]='\0';
    for(unsigned i=0;i<24;i++) data(oversized,1,true);
    sync_ws_state();
    CHECK(s_end && s_control.lost_frame && !s_rx);
    CHECK(owner_bytes==23u*(LINK_MAX_MSG+1u) && owner_bytes<=384u*1024u);
    drain(); CHECK(owner_bytes==0 && delivered==0);
    cleanup(); printf("link callbacks: %u checks passed\n", checks);
}
"""
source = work / "callbacks.c"
source.write_text(prelude + callbacks + tests)
binary = work / "callbacks"
subprocess.run(
    [
        "clang",
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-unused-parameter",
        "-O1",
        "-g",
        "-fsanitize=address,undefined",
        f"-I{component}",
        str(source),
        "-o",
        str(binary),
    ],
    check=True,
)
subprocess.run([str(binary)], check=True)
