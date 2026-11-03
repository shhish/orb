/* Byte-exact frames captured from HomeBridge against hub/tests' fake relay.
 * The same files are regenerated and compared byte-for-byte by pytest. */
#include "link_home.h"
#include "link_proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_checks;
static orb_home_snapshot_t *s_view;
#define CHECK(c) do { s_checks++; if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static const char *s_base = "{\"reset\":true,\"more\":false,\"available\":true,\"rooms\":[{\"id\":\"r1\",\"name\":\"Living Room\"}],\"accessories\":[{\"id\":\"a1\",\"name\":\"Lamp\",\"room\":\"r1\",\"kind\":\"light\",\"reachable\":true,\"favourite\":true,\"state\":{\"on\":true},\"writable\":[\"on\"]}],\"scenes\":[{\"id\":\"s1\",\"name\":\"Evening\",\"favourite\":true}]}";
static const cJSON *get(const cJSON *o, const char *key) { return cJSON_GetObjectItemCaseSensitive(o, key); }
static void refresh(void) { CHECK(orb_home_snapshot(s_view)); }
static link_v11_result_t event(const char *name, const char *body)
{
    cJSON *o = cJSON_Parse(body); CHECK(o);
    link_v11_result_t result = link_home_event(name, o); cJSON_Delete(o); return result;
}
static void base(void)
{
    CHECK(event("home.snapshot", s_base) == LINK_V11_APPLIED);
    refresh(); CHECK(s_view->online && s_view->synced && s_view->available);
}
static void fixture_events(const char *name, bool home)
{
    char path[512], line[16386]; snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, name);
    FILE *f = fopen(path, "r"); CHECK(f);
    unsigned count = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line); CHECK(len && line[len - 1] == '\n'); len--;
        CHECK(len <= 4096);
        link_msg_t m; cJSON *root = link_parse(line, len, &m); CHECK(root && m.kind == LINK_EVT);
        link_v11_result_t result = home ? link_home_event(m.name, m.body) : link_v11_event(m.name, m.body);
        if (result != LINK_V11_APPLIED) fprintf(stderr, "%s chunk %u: %s\n", name, count + 1, home ? link_home_error() : "invalid card");
        CHECK(result == LINK_V11_APPLIED);
        cJSON_Delete(root); count++;
        if (home && !strcmp(name, "snapshot.jsonl") && count < 3) {
            refresh(); CHECK(s_view->accessory_count == 0 && !s_view->synced);
        }
    }
    CHECK(feof(f) && count); CHECK(!fclose(f));
}
static void wire_fixtures(void)
{
    CHECK(orb_model_init()); orb_model_set_connection(true, true);
    fixture_events("snapshot.jsonl", true); refresh();
    CHECK(s_view->accessory_count == 45 && s_view->room_count == 9 && s_view->scene_count == 12 && s_view->synced);
    const orb_home_accessory_t *lamp = orb_home_find(s_view, "acc6ec49fcbbf"); CHECK(lamp && lamp->state.ct == 250);
    fixture_events("updates.jsonl", true); refresh();
    lamp = orb_home_find(s_view, "acc6ec49fcbbf"); CHECK(lamp && lamp->state.hue == 120 && lamp->state.ct == 250);
    CHECK(!orb_home_find(s_view, "a315febc1153f")->reachable);
    char request_path[512], reply_path[512], req_line[4098], res_line[4098];
    snprintf(request_path, sizeof(request_path), "%s/requests.jsonl", FIXTURE_DIR);
    snprintf(reply_path, sizeof(reply_path), "%s/replies.jsonl", FIXTURE_DIR);
    FILE *requests = fopen(request_path, "r"), *replies = fopen(reply_path, "r"); CHECK(requests && replies);
    for (unsigned i = 0; i < 4; i++) {
        CHECK(fgets(req_line, sizeof(req_line), requests) && fgets(res_line, sizeof(res_line), replies));
        link_msg_t req_msg, res_msg;
        cJSON *req_root = link_parse(req_line, strlen(req_line), &req_msg);
        cJSON *res_root = link_parse(res_line, strlen(res_line), &res_msg);
        CHECK(req_root && res_root && req_msg.kind == LINK_REQ && res_msg.kind == LINK_RES && req_msg.id == res_msg.id);
        orb_home_request_t req;
        const char *id = cJSON_GetStringValue(get(req_msg.body, "id")); CHECK(id);
        if (i == 0) {
            orb_home_state_t values = {.present=ORB_HOME_ON|ORB_HOME_BRIGHTNESS|ORB_HOME_SAT,.on=true,.brightness=67,.sat=20};
            CHECK(orb_home_set(id, &values, false));
        } else if (i == 2) CHECK(orb_home_scene(id));
        else CHECK(orb_home_toggle(id));
        CHECK(orb_home_take_request(&req));
        CHECK(!strcmp(link_home_request_method(&req), req_msg.name));
        cJSON *encoded = link_home_request_params(&req); CHECK(encoded && cJSON_Compare(encoded, req_msg.body, true)); cJSON_Delete(encoded);
        orb_home_state_t accepted;
        if (res_msg.ok) {
            CHECK(link_home_read_reply(res_msg.body, &req, &accepted));
            orb_home_complete(&req, &accepted, NULL);
        } else orb_home_complete(&req, NULL, res_msg.code);
        refresh(); CHECK(!s_view->scene_pending);
        lamp = orb_home_find(s_view, "acc6ec49fcbbf"); CHECK(lamp && !lamp->pending);
        if (i == 0) CHECK(lamp->state.brightness == 70 && lamp->state.hue == 0 && lamp->state.on);
        if (i == 1 || i == 3) CHECK(!lamp->state.on);
        if (i == 3) CHECK(s_view->error[0]);
        cJSON_Delete(req_root); cJSON_Delete(res_root);
    }
    CHECK(!fclose(requests) && !fclose(replies));
    cJSON *sessions = cJSON_Parse("{\"sessions\":[],\"reset\":true,\"more\":false}");
    CHECK(sessions && link_v11_event("sessions.snapshot", sessions) == LINK_V11_APPLIED); cJSON_Delete(sessions);
    fixture_events("doorbell.jsonl", false);
    orb_model_snapshot_t *model = calloc(1, sizeof(*model)); CHECK(model && orb_model_snapshot(model));
    CHECK(model->card_count == 1 && model->cards[0].kind == ORB_CARD_ATTENTION && !model->cards[0].reply);
    CHECK(!strncmp(model->cards[0].id, "home-doorbell-", 14) && !strcmp(model->cards[0].options[0].id, "dismiss")); free(model);
    fixture_events("offline.jsonl", true); refresh();
    CHECK(s_view->online && s_view->synced && !s_view->available && s_view->accessory_count == 45);
    CHECK(!orb_home_toggle("acc6ec49fcbbf") && !orb_home_scene("s297178749bb1"));
    for (size_t i = 0; i < s_view->accessory_count; i++) CHECK(!s_view->accessories[i].reachable);
}
static void reject_field(const char *path, const char *key, const char *json)
{
    base(); cJSON *root = cJSON_Parse(s_base); CHECK(root);
    cJSON *object = root;
    if (path) object = cJSON_GetArrayItem(get(root, path), 0);
    CHECK(object);
    cJSON_DeleteItemFromObjectCaseSensitive(object, key);
    if (json) { cJSON *v = cJSON_Parse(json); CHECK(v && cJSON_AddItemToObject(object, key, v)); }
    CHECK(link_home_event("home.snapshot", root) == LINK_V11_INVALID); cJSON_Delete(root);
    refresh(); CHECK(s_view->online && !s_view->synced && s_view->accessory_count == 1);
    CHECK(!orb_home_toggle("a1") && link_home_error()[0]);
}
static void invalid_fields(void)
{
    static const char *const flags[] = {"reset", "more", "available"};
    for (size_t i = 0; i < 3; i++) { reject_field(NULL, flags[i], NULL); reject_field(NULL, flags[i], "1"); }
    static const char *const arrays[] = {"rooms", "accessories", "scenes"};
    for (size_t i = 0; i < 3; i++) { reject_field(NULL, arrays[i], NULL); reject_field(NULL, arrays[i], "{}"); }
    static const char *const accessory_fields[] = {"id","name","room","kind","reachable","favourite","state","writable"};
    for (size_t i = 0; i < 8; i++) reject_field("accessories", accessory_fields[i], NULL);
    reject_field("rooms", "id", "\"12345678901234567\"");
    reject_field("accessories", "id", "\"é\"");
    reject_field("accessories", "room", "\"unknown\"");
    reject_field("accessories", "name", "\"123456789012345678901234567890123\"");
    reject_field("accessories", "name", "\"ééééééééééééééééé\"");
    reject_field("accessories", "kind", "\"temperature\"");
    reject_field("accessories", "kind", "\"unsupported\"");
    reject_field("accessories", "writable", "[\"temp\"]");
    reject_field("accessories", "writable", "[\"on\",\"on\"]");
    reject_field("accessories", "writable", "[\"vendor\"]");
    reject_field("scenes", "favourite", NULL);
    reject_field("scenes", "favourite", "null");
    const char *bad[] = {"{\"on\":1}","{\"on\":null}","{\"ct\":49}","{\"ct\":1001}",
        "{\"target\":-101}","{\"target\":101}","{\"temp\":101}","{\"mode\":\"heat\"}",
        "{\"mode\":1.5}","{\"mode\":true}","{\"mode\":4}","{\"speed\":true}",
        "{\"battery\":1e999}","{\"custom.vendor\":1}","{\"on\":true,\"on\":false}"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) reject_field("accessories", "state", bad[i]);
    base();
    CHECK(event("home.snapshot", "{\"reset\":true,\"more\":true,\"available\":true,\"rooms\":[],\"accessories\":[],\"scenes\":[]}") == LINK_V11_APPLIED);
    CHECK(event("home.snapshot", "{\"reset\":false,\"more\":false,\"available\":false,\"rooms\":[],\"accessories\":[],\"scenes\":[]}") == LINK_V11_INVALID);
    CHECK(event("home.snapshot", "{\"reset\":false,\"more\":false,\"available\":true,\"rooms\":[],\"accessories\":[],\"scenes\":[]}") == LINK_V11_INVALID);
    base();
    CHECK(event("home.snapshot", "{\"reset\":true,\"more\":true,\"available\":true,\"rooms\":[],\"accessories\":[],\"scenes\":[]}") == LINK_V11_APPLIED);
    link_home_reset(); orb_home_set_connection(true, false);
    CHECK(event("home.snapshot", "{\"reset\":false,\"more\":false,\"available\":true,\"rooms\":[],\"accessories\":[],\"scenes\":[]}") == LINK_V11_INVALID);
}
static void valid_boundaries(void)
{
    const char *kinds[] = {"light","switch","outlet","fan","thermostat","lock","sensor","doorbell","button","cover","other"};
    for (size_t i = 0; i < sizeof(kinds)/sizeof(kinds[0]); i++) {
        cJSON *root = cJSON_Parse(s_base), *a = cJSON_GetArrayItem(get(root, "accessories"), 0); CHECK(root && a);
        CHECK(cJSON_ReplaceItemInObjectCaseSensitive(a, "kind", cJSON_CreateString(kinds[i])));
        CHECK(link_home_event("home.snapshot", root) == LINK_V11_APPLIED); cJSON_Delete(root);
    }
    base();
    CHECK(event("home.update", "{\"id\":\"a1\",\"state\":{\"ct\":50,\"temp\":-100,\"target\":-100,\"mode\":0}}") == LINK_V11_APPLIED);
    CHECK(event("home.update", "{\"id\":\"a1\",\"state\":{\"ct\":1000,\"temp\":100,\"target\":100,\"mode\":3}}") == LINK_V11_APPLIED);
    refresh(); CHECK(s_view->accessories[0].state.on && s_view->accessories[0].state.mode == ORB_HOME_MODE_AUTO);
    cJSON *root = cJSON_Parse(s_base), *a = cJSON_GetArrayItem(get(root, "accessories"), 0); CHECK(root && a);
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(a, "id", cJSON_CreateString("1234567890123456")));
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(a, "name", cJSON_CreateString("éééééééééééééééé")));
    CHECK(link_home_event("home.snapshot", root) == LINK_V11_APPLIED); cJSON_Delete(root);
    orb_home_request_t req = {.kind=ORB_HOME_REQUEST_SET,.id="a1",.values={.present=ORB_HOME_MODE,.mode=ORB_HOME_MODE_COOL}};
    cJSON *params = link_home_request_params(&req); CHECK(params && cJSON_IsNumber(get(get(params, "values"), "mode"))); cJSON_Delete(params);
    req.values.present = ORB_HOME_TEMP; CHECK(!link_home_request_params(&req));
    req.values.present = ORB_HOME_MODE; req.values.mode = (orb_home_mode_t)4; CHECK(!link_home_request_params(&req));
    orb_home_state_t accepted;
    const char *bad_replies[] = {"{}","{\"state\":{}}","{\"id\":\"a1\",\"values\":{\"on\":true}}","{\"id\":\"a1\",\"state\":{\"mode\":\"heat\"}}"};
    for (size_t i = 0; i < sizeof(bad_replies)/sizeof(bad_replies[0]); i++) {
        root = cJSON_Parse(bad_replies[i]); CHECK(root && !link_home_read_reply(root, &req, &accepted)); cJSON_Delete(root);
    }
    root = cJSON_Parse("{\"id\":\"a1\",\"state\":{}}"); CHECK(root && link_home_read_reply(root, &req, &accepted) && !accepted.present); cJSON_Delete(root);
    req.kind = ORB_HOME_REQUEST_SCENE;
    root = cJSON_Parse("{\"id\":\"a1\"}"); CHECK(root && !link_home_read_reply(root, &req, &accepted)); cJSON_Delete(root);
    root = cJSON_Parse("{\"id\":\"a1\",\"accepted\":true}"); CHECK(root && link_home_read_reply(root, &req, &accepted)); cJSON_Delete(root);
}
static void maximum_model(void)
{
    /* Every chunk fits the wire ceiling while the aggregate reaches all three
     * model limits. The 65th accessory must invalidate only Home staging. */
    for (unsigned overflow = 0; overflow < 2; overflow++) {
        for (unsigned chunk = 0; chunk < 21; chunk++) {
            cJSON *p = cJSON_CreateObject(); CHECK(p);
            cJSON *rooms = cJSON_AddArrayToObject(p, "rooms"), *accessories = cJSON_AddArrayToObject(p, "accessories"),
                *scenes = cJSON_AddArrayToObject(p, "scenes"); CHECK(rooms && accessories && scenes);
            CHECK(cJSON_AddBoolToObject(p, "available", true) && cJSON_AddBoolToObject(p, "reset", chunk == 0) &&
                cJSON_AddBoolToObject(p, "more", chunk < 20 || overflow));
            if (chunk == 0) for (unsigned i = 0; i < 16; i++) {
                char id[17]; snprintf(id, sizeof(id), "r%u", i);
                cJSON *room = cJSON_CreateObject(); CHECK(room && cJSON_AddStringToObject(room,"id",id) &&
                    cJSON_AddStringToObject(room,"name","Room") && cJSON_AddItemToArray(rooms,room));
            }
            if (chunk > 0 && chunk <= 16) for (unsigned i = 0; i < 4; i++) {
                cJSON *base_root = cJSON_Parse(s_base); CHECK(base_root);
                cJSON *a = cJSON_Duplicate(cJSON_GetArrayItem(get(base_root, "accessories"), 0), true); CHECK(a); cJSON_Delete(base_root);
                char id[17]; snprintf(id, sizeof(id), "a%u", (chunk - 1) * 4 + i);
                CHECK(cJSON_ReplaceItemInObjectCaseSensitive(a,"id",cJSON_CreateString(id)) && cJSON_AddItemToArray(accessories,a));
            }
            if (chunk >= 17) for (unsigned i = 0; i < 8; i++) {
                char id[17]; snprintf(id, sizeof(id), "s%u", (chunk - 17) * 8 + i);
                cJSON *scene = cJSON_CreateObject(); CHECK(scene && cJSON_AddStringToObject(scene,"id",id) &&
                    cJSON_AddStringToObject(scene,"name","Scene") && cJSON_AddBoolToObject(scene,"favourite",true) &&
                    cJSON_AddItemToArray(scenes,scene));
            }
            char *wire = link_encode_evt("home.snapshot",p); CHECK(wire && strlen(wire) <= 4096);
            link_msg_t m; cJSON *decoded = link_parse(wire,strlen(wire),&m); CHECK(decoded);
            CHECK(link_home_event(m.name,m.body) == LINK_V11_APPLIED); cJSON_Delete(decoded); cJSON_free(wire);
        }
        if (overflow) CHECK(event("home.snapshot", "{\"reset\":false,\"more\":false,\"available\":true,\"rooms\":[],\"accessories\":[{\"id\":\"a65\",\"name\":\"Overflow\",\"room\":\"r1\",\"kind\":\"light\",\"reachable\":true,\"favourite\":false,\"state\":{},\"writable\":[]}],\"scenes\":[]}") == LINK_V11_INVALID);
        refresh(); CHECK(s_view->accessory_count == 64 && s_view->room_count == 16 && s_view->scene_count == 32);
        CHECK(s_view->synced == !overflow && s_view->online);
    }
    fixture_events("empty.jsonl", true); refresh();
    CHECK(s_view->synced && !s_view->available && !s_view->accessory_count && !s_view->room_count && !s_view->scene_count);
}
int main(void)
{
    CHECK(orb_home_init()); s_view = calloc(1, sizeof(*s_view)); CHECK(s_view);
    orb_home_set_connection(true, false);
    wire_fixtures(); invalid_fields(); valid_boundaries(); maximum_model();
    free(s_view); printf("Home byte-exact wire: %u checks passed\n", s_checks); return 0;
}
