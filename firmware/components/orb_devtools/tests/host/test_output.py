"""Exercise the production formatted Wi-Fi sink and channel advertisement."""

import subprocess
import sys
from pathlib import Path

component = Path(__file__).resolve().parents[2]
work = Path(sys.argv[1] if len(sys.argv) > 1 else "build").resolve()
source = (component / "orb_devtools.c").read_text()
sink = source[source.index("static bool wifi_text_until(") : source.index("#define printf")]
hello = source[source.index("static void wifi_hello(") : source.index("static size_t reply_write(")]
reply = source[
    source.index("static bool command_reply_vprintf(") : source.index("static portMUX_TYPE s_lock")
]
prelude = r"""
#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_devtools_wire.h"
#include "orb_console_reply.h"
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define ESP_OK 0
static int s_output_lock, s_wifi_lock, lock_depth;
#define portENTER_CRITICAL(lock) do {(void)(lock);lock_depth++;}while(0)
#define portEXIT_CRITICAL(lock) do {(void)(lock);lock_depth--;}while(0)
static struct {bool active,failed,acknowledged;uint32_t stream,seq;size_t bytes;int64_t deadline;} s_wifi;
static unsigned checks,events,fail_event;
static char received[40000];
static size_t received_size;
static int64_t now, received_deadline;
static int lock_wait;
static unsigned usb_writes;
#define CHECK(c) do {checks++;if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
static int xSemaphoreTake(int sem,int timeout) {(void)sem;lock_wait=timeout;return pdTRUE;}
static int64_t esp_timer_get_time(void) {return now;}
static void xSemaphoreGive(int sem) {(void)sem;}
static bool wifi_current(void) {return true;}
static void *heap_caps_malloc(size_t size,int caps) {(void)caps;return malloc(size);}
static void heap_caps_free(void *p) {free(p);}
static size_t reply_write(void *ctx,const uint8_t *text,size_t size) {
    (void)ctx;CHECK(received_size+size<sizeof(received));
    memcpy(received+received_size,text,size);received_size+=size;usb_writes++;return size;
}
static bool reply_writable(void *ctx) {(void)ctx;return true;}
static void reply_flush(void *ctx) {(void)ctx;}
static int64_t reply_now(void *ctx) {(void)ctx;return now;}
static void reply_yield(void *ctx) {(void)ctx;now+=1000;}
static bool wifi_event_until(const char *name,cJSON *params,int64_t deadline) {
    received_deadline=deadline;
    CHECK(!strcmp(name,"devtools.output") && !lock_depth);
    CHECK(cJSON_GetObjectItemCaseSensitive(params,"stream")->valueint==7);
    CHECK(cJSON_GetObjectItemCaseSensitive(params,"seq")->valueint==(int)events);
    const char *text=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params,"text"));
    size_t size=strlen(text);
    CHECK(size>0 && size<=1500);
    CHECK(((unsigned char)text[0]&0xc0)!=0x80);
    CHECK(received_size+size<sizeof(received));
    memcpy(received+received_size,text,size);received_size+=size;events++;
    cJSON_Delete(params);
    return !fail_event || events!=fail_event;
}
typedef struct {char channel[16];} orb_link_status_t;
static const char *channel;
static int orb_link_status(orb_link_status_t *status) {snprintf(status->channel,sizeof(status->channel),"%s",channel);return ESP_OK;}
"""
tests = r"""
static void reset(void) {
    s_wifi.active=true;s_wifi.failed=false;s_wifi.acknowledged=false;
    s_wifi.stream=7;s_wifi.seq=0;s_wifi.bytes=0;
    received_size=0;events=0;fail_event=0;now=1000000;
    s_wifi.deadline=now+90000000;received_deadline=0;lock_wait=0;usb_writes=0;
}
int main(void) {
    char line[12001];memset(line,'x',sizeof(line)-1);line[sizeof(line)-1]=0;
    const int sizes[]={4095,4096,9000,12000};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++) {
        reset();CHECK(devtools_printf("%.*s",sizes[i],line)==sizes[i]);
        CHECK(!s_wifi.failed && received_size==(size_t)sizes[i]);
        CHECK(!memcmp(received,line,received_size));
        CHECK(events==(unsigned)(sizes[i]+1499)/1500 && s_wifi.bytes==received_size);
    }
    reset();CHECK(wifi_file_write(NULL,line,12000)==12000);
    CHECK(received_size==12000 && events==8 && !s_wifi.failed);
    reset();fail_event=2;
    CHECK(devtools_printf("%s",line)==-1 && s_wifi.failed && events==2);
    CHECK(devtools_printf("later")==-1 && events==2);
    reset();CHECK(devtools_printf("%*s",ORB_DEVTOOLS_MAX_OUTPUT+1,"")==-1);
    CHECK(s_wifi.failed && !events);
    reset();
    char utf8[9001];for(unsigned i=0;i<9000;i+=3)memcpy(utf8+i,"\xe2\x9c\x93",3);utf8[9000]=0;
    CHECK(devtools_printf(".%s",utf8)==9001);
    CHECK(received_size==9001 && received[0]=='.' && !memcmp(received+1,utf8,9000));
    const char *channels[]={"","stable","dev"};
    for(unsigned i=0;i<3;i++) {
        channel=channels[i];cJSON *p=cJSON_CreateObject();
        cJSON *caps=cJSON_AddArrayToObject(p,"caps");
        wifi_hello(p,NULL);
        CHECK(cJSON_GetArraySize(caps)==(i==2));
        if(i==2)CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetArrayItem(caps,0)),"devtools"));
        cJSON_Delete(p);
    }
    reset();CHECK(wifi_text_until("receipt",7,now+1234000));
    CHECK(received_deadline==now+1234000 && lock_wait==1234);
    reset();CHECK(wifi_text_until("receipt",7,s_wifi.deadline+1000000));
    CHECK(received_deadline==s_wifi.deadline && lock_wait==5000);
    reset();CHECK(!wifi_text_until("receipt",7,now));
    CHECK(s_wifi.failed && !events && !lock_wait);
    reset();CHECK(command_reply_printf_until(now+765000,"nav %d",7));
    CHECK(received_deadline==now+765000 && received_size==5);
    CHECK(!memcmp(received,"nav 7",5) && !usb_writes);
    reset();CHECK(command_reply_printf("nav %d",7));
    CHECK(received_deadline==now+2000000 && !usb_writes);
    reset();s_wifi.active=false;
    CHECK(command_reply_printf("usb %d",7));
    CHECK(received_size==5 && !memcmp(received,"usb 7",5));
    CHECK(usb_writes && !events);
    reset();CHECK(!command_reply_printf_until(now,"expired"));
    CHECK(s_wifi.failed && !events && !usb_writes);
    CHECK(!lock_depth);
    printf("devtools output: %u checks passed (long receipts, chunks, loss, UTF-8, channels)\n",checks);
    return 0;
}
"""
work.mkdir(parents=True, exist_ok=True)
test = work / "test_output.c"
test.write_text(prelude + sink + hello + reply + tests)
cjson = component.parents[1] / "managed_components/espressif__cjson/cJSON"
binary = work / "test_output"
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
        f"-I{component}",
        f"-I{cjson}",
        str(test),
        str(cjson / "cJSON.c"),
        "-o",
        str(binary),
    ],
    check=True,
)
subprocess.run([str(binary)], check=True)
