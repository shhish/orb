/* HIG §§1/3/7: reserve one exclusive system notice lane. A translucent
 * overlay cannot promise a clear title/content lane across arbitrary apps. */
bool orb_shell_face_visible(const orb_shell_t *shell)
{
    if (!orb_shell_notice_allowed(shell)) return false;
    orb_shell_surface_t surface = orb_shell_surface(shell);
    return surface == ORB_SHELL_FACE || surface == ORB_SHELL_OFFLINE;
}

static void glance_label_text(lv_obj_t *obj, char *storage, size_t cap, const char *value)
{
    if (!strcmp(storage, value)) return;
    snprintf(storage, cap, "%s", value);
    orb_kit_chip_set_value(obj, storage, "100% · Battery low");
}

bool orb_shell_calm_notice_visible(const orb_shell_calm_t *c)
{
    return c && c->notice && lv_obj_is_visible(c->notice);
}

static void notice_dispose(void *context)
{
    orb_shell_calm_t *c = context;
    lv_obj_t *notice = c->notice;
    c->notice = c->notice_label = NULL;
    c->notice_text[0] = 0;
    c->notice_retiring = false;
    if (notice) lv_obj_delete(notice);
    if (c->notice_open_battery) {
        c->notice_open_battery = false;
        orb_shell_navigate(c->shell, ORB_SHELL_BATTERY);
    }
}

static void notice_retire(orb_shell_calm_t *c)
{
    if (!c->notice || c->notice_retiring) return;
    lv_obj_set_hidden(c->notice, true);
    if (!lv_indev_active()) { notice_dispose(c); return; }
    /* The current pointer dispatch owns these objects until it unwinds.
     * Release their material leases before constructing the next place. */
    c->notice_retiring = true;
    (void)lv_async_call(notice_dispose, c);
}

bool orb_shell_calm_dismiss_notice(orb_shell_calm_t *c)
{
    if (!c || !c->battery_notice || !orb_shell_calm_notice_visible(c)) return false;
    c->battery_notice = 0;
    notice_retire(c);
    return true;
}

static void notice_clicked(lv_event_t *event)
{
    orb_shell_calm_t *c = lv_event_get_user_data(event);
    bool open = lv_event_get_target_obj(event) == c->notice_label;
    /* Battery inspection and dismissal are local, reversible navigation. */
    if (orb_shell_calm_dismiss_notice(c) && open) {
        if (c->notice_retiring) c->notice_open_battery = true;
        else orb_shell_navigate(c->shell, ORB_SHELL_BATTERY);
    }
}

static void notice_gesture(lv_event_t *event)
{
    orb_shell_calm_t *c = lv_event_get_user_data(event);
    lv_indev_t *input = lv_indev_active();
    if (!input) return;
    lv_point_t point; lv_indev_get_point(input, &point);
    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        c->notice_start = point;
        c->notice_captured = c->notice_back = false;
        return;
    }
    int dx = c->notice_start.x - 233, dy = c->notice_start.y - 233;
    int mx = point.x - c->notice_start.x, my = point.y - c->notice_start.y;
    if (!c->notice_captured && mx * mx + my * my >= 12 * 12) {
        c->notice_captured = true;
        /* Interaction v3: DOWN chooses the radial edge; the first 12 px
         * choose radial versus tangential ownership for the whole contact.
         * Use displacement so slow drags do not depend on LVGL velocity. */
        bool left = dx < 0 && abs(dx) >= abs(dy) && dx * dx + dy * dy >= 193 * 193;
        c->notice_back = left && -dx * mx - dy * my > abs(dy * mx - dx * my);
    }
    if (c->notice_back && mx >= 40) orb_shell_calm_dismiss_notice(c);
}

static void notice_contact_reset(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_param(event);
    if (!target) return;
    orb_feedback_cancel(target);
    orb_kit_press_cancel(target);
}

