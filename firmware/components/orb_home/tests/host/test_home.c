#include "orb_home.h"
#include "link_home.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static orb_home_snapshot_t *view;
static link_v11_result_t event(const char *name, const char *json)
{
    cJSON *p = cJSON_Parse(json); CHECK(p); link_v11_result_t result = link_home_event(name, p); cJSON_Delete(p); return result;
}
static void refresh(void) { CHECK(orb_home_snapshot(view)); }
static void fixtures(void)
{
    orb_home_load_demo(); refresh();
    CHECK(view->accessory_count == 18 && view->room_count == 5 && view->scene_count == 5);
    CHECK(orb_home_snapshot_chunk(view->rooms, view->room_count, view->accessories, view->accessory_count, view->scenes, view->scene_count, true, false));
    orb_home_set_connection(true, true); refresh(); CHECK(!view->demo);
}
static void model_tests(void)
{
    fixtures();
    uint64_t rev = view->revision; CHECK(!orb_home_snapshot_if_changed(rev, view));
    orb_home_set_connection(true, true); CHECK(orb_home_revision() == rev);
    CHECK(orb_home_toggle("floor-lamp")); refresh();
    CHECK(!orb_home_find(view,"floor-lamp")->state.on && orb_home_find(view,"floor-lamp")->pending);
    CHECK(!orb_home_toggle("floor-lamp") && !orb_home_toggle("front-door") && !orb_home_toggle("bookshelf"));
    orb_home_request_t req; CHECK(orb_home_take_request(&req)); CHECK(!orb_home_take_request(&(orb_home_request_t){0}));
    orb_home_state_t actual = {.present = ORB_HOME_BRIGHTNESS, .brightness = 42};
    CHECK(orb_home_update("floor-lamp", &actual, false, false, 0));
    orb_home_complete(&req, NULL, "unreachable"); refresh();
    CHECK(orb_home_find(view,"floor-lamp")->state.on && orb_home_find(view,"floor-lamp")->state.brightness == 42);
    CHECK(!strcmp(view->error,"Couldn’t reach Floor Lamp"));
    CHECK(orb_home_toggle("floor-lamp")); CHECK(orb_home_take_request(&req));
    actual = (orb_home_state_t){.present=ORB_HOME_ON | ORB_HOME_BRIGHTNESS,.on=true,.brightness=33};
    CHECK(orb_home_update("floor-lamp", &actual, false, false, 0));
    orb_home_state_t stale = {.present=ORB_HOME_ON | ORB_HOME_BRIGHTNESS,.on=false,.brightness=42};
    orb_home_complete(&req,&stale,NULL); refresh();
    CHECK(orb_home_find(view,"floor-lamp")->state.on && orb_home_find(view,"floor-lamp")->state.brightness == 33);
    CHECK(orb_home_toggle("floor-lamp")); CHECK(orb_home_take_request(&req));
    /* Snapshot reset supersedes the old command; a late reply cannot install
     * optimism or canonical fields into the replacement snapshot. */
    refresh(); view->accessories[0].state.on=true;
    CHECK(orb_home_snapshot_chunk(view->rooms,view->room_count,view->accessories,view->accessory_count,
        view->scenes,view->scene_count,true,false));
    orb_home_complete(&req,&stale,NULL); refresh();
    CHECK(orb_home_find(view,"floor-lamp")->state.on && !orb_home_find(view,"floor-lamp")->pending);
    CHECK(!orb_home_set("front-door", &(orb_home_state_t){.present=ORB_HOME_LOCKED,.locked=false},false));
    CHECK(orb_home_set("front-door", &(orb_home_state_t){.present=ORB_HOME_LOCKED,.locked=false},true));
    CHECK(orb_home_take_request(&req));
    actual = (orb_home_state_t){.present=ORB_HOME_LOCKED,.locked=false};
    orb_home_complete(&req, &actual, NULL); refresh(); CHECK(!orb_home_find(view,"front-door")->state.locked);
    CHECK(!orb_home_set("floor-lamp", &(orb_home_state_t){.present=ORB_HOME_BRIGHTNESS,.brightness=NAN},false));
    CHECK(!orb_home_set("floor-lamp", &(orb_home_state_t){.present=ORB_HOME_BRIGHTNESS,.brightness=101},false));
    CHECK(!orb_home_set("hall-motion", &(orb_home_state_t){.present=ORB_HOME_ON,.on=true},false));
    CHECK(orb_home_toggle("floor-lamp")); CHECK(orb_home_take_request(&req));
    orb_home_set_connection(false,false); refresh();
    CHECK(orb_home_find(view,"floor-lamp")->state.on && !orb_home_find(view,"floor-lamp")->pending);
    CHECK(!orb_home_toggle("kitchen") && !orb_home_scene("evening"));
    orb_home_complete(&req,NULL,NULL); refresh(); CHECK(orb_home_find(view,"floor-lamp")->state.on);
    fixtures();
    CHECK(orb_home_toggle("floor-lamp"));
    CHECK(orb_home_update("floor-lamp", &(orb_home_state_t){0}, true, false, 0));
    CHECK(!orb_home_take_request(&req)); refresh();
    CHECK(!orb_home_find(view,"floor-lamp")->reachable && !orb_home_find(view,"floor-lamp")->pending && orb_home_find(view,"floor-lamp")->state.on);
    fixtures();
    CHECK(orb_home_toggle("floor-lamp") && orb_home_toggle("sofa-lamp") && orb_home_toggle("bedside") && orb_home_toggle("kitchen"));
    CHECK(!orb_home_toggle("office"));
    orb_home_set_connection(false,false);
    fixtures(); view->scenes[0].contains_unlock = true;
    CHECK(orb_home_snapshot_chunk(view->rooms,view->room_count,view->accessories,view->accessory_count,view->scenes,view->scene_count,true,false));
    CHECK(!orb_home_scene("morning") && orb_home_scene("evening"));
    CHECK(orb_home_take_request(&req)); orb_home_complete(&req,NULL,"error"); refresh(); CHECK(!view->scene_pending && view->error[0]);
    fixtures();
    CHECK(orb_home_snapshot_chunk(view->rooms,view->room_count,view->accessories,1,NULL,0,true,true));
    CHECK(!orb_home_snapshot_chunk(NULL,0,view->accessories,1,NULL,0,false,false));
    refresh(); CHECK(view->accessory_count == 18); /* Invalid stage never leaks. */
    CHECK(!orb_home_snapshot_chunk(NULL,0,view->accessories,65,NULL,0,true,false));
    fixtures();
    CHECK(orb_home_snapshot_chunk(view->rooms,view->room_count,view->accessories,1,NULL,0,true,true));
    refresh(); CHECK(view->accessory_count == 18);
    CHECK(orb_home_snapshot_chunk(NULL,0,view->accessories+1,1,NULL,0,false,false));
    refresh(); CHECK(view->accessory_count == 2);
}
static void parser_tests(void)
{
    orb_home_reset(); link_home_reset(); orb_home_set_connection(true,false);
    CHECK(event("home.snapshot", "{\"reset\":true,\"more\":true,\"available\":true,\"rooms\":[{\"id\":\"r1\",\"name\":\"Living Room\"}],\"accessories\":[{\"id\":\"a\",\"name\":\"Floor Lamp\",\"room\":\"r1\",\"kind\":\"light\",\"reachable\":true,\"favourite\":true,\"state\":{\"on\":true,\"brightness\":60},\"writable\":[\"on\",\"brightness\"]}],\"scenes\":[]}") == LINK_V11_APPLIED);
    refresh(); CHECK(!view->synced && view->accessory_count == 0);
    CHECK(event("home.snapshot", "{\"reset\":false,\"more\":false,\"available\":true,\"rooms\":[],\"accessories\":[],\"scenes\":[{\"id\":\"s\",\"name\":\"Evening\",\"favourite\":true}]}") == LINK_V11_APPLIED);
    refresh(); CHECK(view->synced && view->available && view->accessory_count == 1 && view->scene_count == 1);
    CHECK(event("home.update", "{\"id\":\"a\",\"state\":{\"brightness\":50},\"future\":null}") == LINK_V11_APPLIED);
    refresh(); CHECK(view->accessories[0].state.on && view->accessories[0].state.brightness == 50);
    CHECK(event("home.update", "{\"id\":\"a\",\"reachable\":false}") == LINK_V11_APPLIED);
    refresh(); CHECK(!view->accessories[0].reachable);
    CHECK(event("home.update", "{\"id\":\"a\",\"state\":{\"on\":1}}") == LINK_V11_INVALID);
    refresh(); CHECK(view->online && !view->synced && view->accessory_count == 1);
    CHECK(event("home.update", "{\"id\":\"a\",\"state\":{\"brightness\":101}}") == LINK_V11_INVALID);
    CHECK(event("home.update", "{\"id\":\"a\",\"id\":\"b\",\"state\":{}}") == LINK_V11_INVALID);
    CHECK(event("unknown.event", "{}") == LINK_V11_IGNORED);
    CHECK(event("home.future", "{}") == LINK_V11_IGNORED);
    CHECK(event("home.snapshot", "{\"reset\":true,\"more\":true,\"available\":true,\"rooms\":[{\"id\":\"r1\",\"name\":\"Bedroom\"}],\"accessories\":[],\"scenes\":[]}") == LINK_V11_APPLIED);
    CHECK(event("home.update", "{\"id\":\"a\",\"state\":{\"on\":true}}") == LINK_V11_INVALID);
    CHECK(event("home.snapshot", "{\"reset\":false,\"more\":false,\"available\":true,\"rooms\":[],\"accessories\":[],\"scenes\":[]}") == LINK_V11_INVALID);
    fixtures(); CHECK(orb_home_toggle("floor-lamp")); orb_home_request_t req; CHECK(orb_home_take_request(&req));
    cJSON *params = link_home_request_params(&req); CHECK(params); CHECK(!strcmp(link_home_request_method(&req),"home.toggle"));
    cJSON_Delete(params);
    orb_home_request_t set = {.kind=ORB_HOME_REQUEST_SET,.id="floor-lamp",
        .values={.present=ORB_HOME_BRIGHTNESS,.brightness=NAN}};
    CHECK(!link_home_request_params(&set));
    set.values.brightness=60; params=link_home_request_params(&set); CHECK(params); cJSON_Delete(params);
    orb_home_state_t accepted; cJSON *reply = cJSON_Parse("{\"id\":\"floor-lamp\",\"state\":{\"on\":false}}");
    CHECK(link_home_read_reply(reply,&req,&accepted) && !accepted.on); cJSON_Delete(reply);
    reply = cJSON_Parse("{\"id\":\"other\",\"state\":{\"on\":false}}"); CHECK(!link_home_read_reply(reply,&req,&accepted)); cJSON_Delete(reply);
    reply = cJSON_Parse("{\"accepted\":true}"); CHECK(!link_home_read_reply(reply,&req,&accepted)); cJSON_Delete(reply);
    link_home_reset(); refresh(); CHECK(!view->online && !view->synced);
}
int main(void)
{
    CHECK(orb_home_init()); view = calloc(1,sizeof(*view)); CHECK(view);
    model_tests(); parser_tests(); free(view); printf("home model/link: %u checks passed\n", checks); return 0;
}
