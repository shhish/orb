#include "home_context.h"

#include <stddef.h>
#include <string.h>

#define U32(member) {offsetof(home_context_t, member), sizeof(uint32_t), ORB_CONTEXT_U32}
#define I32(member) {offsetof(home_context_t, member), sizeof(int32_t), ORB_CONTEXT_I32}
#define FLAG(member) {offsetof(home_context_t, member), sizeof(bool), ORB_CONTEXT_BOOL}
#define TEXT(member) {offsetof(home_context_t, member), HOME_CONTEXT_ID, ORB_CONTEXT_TEXT}
#define I32X4(member) I32(member[0]), I32(member[1]), I32(member[2]), I32(member[3])
#define ROOMS(macro, member) \
    macro(member[0]), macro(member[1]), macro(member[2]), macro(member[3]), \
    macro(member[4]), macro(member[5]), macro(member[6]), macro(member[7]), \
    macro(member[8]), macro(member[9]), macro(member[10]), macro(member[11]), \
    macro(member[12]), macro(member[13]), macro(member[14]), macro(member[15])

_Static_assert(ORB_HOME_MAX_ROOMS == 16, "the room schema names every slot");
_Static_assert(sizeof(((orb_home_view_state_t *)0)->room) == HOME_CONTEXT_ID, "stable ID width");
_Static_assert(sizeof(lv_coord_t) == sizeof(int32_t), "token coordinates are 32-bit values");

/* An excess initializer fails to compile; a missing one leaves a zero-size
 * field that registry validation rejects. */
const orb_context_field_t home_context_fields[HOME_CONTEXT_FIELD_COUNT] = {
    U32(surface), U32(offset), U32(detail_page), U32(scene_index),
    U32(detail_scene_offset),
    I32(rooms_scroll), I32(room_scroll), I32(scroll), I32(scene_scroll), I32(sensor_scroll),
    I32(detail_scroll_y[0]), I32(detail_scroll_y[1]), I32(detail_scroll_y[2]),
    I32X4(room_token), I32X4(accessory_token),
    ROOMS(I32, room_scrolls),
    FLAG(sections), FLAG(sensors), FLAG(scene_page),
    TEXT(detail_scene_id), TEXT(room), TEXT(selected),
    ROOMS(TEXT, room_ids),
};

static void area_encode(const lv_area_t *area, int32_t token[4])
{
    token[0] = area->x1; token[1] = area->y1; token[2] = area->x2; token[3] = area->y2;
}

static lv_area_t area_decode(const int32_t token[4])
{
    return (lv_area_t){token[0], token[1], token[2], token[3]};
}

void home_context_encode(const orb_home_view_state_t *view, home_context_t *context)
{
    /* Field-wise copies after one memset keep every padding byte zero, as the
     * framework's value schema requires. */
    memset(context, 0, sizeof(*context));
    context->surface = view->surface;
    context->offset = view->offset;
    context->detail_page = view->detail_page;
    context->scene_index = view->scene_index;
    context->detail_scene_offset = view->detail_scene_offset;
    context->rooms_scroll = view->rooms_scroll;
    context->room_scroll = view->room_scroll;
    context->scroll = view->scroll;
    context->scene_scroll = view->scene_scroll;
    context->sensor_scroll = view->sensor_scroll;
    memcpy(context->detail_scroll_y, view->detail_scroll_y, sizeof(context->detail_scroll_y));
    area_encode(&view->room_token, context->room_token);
    area_encode(&view->accessory_token, context->accessory_token);
    context->sections = view->sections;
    context->sensors = view->sensors;
    context->scene_page = view->scene_page;
    memcpy(context->detail_scene_id, view->detail_scene_id, HOME_CONTEXT_ID);
    memcpy(context->room, view->room, HOME_CONTEXT_ID);
    memcpy(context->selected, view->selected, HOME_CONTEXT_ID);
    for (unsigned i = 0; i < ORB_HOME_MAX_ROOMS; i++) {
        memcpy(context->room_ids[i], view->room_positions[i].id, HOME_CONTEXT_ID);
        context->room_scrolls[i] = view->room_positions[i].scroll;
    }
}

void home_context_decode(const home_context_t *context, orb_home_view_state_t *view)
{
    *view = (orb_home_view_state_t){.surface = (orb_home_page_t)context->surface,
        .offset = context->offset, .detail_page = context->detail_page,
        .scene_index = context->scene_index,
        .rooms_scroll = context->rooms_scroll, .room_scroll = context->room_scroll,
        .scroll = context->scroll, .scene_scroll = context->scene_scroll,
        .sensor_scroll = context->sensor_scroll, .detail_scene_offset = context->detail_scene_offset,
        .room_token = area_decode(context->room_token),
        .accessory_token = area_decode(context->accessory_token),
        .sections = context->sections, .sensors = context->sensors, .scene_page = context->scene_page};
    memcpy(view->detail_scroll_y, context->detail_scroll_y, sizeof(view->detail_scroll_y));
    memcpy(view->detail_scene_id, context->detail_scene_id, HOME_CONTEXT_ID);
    memcpy(view->room, context->room, HOME_CONTEXT_ID);
    memcpy(view->selected, context->selected, HOME_CONTEXT_ID);
    for (unsigned i = 0; i < ORB_HOME_MAX_ROOMS; i++) {
        memcpy(view->room_positions[i].id, context->room_ids[i], HOME_CONTEXT_ID);
        view->room_positions[i].scroll = context->room_scrolls[i];
    }
}
