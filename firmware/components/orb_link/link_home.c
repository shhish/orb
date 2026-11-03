#include "link_home.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

static orb_home_snapshot_t *s_chunk;
static bool s_open, s_available;
static const char *s_error = "invalid fields";
static const cJSON *item(const cJSON *o, const char *key) { return cJSON_GetObjectItemCaseSensitive(o, key); }
const char *link_home_error(void) { return s_error; }
static bool object_ok(const cJSON *o)
{
    if (!cJSON_IsObject(o)) return false;
    for (const cJSON *a = o->child; a; a = a->next) {
        if (!a->string) return false;
        for (const cJSON *b = a->next; b; b = b->next) if (!b->string || !strcmp(a->string, b->string)) return false;
    }
    return true;
}
static bool text(const cJSON *v, char out[33])
{
    const char *s = cJSON_GetStringValue(v);
    if (!s || strlen(s) > ORB_HOME_NAME_MAX) return false;
    memcpy(out, s, strlen(s) + 1); return true;
}
static bool alias(const cJSON *v, char out[33])
{
    const char *s = cJSON_GetStringValue(v);
    if (!s || !s[0] || strlen(s) > ORB_HOME_ID_MAX) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) if (*p < 0x20 || *p > 0x7e) return false;
    memcpy(out, s, strlen(s) + 1); return true;
}
static bool read_bool(const cJSON *o, const char *key, bool *out)
{
    const cJSON *v = item(o, key);
    if (!cJSON_IsBool(v)) return false;
    *out = cJSON_IsTrue(v); return true;
}
static const struct { const char *name; uint32_t bit; size_t offset; bool boolean; float min, max; } fields[] = {
#define F(name, bit, lo, hi) {#name, bit, offsetof(orb_home_state_t, name), false, lo, hi}
#define B(name, bit) {#name, bit, offsetof(orb_home_state_t, name), true, 0, 0}
    B(on, ORB_HOME_ON), F(brightness, ORB_HOME_BRIGHTNESS, 0, 100), F(hue, ORB_HOME_HUE, 0, 360),
    F(sat, ORB_HOME_SAT, 0, 100), F(ct, ORB_HOME_CT, 50, 1000), F(temp, ORB_HOME_TEMP, -100, 100),
    F(target, ORB_HOME_TARGET, -100, 100), F(humidity, ORB_HOME_HUMIDITY_VALUE, 0, 100),
    B(motion, ORB_HOME_MOTION_VALUE), B(contact, ORB_HOME_CONTACT_VALUE), B(locked, ORB_HOME_LOCKED),
    F(speed, ORB_HOME_SPEED, 0, 100), F(position, ORB_HOME_POSITION, 0, 100),
    F(battery, ORB_HOME_BATTERY, 0, 100), B(low_battery, ORB_HOME_LOW_BATTERY),
#undef F
#undef B
};
static uint32_t field_bit(const char *name)
{
    if (!name) return 0;
    if (!strcmp(name, "mode")) return ORB_HOME_MODE;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) if (!strcmp(name, fields[i].name)) return fields[i].bit;
    return 0;
}
static bool read_state(const cJSON *o, orb_home_state_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!object_ok(o)) return false;
    for (const cJSON *v = o->child; v; v = v->next) if (!field_bit(v->string)) return false;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        const cJSON *v = item(o, fields[i].name);
        if (!v) continue; /* Omission, never null/zero, means unknown. */
        char *dest = (char *)out + fields[i].offset;
        if (fields[i].boolean) { if (!cJSON_IsBool(v)) return false; *(bool *)dest = cJSON_IsTrue(v); }
        else {
            if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < fields[i].min || v->valuedouble > fields[i].max) return false;
            *(float *)dest = (float)v->valuedouble;
        }
        out->present |= fields[i].bit;
    }
    const cJSON *mode = item(o, "mode");
    if (mode) {
        if (!cJSON_IsNumber(mode) || !isfinite(mode->valuedouble) || mode->valuedouble < 0 ||
            mode->valuedouble > 3 || floor(mode->valuedouble) != mode->valuedouble) return false;
        out->mode = (orb_home_mode_t)mode->valueint;
        out->present |= ORB_HOME_MODE;
    }
    return true;
}
static bool read_kind(const char *s, orb_home_kind_t *out)
{
    static const struct { const char *name; orb_home_kind_t kind; } kinds[] = {
        {"light", ORB_HOME_LIGHT}, {"switch", ORB_HOME_SWITCH}, {"outlet", ORB_HOME_OUTLET},
        {"fan", ORB_HOME_FAN}, {"thermostat", ORB_HOME_THERMOSTAT}, {"lock", ORB_HOME_LOCK},
        {"sensor", ORB_HOME_SENSOR}, {"doorbell", ORB_HOME_DOORBELL}, {"button", ORB_HOME_BUTTON},
        {"cover", ORB_HOME_COVER}, {"other", ORB_HOME_OTHER},
    };
    if (s) for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
        if (!strcmp(s, kinds[i].name)) { *out = kinds[i].kind; return true; }
    return false;
}
static bool read_accessory(const cJSON *o, orb_home_accessory_t *a)
{
    if (!object_ok(o) || !alias(item(o, "id"), a->id) || !text(item(o, "name"), a->name) ||
        !alias(item(o, "room"), a->room) || !read_bool(o, "reachable", &a->reachable) ||
        !read_bool(o, "favourite", &a->favourite) || !read_kind(cJSON_GetStringValue(item(o, "kind")), &a->kind) ||
        !read_state(item(o, "state"), &a->state)) return false;
    const cJSON *writable = item(o, "writable"), *entry;
    if (!cJSON_IsArray(writable) || cJSON_GetArraySize(writable) > 10) return false;
    cJSON_ArrayForEach(entry, writable) {
        uint32_t bit = field_bit(cJSON_GetStringValue(entry));
        if (!bit || !(bit & ORB_HOME_WRITABLE) || (bit & a->writable)) return false;
        a->writable |= bit;
    }
    return true;
}
static bool read_arrays(const cJSON *params)
{
    const cJSON *a = item(params, "rooms"), *entry;
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) > ORB_HOME_MAX_ROOMS) return false;
    cJSON_ArrayForEach(entry, a) {
        orb_home_room_t *r = &s_chunk->rooms[s_chunk->room_count++];
        if (!object_ok(entry) || !alias(item(entry, "id"), r->id) || !text(item(entry, "name"), r->name)) return false;
    }
    a = item(params, "accessories");
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) > ORB_HOME_MAX_ACCESSORIES) return false;
    cJSON_ArrayForEach(entry, a) if (!read_accessory(entry, &s_chunk->accessories[s_chunk->accessory_count++])) return false;
    a = item(params, "scenes");
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) > ORB_HOME_MAX_SCENES) return false;
    cJSON_ArrayForEach(entry, a) {
        orb_home_scene_t *s = &s_chunk->scenes[s_chunk->scene_count++];
        if (!object_ok(entry) || !alias(item(entry, "id"), s->id) || !text(item(entry, "name"), s->name) ||
            !read_bool(entry, "favourite", &s->favourite)) return false;
    }
    return true;
}
void link_home_reset(void)
{
    s_open = false; orb_home_snapshot_discard(); orb_home_set_connection(false, false);
}
link_v11_result_t link_home_event(const char *name, const cJSON *params)
{
    if (!name || (strcmp(name, "home.snapshot") && strcmp(name, "home.update"))) return LINK_V11_IGNORED;
    s_error = "invalid fields";
    if (!object_ok(params)) goto invalid;
    if (!strcmp(name, "home.update")) {
        /* Home has no resync request. Once a delta is lost, retain the cached
         * view disabled until the hub supplies a complete fresh snapshot. */
        s_error = "update outside complete snapshot";
        if (s_open) goto invalid;
        char id[33]; orb_home_state_t state = {0};
        const cJSON *reachable = item(params, "reachable"), *values = item(params, "state");
        s_error = "invalid update fields";
        if (!alias(item(params, "id"), id) || (values ? !read_state(values, &state) : !reachable) ||
            (reachable && !cJSON_IsBool(reachable))) goto invalid;
        s_error = "unknown accessory or unavailable model";
        if (!orb_home_update(id, &state, reachable != NULL, cJSON_IsTrue(reachable), 0)) goto invalid;
        return LINK_V11_APPLIED;
    }
    bool reset, more, available;
    s_error = "missing snapshot arrays or flags";
    if (!read_bool(params, "reset", &reset) || !read_bool(params, "more", &more) ||
        !read_bool(params, "available", &available)) goto invalid;
    s_error = "snapshot chunk sequence or availability";
    if ((!reset && !s_open) || (!reset && available != s_available)) goto invalid;
    if (!s_chunk) {
#ifdef ESP_PLATFORM
        s_chunk = heap_caps_calloc(1, sizeof(*s_chunk), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        s_chunk = calloc(1, sizeof(*s_chunk));
#endif
        s_error = "snapshot allocation failed";
        if (!s_chunk) goto invalid;
    }
    memset(s_chunk, 0, sizeof(*s_chunk));
    s_error = "invalid snapshot fields or array limits";
    if (!read_arrays(params)) goto invalid;
    s_error = "snapshot model limits, duplicate ID or room reference";
    if (!orb_home_snapshot_chunk_available(s_chunk->rooms, s_chunk->room_count,
        s_chunk->accessories, s_chunk->accessory_count, s_chunk->scenes, s_chunk->scene_count,
        available, reset, more)) goto invalid;
    s_available = available; s_open = more; return LINK_V11_APPLIED;
invalid:
    s_open = false; orb_home_snapshot_discard(); orb_home_set_connection(true, false);
    return LINK_V11_INVALID;
}
const char *link_home_request_method(const orb_home_request_t *request)
{
    if (!request) return NULL;
    return request->kind == ORB_HOME_REQUEST_SET ? "home.set" : request->kind == ORB_HOME_REQUEST_TOGGLE ? "home.toggle" : request->kind == ORB_HOME_REQUEST_SCENE ? "home.scene" : NULL;
}
cJSON *link_home_request_params(const orb_home_request_t *request)
{
    if (!link_home_request_method(request) || !request->id[0] ||
        strnlen(request->id, sizeof(request->id)) > ORB_HOME_ID_MAX) return NULL;
    for (const unsigned char *p = (const unsigned char *)request->id; *p; p++) if (*p < 0x20 || *p > 0x7e) return NULL;
    cJSON *o = cJSON_CreateObject();
    if (!o || !cJSON_AddStringToObject(o, "id", request->id)) { cJSON_Delete(o); return NULL; }
    if (request->kind != ORB_HOME_REQUEST_SET) return o;
    if (!request->values.present || (request->values.present & ~ORB_HOME_WRITABLE)) goto fail;
    cJSON *values = cJSON_AddObjectToObject(o, "values");
    if (!values) goto fail;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (!(request->values.present & fields[i].bit)) continue;
        const char *src = (const char *)&request->values + fields[i].offset;
        if (!fields[i].boolean && (!isfinite(*(const float *)src) ||
            *(const float *)src < fields[i].min || *(const float *)src > fields[i].max)) goto fail;
        cJSON *v = fields[i].boolean ? cJSON_CreateBool(*(const bool *)src) : cJSON_CreateNumber(*(const float *)src);
        if (!v || !cJSON_AddItemToObject(values, fields[i].name, v)) { cJSON_Delete(v); goto fail; }
    }
    if (request->values.present & ORB_HOME_MODE) {
        if (request->values.mode < ORB_HOME_MODE_OFF || request->values.mode > ORB_HOME_MODE_AUTO ||
            !cJSON_AddNumberToObject(values, "mode", request->values.mode)) goto fail;
    }
    return o;
fail:
    cJSON_Delete(o); return NULL;
}
bool link_home_read_reply(const cJSON *result, const orb_home_request_t *request, orb_home_state_t *accepted)
{
    s_error = "invalid reply fields or ID";
    if (!link_home_request_method(request) || !accepted || !object_ok(result)) return false;
    char id[33];
    if (!alias(item(result, "id"), id) || strcmp(id, request->id)) return false;
    memset(accepted, 0, sizeof(*accepted));
    s_error = "invalid accepted state";
    if (request->kind == ORB_HOME_REQUEST_SCENE) return cJSON_IsTrue(item(result, "accepted"));
    /* A completed write may report no readable characteristics. Never invent
     * acceptance by echoing the requested values into the confirmed state. */
    return read_state(item(result, "state"), accepted);
}