static void notice_retire_contacts(orb_shell_calm_t *c)
{
    lv_display_t *display = lv_obj_get_display(c->root);
    for (lv_indev_t *input = lv_indev_get_next(NULL); input; input = lv_indev_get_next(input)) {
        if (lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER || lv_indev_get_display(input) != display ||
            lv_indev_get_state(input) != LV_INDEV_STATE_PRESSED) continue;
        /* INDEV_RESET supplies this pointer's actual target, even with more
         * than one pointer. Retire kit material as well as native press state. */
        lv_indev_add_event_cb(input, notice_contact_reset, LV_EVENT_INDEV_RESET, NULL);
        lv_indev_wait_release(input);
        lv_indev_reset(input, NULL);
        lv_indev_remove_event_cb_with_user_data(input, notice_contact_reset, NULL);
    }
}

static void glance_create(orb_shell_calm_t *c)
{
    notice_retire_contacts(c);
    c->notice = lv_obj_create(lv_display_get_layer_top(lv_obj_get_display(c->root)));
    bare(c->notice); lv_obj_set_size(c->notice, 466, 466);
    lv_obj_set_style_bg_color(c->notice, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(c->notice, LV_OPA_COVER, 0);
    lv_obj_set_clickable(c->notice, true);
    lv_obj_set_gesture_bubble(c->notice, false);
    lv_obj_add_event_cb(c->notice, notice_gesture, LV_EVENT_PRESSED, c);
    lv_obj_add_event_cb(c->notice, notice_gesture, LV_EVENT_PRESSING, c);
    lv_obj_add_event_cb(c->notice, notice_gesture, LV_EVENT_RELEASED, c);
    orb_kit_rim_title_create(c->notice, "Battery", ORB_KIT_GREY);
    lv_obj_t *close = orb_kit_close_create(c->notice);
    lv_obj_add_event_cb(close, notice_clicked, LV_EVENT_CLICKED, c);
    c->notice_label = orb_kit_button_secondary_create(c->notice, "Battery");
    orb_kit_chip_set_value(c->notice_label, "", "100% · Battery low");
    lv_obj_center(c->notice_label);
    lv_obj_add_event_cb(c->notice_label, notice_clicked, LV_EVENT_CLICKED, c);
    lv_obj_set_hidden(c->notice, true);
}

static void notice_present(orb_shell_calm_t *c)
{
    if (c->notice_retiring) {
        if (lv_indev_active()) return;
        lv_async_call_cancel(notice_dispose, c);
        notice_dispose(c);
    }
    orb_power_snapshot_t power = orb_power_get_snapshot(&c->controller);
    orb_shell_surface_t surface = orb_shell_surface(c->shell);
    bool battery_visible = orb_shell_face_visible(c->shell) || surface == ORB_SHELL_BATTERY ||
        surface == ORB_SHELL_CHARGING || surface == ORB_SHELL_CONTROL_CENTER;
    bool visible = c->battery_notice && !battery_visible && !c->alert &&
        orb_shell_notice_allowed(c->shell) && (!c->enabled || power.state == ORB_POWER_ACTIVE);
    if (!visible) { notice_retire(c); return; }
    if (!c->notice) glance_create(c);
    lv_obj_set_hidden(c->notice, false);
    bool critical = c->battery_notice == 2;
    char text[80];
    snprintf(text, sizeof(text), critical ? "%d%% · Battery low" : "%d%% · Battery", c->status.battery_pct);
    glance_label_text(c->notice_label, c->notice_text, sizeof(c->notice_text), text);
    orb_kit_disc_set_ink(c->notice_label, critical ? ORB_RED : ORB_LABEL);
    lv_obj_move_foreground(c->notice);
}

static void notice_update(orb_shell_calm_t *c)
{
    int pct = c->status.battery_pct;
    if (c->status.charging || c->status.usb) {
        c->battery_notice = 0;
        c->low_notified = c->critical_notified = false;
    } else if (pct >= 0) {
        /* Recovery hysteresis prevents noisy 20/21 or 5/6 readings from
         * replaying a dismissed notice. Unknown never proves recovery. */
        if (pct >= 23) { c->low_notified = false; c->battery_notice = 0; }
        if (pct >= 8) c->critical_notified = false;
        if (pct <= 5 && !c->critical_notified) {
            c->critical_notified = c->low_notified = true;
            c->battery_notice = 2;
        } else if (pct <= 20 && !c->low_notified) {
            c->low_notified = true;
            c->battery_notice = 1;
        }
    }
    notice_present(c);
}
