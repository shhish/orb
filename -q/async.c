
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
typedef struct {struct {uint32_t w,h,stride;} header;uint8_t *data;} lv_draw_buf_t;
static lv_draw_buf_t *s_capture;
static int s_captured;
static struct {bool active,failed;} s_wifi;
static int scenario,allocations,destroyed,completions,cancels,locks,touches,releases,delays;
static int wakes,last_x,last_y;
static unsigned checks,failures,headers,ends;
static bool queued,current=true;
#define CHECK(c) do {checks++;if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
static lv_draw_buf_t *image(void) {
    lv_draw_buf_t *buf=calloc(1,sizeof(*buf));CHECK(buf!=NULL);
    buf->header.w=466;buf->header.h=466;buf->header.stride=932;
    buf->data=calloc(466*466,2);CHECK(buf->data!=NULL);allocations++;return buf;
}
static void capture(void *arg) {(void)arg;s_capture=image();completions++;}
static void snapshot_free(lv_draw_buf_t *buf) {if(!buf)return;free(buf->data);free(buf);destroyed++;}
static void lv_lock(void) {locks++;if(scenario==3 && locks==2 && queued){capture(NULL);queued=false;}}
static void lv_unlock(void) {}
static int lv_async_call(void (*fn)(void *),void *arg) {(void)fn;(void)arg;if(scenario==1)return 1;queued=true;return 0;}
static void lv_async_call_cancel(void (*fn)(void *),void *arg) {(void)fn;(void)arg;queued=false;cancels++;}
static int xSemaphoreTake(int sem,int wait) {
    (void)sem;
    if(!wait) {if(completions){completions--;return pdTRUE;}return 0;}
    if(scenario==2 || scenario==3) return 0;
    if(queued){capture(NULL);queued=false;}
    if(completions){completions--;return pdTRUE;}return 0;
}
static uint32_t esp_rom_crc32_le(uint32_t seed,const uint8_t *data,size_t size) {(void)seed;(void)data;CHECK(size==434312);return 0;}
static void *heap_caps_malloc(size_t size,int caps) {(void)caps;return malloc(size);}
static void heap_caps_free(void *p) {free(p);}
static void base64(const uint8_t *in,size_t size,char *out) {(void)in;CHECK(size<=192);strcpy(out,"AAAA");}
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
static void send_shot(void)
{
    lv_lock();
    xSemaphoreTake(s_captured, 0);
    s_capture = NULL;
    bool queued = lv_async_call(capture, NULL) == LV_RESULT_OK;
    lv_unlock();
    if (!queued || xSemaphoreTake(s_captured, pdMS_TO_TICKS(5000)) != pdTRUE) {
        lv_lock();
        lv_async_call_cancel(capture, NULL);
        /* The callback can finish while timeout cleanup waits for LVGL. It
         * must not leave an old image/completion for the next screenshot. */
        xSemaphoreTake(s_captured, 0);
        snapshot_free(s_capture);
        s_capture = NULL;
        lv_unlock();
        printf("\n@@ORBSHOT FAIL timeout\n");
        return;
    }
    lv_draw_buf_t *buf = s_capture;
    s_capture = NULL;
    if (!buf) {
        printf("\n@@ORBSHOT FAIL no memory\n");
        return;
    }

    /* Pack rows tightly (the snapshot's stride may be padded). */
    uint32_t w = buf->header.w, h = buf->header.h, row = w * 2;
    if (buf->header.stride != row) {
        for (uint32_t y = 1; y < h; y++) {
            memmove(buf->data + y * row, buf->data + y * buf->header.stride, row);
        }
    }
    uint32_t len = row * h, crc = esp_rom_crc32_le(0, buf->data, len);

    char *line = heap_caps_malloc(LINE_BYTES * 4 / 3 + 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!line) {
        printf("\n@@ORBSHOT FAIL no memory\n");
        snapshot_free(buf);
        return;
    }
    printf("\n@@ORBSHOT BEGIN %lu %lu rgb565le %d %lu %08lx\n", (unsigned long)w, (unsigned long)h, LINE_BYTES,
           (unsigned long)len, (unsigned long)crc);
    char *batch = s_wifi.active ? heap_caps_malloc(1500, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    size_t used = 0;
    if (s_wifi.active && !batch) s_wifi.failed = true;
    for (uint32_t off = 0, i = 0; off < len; off += LINE_BYTES, i++) {
        if (s_wifi.active && (!wifi_current() || s_wifi.failed)) break;
        base64(buf->data + off, len - off < LINE_BYTES ? len - off : LINE_BYTES, line);
        if (batch) {
            if (used + 280 >= 1500) { printf("%s", batch); used = 0; }
            used += snprintf(batch + used, 1500 - used, "@@%05lx %s\n", (unsigned long)i, line);
        } else printf("@@%05lx %s\n", (unsigned long)i, line);
    }
    if (used) printf("%s", batch);
    heap_caps_free(batch);
    printf("@@ORBSHOT END\n");
    fflush(stdout);
    heap_caps_free(line);
    snapshot_free(buf);
}

static void glide(int x1, int y1, int x2, int y2, int ms)
{
    wake_display();
    int steps = ms / 10 > 1 ? ms / 10 : 1;
    for (int i = 0; i <= steps; i++) {
        if (s_wifi.active && !wifi_current()) { s_wifi.failed = true; break; }
        set_touch(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps, true);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void glide_arc(const orb_devtools_arc_t *arc)
{
    wake_display();
    int steps = arc->duration_ms / 10 > 1 ? arc->duration_ms / 10 : 1;
    int x = 233, y = 233;
    for (int i = 0; i <= steps; i++) {
        if (s_wifi.active && !wifi_current()) { s_wifi.failed = true; break; }
        orb_devtools_arc_point(arc, (float)i / steps, &x, &y);
        set_touch(x, y, true);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    set_touch(x, y, false);
}


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
