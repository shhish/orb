#include "voice_internal.h"

bool voice_set(lv_obj_t *obj, char *buffer, size_t capacity, const char *value)
{
    if (!obj || !strcmp(buffer, value)) return false;
    snprintf(buffer, capacity, "%s", value);
    if (lv_label_get_long_mode(obj) == LV_LABEL_LONG_MODE_DOTS)
        lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text_static(obj, buffer);
    return true;
}

/* HIG §§3/4: both compact lines and wrapped previews use the shared text
 * lane's U+2026 fitting. DOTS owns mutable text for a wrapped preview; callers
 * invoke this only when content changes, so idle updates stay allocation-free. */
void voice_fit(lv_obj_t *obj, char *buffer, size_t capacity, const char *value)
{
    if (!obj || !buffer || !capacity) return;
    lv_obj_update_layout(obj);
    /* Match the original wrapped LVGL preview fitter; idle calls never run it. */
    lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(obj, value);
    lv_obj_update_layout(obj);
    snprintf(buffer, capacity, "%s", lv_label_get_text(obj));
}
