/*
 * The motion sensor feeding the UI: the light on the glass and parallax
 * (docs/design.md, Parallax; hig.md §2.2, §6). Gravity is smoothed twice:
 * quickly (150 ms, as the HIG's light) for where the device points now, and
 * slowly for the pose it has rested in, so parallax shows movement and then
 * settles back to centre.
 */
#include "orb_motion_math.h"
#include "orb_ui.h"
#include "orb_ui_private.h"

#define MAX_LAYERS 8
#define TAU_FAST_S 0.15f
#define TAU_REST_S 3.0f
#define LIGHT_MAX_DEV 30.0f   /* degrees the highlight may leave 12 o'clock */

typedef struct {
    lv_obj_t *obj;
    float depth;
    int32_t max_px;
    int x, y;                 /* the shift in effect */
} layer_t;

static layer_t s_layers[MAX_LAYERS];
static int s_layer_count;
static orb_vec3_t s_fast, s_rest;
static bool s_primed;

static void on_layer_delete(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    for (int i = 0; i < s_layer_count; i++) {
        if (s_layers[i].obj == obj) {
            s_layers[i] = s_layers[--s_layer_count];
            return;
        }
    }
}

void orb_parallax_add(lv_obj_t *obj, float depth, int32_t max_px)
{
    if (s_layer_count >= MAX_LAYERS) {
        return;
    }
    s_layers[s_layer_count++] = (layer_t){ .obj = obj, .depth = depth, .max_px = max_px };
    lv_obj_add_event_cb(obj, on_layer_delete, LV_EVENT_DELETE, NULL);
}

static void move_layers(orb_vec3_t tilt)
{
    for (int i = 0; i < s_layer_count; i++) {
        layer_t *l = &s_layers[i];
        if (orb_motion_parallax(tilt, l->depth, (float)l->max_px, &l->x, &l->y)) {
#if (defined(CONFIG_ORB_DISPLAY_GLASS_MOTION_QUALITY) && CONFIG_ORB_DISPLAY_GLASS_MOTION_QUALITY) || \
    (defined(CONFIG_ORB_DISPLAY_RETAINED_SETTLED_ONLY) && CONFIG_ORB_DISPLAY_RETAINED_SETTLED_ONLY) || \
    (defined(CONFIG_ORB_DISPLAY_TE_PACING) && CONFIG_ORB_DISPLAY_TE_PACING)
            lv_draw_sw_blur_note_object_motion(l->obj);
#endif
            lv_obj_set_style_translate_x(l->obj, l->x, 0);
            lv_obj_set_style_translate_y(l->obj, l->y, 0);
        }
    }
}

void orb_ui_set_gravity(float x, float y, float z, float dt_s)
{
    orb_vec3_t g = { x, y, z };
    if (!s_primed) {
        s_fast = s_rest = g;
        s_primed = true;
    }
    orb_motion_lowpass(&s_fast, g, dt_s, TAU_FAST_S);
    orb_motion_lowpass(&s_rest, g, dt_s, TAU_REST_S);

    /* Reduce Motion: layers back to rest and the light frozen where it is. */
    if (orb_ui_reduce_motion()) {
        move_layers((orb_vec3_t){ 0 });
        return;
    }
    move_layers((orb_vec3_t){ s_fast.x - s_rest.x, s_fast.y - s_rest.y, s_fast.z - s_rest.z });

    float deg, amount;
    orb_motion_light(s_fast, LIGHT_MAX_DEV, &deg, &amount);
    orb_glass_set_light(deg, amount);
}
