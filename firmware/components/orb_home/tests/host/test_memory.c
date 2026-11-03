/* Compile the target's real PSRAM branch, including allocation failure. */
#include "orb_home.h"
#include "orb_memory.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, allocations;
static bool fail = true;
static void *payload;
static orb_home_snapshot_t view;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    CHECK(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); allocations++;
    CHECK(count * size < 65536); /* Two bounded snapshots + four writes. */
    if (fail) return NULL;
    payload = calloc(count,size); CHECK(payload); return payload;
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) { return storage; }
int xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t ticks) { CHECK(semaphore && ticks == portMAX_DELAY); return 1; }
int xSemaphoreGive(SemaphoreHandle_t semaphore) { CHECK(semaphore); return 1; }
int main(void)
{
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_MODEL, &stats);
    CHECK(stats.used == 0);
    CHECK(orb_memory_reserve(ORB_MEMORY_MODEL, stats.budget));
    CHECK(!orb_home_init() && allocations == 0);
    orb_memory_release(ORB_MEMORY_MODEL, stats.budget);
    CHECK(!orb_home_init()); CHECK(allocations == 1);
    CHECK(!orb_home_snapshot(&view) && orb_home_revision() == 0);
    orb_memory_get(ORB_MEMORY_MODEL, &stats);
    CHECK(stats.used == 0); /* Failed PSRAM allocation rolls back admission. */
    fail = false; CHECK(orb_home_init()); CHECK(allocations == 2);
    size_t retained = orb_home_memory_bytes();
    orb_memory_get(ORB_MEMORY_MODEL, &stats);
    CHECK(stats.used == retained && retained > 0);
    CHECK(orb_home_init() && allocations == 2);
    orb_memory_get(ORB_MEMORY_MODEL, &stats);
    CHECK(stats.used == retained); /* Idempotent init cannot charge twice. */
    orb_home_set_connection(true,false);
    orb_home_room_t room = {.id="r0", .name="Room"};
    for (size_t i=0;i<ORB_HOME_MAX_ACCESSORIES;i++) {
        orb_home_accessory_t a={.room="r0",.kind=ORB_HOME_SWITCH,.reachable=true,.writable=ORB_HOME_ON,.state={.present=ORB_HOME_ON,.on=true}};
        snprintf(a.id,sizeof(a.id),"a%zu",i); strcpy(a.name,"Switch");
        CHECK(orb_home_snapshot_chunk(i==0?&room:NULL,i==0?1:0,&a,1,NULL,0,i==0,i+1<ORB_HOME_MAX_ACCESSORIES));
    }
    CHECK(orb_home_snapshot(&view) && view.accessory_count == 64 && view.synced);
    CHECK(orb_home_room_off("r0"));
    orb_home_request_t request;
    unsigned completed = 0;
    while (orb_home_take_request(&request)) { orb_home_complete(&request,&request.values,NULL); completed++; }
    CHECK(completed == 64 && orb_home_memory_bytes() == retained && allocations == 2);
    CHECK(orb_home_toggle("a0")); CHECK(orb_home_take_request(&request));
    orb_home_complete(&request,NULL,"failure");
    orb_home_reset(); CHECK(orb_home_snapshot(&view) && !view.accessory_count);
    orb_memory_get(ORB_MEMORY_MODEL, &stats);
    CHECK(stats.used == retained);
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    CHECK(stats.used == 0); /* The service has no presentation allocation. */
    CHECK(allocations == 2); free(payload);
    printf("home PSRAM: %u checks passed, %zu retained bytes\n",checks,retained); return 0;
}
