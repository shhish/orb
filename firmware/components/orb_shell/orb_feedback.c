#include "orb_feedback.h"
#include "orb_kit.h"
#include "orb_kit_effects.h"

/* Legacy shell adapters retain their API; the kit owns each implementation. */
void orb_feedback_press(lv_obj_t *obj, bool icon)
{
    if (icon) orb_kit_press_apply(obj);
}

void orb_feedback_cancel(lv_obj_t *obj)
{
    orb_kit_press_cancel(obj);
}

void orb_feedback_check(lv_obj_t *parent, lv_color_t color, bool deny)
{
    orb_kit_check_create(parent, color, deny);
}
