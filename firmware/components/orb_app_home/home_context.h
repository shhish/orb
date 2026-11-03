#pragma once

#include "orb_home_view.h"
#include "orb_screen.h"

/* Versioned, pointer-free value record for Home's local hierarchy: page,
 * stable room/accessory/scene IDs, every scroll slot and the iris
 * tokens. Fields have explicit widths so the framework schema covers every
 * byte; restoring resolves IDs against the current model and replays nothing. */
#define HOME_CONTEXT_ID 33

typedef struct {
    uint32_t surface, offset, detail_page, scene_index, detail_scene_offset;
    int32_t rooms_scroll, room_scroll, scroll, scene_scroll, sensor_scroll;
    int32_t detail_scroll_y[3];
    int32_t room_token[4], accessory_token[4];
    int32_t room_scrolls[ORB_HOME_MAX_ROOMS];
    bool sections, sensors, scene_page;
    char detail_scene_id[HOME_CONTEXT_ID], room[HOME_CONTEXT_ID];
    char selected[HOME_CONTEXT_ID];
    char room_ids[ORB_HOME_MAX_ROOMS][HOME_CONTEXT_ID];
} home_context_t;

#define HOME_CONTEXT_BYTES (sizeof(orb_screen_context_t) + sizeof(home_context_t))

/* Five page/selection words, 32 scroll and token words, three flags and
 * 19 stable IDs: one schema field per value. */
#define HOME_CONTEXT_FIELD_COUNT 59
extern const orb_context_field_t home_context_fields[HOME_CONTEXT_FIELD_COUNT];

void home_context_encode(const orb_home_view_state_t *view, home_context_t *context);
/* Exact inverse of encode. The view still ignores a page it cannot present. */
void home_context_decode(const home_context_t *context, orb_home_view_state_t *view);
