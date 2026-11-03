"""Drive the production screenshot lifetime and gesture loop with fake LVGL."""

import subprocess
import sys
from pathlib import Path

component = Path(__file__).resolve().parents[2]
work = Path(sys.argv[1] if len(sys.argv) > 1 else "build").resolve()
text = (component / "orb_devtools.c").read_text()
shot = text[text.index("static void send_shot(void)") : text.index("static void print_power(void)")]
glide = text[text.index("static void glide(") : text.index("/* A bounded, repeatable live scene")]
prelude = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_arc_command.h"
#define LINE_BYTES 192
#define LV_RESULT_OK 0
#define pdTRUE 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdMS_TO_TICKS(ms) (ms)
#define SHOT_PARTS 4
#define LV_MIN(a,b) ((a)<(b)?(a):(b))
typedef struct {struct {uint32_t w,h,stride;} header;uint8_t *data;} lv_draw_buf_t;
typedef struct {lv_draw_buf_t *part[SHOT_PARTS];uint32_t w,h,part_rows;} shot_t;
static shot_t *s_capture;
static int s_captured;
static struct {bool active,failed;} s_wifi;
static int scenario,allocations,destroyed,completions,cancels,locks,touches,releases,delays;
static int wakes,last_x,last_y;
static unsigned checks,failures,headers,ends;
static size_t crc_bytes,encoded_bytes;
static bool queued,current=true;
#define CHECK(c) do {checks++;if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
static shot_t *image(void) {
    shot_t *shot=calloc(1,sizeof(*shot));CHECK(shot!=NULL);
    shot->w=466;shot->h=466;shot->part_rows=116;
    for(unsigned p=0;p<SHOT_PARTS;p++) {
        lv_draw_buf_t *buf=calloc(1,sizeof(*buf));CHECK(buf!=NULL);shot->part[p]=buf;
        buf->header.w=466;buf->header.h=p==3?118:116;buf->header.stride=932;
        size_t size=(size_t)buf->header.stride*buf->header.h;
        buf->data=malloc(size);CHECK(buf->data!=NULL);
        for(size_t i=0;i<size;i++)buf->data[i]=(uint8_t)((p*116u*932u+i)%251);
    }
    allocations++;return shot;
}
static void capture(void *arg) {(void)arg;s_capture=image();completions++;}
static void shot_free(shot_t *shot) {
    if(!shot)return;
    for(unsigned p=0;p<SHOT_PARTS;p++){free(shot->part[p]->data);free(shot->part[p]);}
    free(shot);destroyed++;
}
static void lv_lock(void) {locks++;if(scenario==3 && locks==2 && queued){capture(NULL);queued=false;}}
static void lv_unlock(void) {}
static int lv_async_call(void (*fn)(void *),void *arg) {(void)fn;(void)arg;if(scenario==1)return 1;queued=true;return 0;}
static void orb_display_wake_service(void) { CHECK(queued); }
static void lv_async_call_cancel(void (*fn)(void *),void *arg) {(void)fn;(void)arg;queued=false;cancels++;}
static int xSemaphoreTake(int sem,int wait) {
    (void)sem;
    if(!wait) {if(completions){completions--;return pdTRUE;}return 0;}
    if(scenario==2 || scenario==3) return 0;
    if(queued){capture(NULL);queued=false;}
    if(completions){completions--;return pdTRUE;}return 0;
}
static uint32_t esp_rom_crc32_le(uint32_t seed,const uint8_t *data,size_t size) {
    (void)seed;CHECK(size==116u*932u || size==118u*932u);
    for(size_t i=0;i<size;i++)CHECK(data[i]==(crc_bytes+i)%251);
    crc_bytes+=size;return 0;
}
static void *heap_caps_malloc(size_t size,int caps) {(void)caps;return malloc(size);}
static void heap_caps_free(void *p) {free(p);}
static void base64(const uint8_t *in,size_t size,char *out) {
    CHECK(size<=192);
    for(size_t i=0;i<size;i++)CHECK(in[i]==(encoded_bytes+i)%251);
    encoded_bytes+=size;strcpy(out,"AAAA");
}
static bool wifi_current(void) {return current;}
static int fake_printf(const char *format,...) {
    if(strstr(format,"@@ORBSHOT FAIL"))failures++;
    if(strstr(format,"@@ORBSHOT BEGIN"))headers++;
    if(strstr(format,"@@ORBSHOT END"))ends++;
    return 0;
}
#define printf fake_printf
static bool wake_display(void) {wakes++;return true;}
static void set_touch(int x,int y,bool pressed) {CHECK(wakes>0);last_x=x;last_y=y;if(pressed)touches++;else releases++;}
static void vTaskDelay(int ms) {CHECK(ms==10);delays++;if(scenario==4 && delays==2)current=false;}
"""
tests = r"""
#undef printf
int main(void) {
    for(scenario=1;scenario<=3;scenario++) {
        locks=0;int before=destroyed;
        send_shot();
        CHECK(!s_capture && !completions && !queued);
        CHECK(failures==(unsigned)scenario && !headers && !ends);
        CHECK(destroyed-before==(scenario==3));
    }
    scenario=0;locks=0;
    send_shot();
    CHECK(!s_capture && !completions && !queued);
    CHECK(headers==1 && ends==1 && allocations==destroyed);
    CHECK(crc_bytes==434312 && encoded_bytes==434312);
    scenario=4;s_wifi.active=true;current=true;
    glide(0,0,465,465,100);
    CHECK(touches==2 && delays==2 && s_wifi.failed);
    scenario=0;s_wifi.active=false;touches=0;delays=0;
    glide(0,0,465,465,100);
    CHECK(touches==11 && delays==11);
    CHECK(wakes==2);
    orb_devtools_arc_t arc={200,0,90,400};
    touches=delays=releases=0;
    glide_arc(&arc);
    CHECK(wakes==3 && touches==41 && delays==41 && releases==1);
    CHECK(last_x==433 && last_y==233);
    arc=(orb_devtools_arc_t){200,10,-10,400};
    touches=delays=releases=0;
    glide_arc(&arc);
    CHECK(wakes==4 && touches==41 && delays==41 && releases==1);
    CHECK(last_x<233 && last_y<40);
    scenario=4;s_wifi.active=true;current=true;s_wifi.failed=false;
    touches=delays=releases=0;
    glide_arc(&arc);
    CHECK(wakes==5 && touches==2 && delays==2 && releases==1 && s_wifi.failed);
    printf("devtools async: %u checks passed (queued/late capture, no stale image, cancelled gesture)\n",checks);
    return 0;
}
"""
work.mkdir(parents=True, exist_ok=True)
source = work / "async.c"
source.write_text(prelude + shot + glide + tests)
binary = work / "async"
subprocess.run(
    [
        "clang",
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-O1",
        "-g",
        "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all",
        "-I" + str(component),
        str(source),
        "-o",
        str(binary),
    ],
    check=True,
)
subprocess.run([str(binary)], check=True)
