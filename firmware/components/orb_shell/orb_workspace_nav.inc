/* UI adapter for orb_nav. Included here so LVGL remains the only owner of
 * settled content; the portable component never knows shell widget types. */
static bool nav_capture_pixels(orb_workspace_t *w, orb_shell_surface_t surface, unsigned slot)
{
    play_sync_selection(w);
    /* Home and Face have local context outside the model epoch. Capture their
     * current source/return once; drag frames read that frozen plane. */
    bool prepared = surface != w->surface && settings_surface(surface);
    if (prepared && !settings_prepare(w, surface, true)) return false;
    if (surface == w->surface && (orb_kit_motion_settling(w->page) || w->nav_apps_preview[slot])) {
        orb_kit_motion_interrupt(w->page);
        orb_nav_compositor_invalidate(w->compositor, slot);
    }
    if (surface != ORB_SHELL_FACE && surface != ORB_SHELL_HOME &&
        (surface != ORB_SHELL_APPS || ((w->nav_back || w->nav_boundary || surface == w->surface) && !w->nav_apps_preview[slot])) &&
        !(surface == w->surface && (surface == ORB_SHELL_HOME || surface == ORB_SHELL_TIME ||
          (w->nav_kind == ORB_NAV_TRANSITION_IRIS && w->nav_target == ORB_SHELL_SESSION) ||
          w->nav.touch.down)) &&
        w->nav_cached[slot] == surface &&
        orb_nav_compositor_valid(w->compositor, slot, w->nav_revision)) return true;
    if (!orb_nav_compositor_capture_begin(w->compositor, slot, w->nav_revision)) return false;
    /* Apps arrival and final return poses must never share a cache hit. */
    w->nav_apps_preview[slot] = false;
    orb_workspace_t *fixture = NULL;
    orb_neighbour_t *neighbour = NULL;
    lv_obj_t *page = w->page;
    bool retained_face = surface == ORB_SHELL_FACE && orb_app_faces_retained(orb_app_host_current(w->faces_host));
    if (prepared) {
        page = orb_settings_host_snapshot_begin(w->settings);
    } else if (retained_face) {
        page = orb_screen_root(orb_app_host_current(w->faces_host));
        lv_obj_set_hidden(page, false);
    } else if (surface != w->surface && orb_neighbour_supported(surface)) {
        neighbour = neighbour_create(w, surface);
        if (!neighbour) return false;
        page = orb_neighbour_page(neighbour);
    } else if (surface != w->surface) {
        /* A private, short-lived tree renders a neighbour once, never during a
         * drag. It cannot focus voice, send an answer, or touch the live store. */
        fixture = workspace_create_preview(lv_obj_get_parent(w->root), w);
        if (!fixture) return false;
        lv_async_call_cancel(render_async, fixture);
        fixture->render_pending = false;
        fixture->surface = surface;
        fixture->time_open = surface == ORB_SHELL_TIME;
        fixture->time = w->time;
        if (!orb_app_host_clone(&fixture->play_host, w->play_host, true)) { orb_workspace_destroy(fixture); return false; }
        workspace_time_save(w);
        if (!orb_app_host_clone(&fixture->time_host, w->time_host, true)) { orb_workspace_destroy(fixture); return false; }
        if (!orb_app_host_clone(&fixture->home_host, w->home_host, true)) { orb_workspace_destroy(fixture); return false; }
        if (!agents_context_copy(fixture, &fixture->agents_host, w->agents_host, true)) { orb_workspace_destroy(fixture); return false; }
        fixture->status = w->status;
        fixture->silent = w->silent;
        fixture->nav_applying = true;
        fixture->contrast = w->contrast;
        orb_route_location_t place = orb_route_context_read(&w->places[surface]);
        fixture->page_index = w->face_launch && surface == ORB_SHELL_USAGE ? w->face_usage_page :
            place.valid ? place.page : 0;
        orb_apps_view_select(fixture->apps_view, place.valid ? place.focus : orb_apps_view_focus(w->apps_view));
        fixture->control_volume = w->control_volume;
        fixture->restore_place = place.valid;
        fixture->restore_scroll = place.scroll;
        orb_notification_history_clone_preview(fixture->notifications, w->notifications);
        snprintf(fixture->session, sizeof(fixture->session), "%s", w->session);
        fixture->selection = w->selection;
        fixture->has_selection = w->has_selection;
        fixture->selection_external = w->selection_external;
        if (w->idle && (surface == ORB_SHELL_IDLE_SESSIONS || surface == ORB_SHELL_SESSIONS)) {
            orb_model_idle_snapshot(w->idle);
            fixture->idle = orb_workspace_payload_alloc(sizeof(*fixture->idle));
            if (!fixture->idle) { orb_workspace_destroy(fixture); return false; }
            *fixture->idle = *w->idle;
        }
        snprintf(fixture->card, sizeof(fixture->card), "%s",
            surface == ORB_SHELL_NOTIFICATION_DETAIL && orb_notification_history_detail(w->notifications)[0] ? orb_notification_history_detail(w->notifications) : w->card);
        render_now(fixture);
        page = fixture->page;
        /* Curved row typography feeds its measured size back into layout.
         * Resolve that bounded dependency before freezing a neighbour. */
        if (fixture->list) for (unsigned pass = 0; pass < 3; pass++) {
            lv_obj_update_layout(page);
            lv_obj_send_event(fixture->list, LV_EVENT_SCROLL, NULL);
            orb_round_list_update(fixture->list);
        }
    }
    lv_draw_buf_t draw;
    bool ok = page && lv_draw_buf_init(&draw, 466, 466, LV_COLOR_FORMAT_RGB565, 932,
        orb_nav_compositor_pixels(w->compositor, slot), OPEN_PIXELS) == LV_RESULT_OK;
    if (ok) {
        lv_obj_update_layout(page);
        if (surface == ORB_SHELL_APPS)
            w->nav_apps_preview[slot] = orb_apps_arrival_prepare(neighbour ? orb_neighbour_screen(neighbour) : orb_apps_view_current(w->apps_view),
                !w->nav_back && !w->nav_boundary && surface != w->surface);
        if (surface == ORB_SHELL_HOME && !w->nav_back && (!w->home_host || w->home_fresh_entry))
            orb_app_home_arrival_snapshot(neighbour ? orb_neighbour_screen(neighbour) :
                fixture ? home_screen(fixture) : home_screen(w));
        /* Snapshot rendering bypasses the display refresh's raster preparation.
         * Without this, a cold curved row falls back to its enlarged source
         * glyphs instead of the cached scale used on the settled display. */
        lv_obj_raster_prepare(page);
        ok = orb_display_capture_snapshot(page, &draw);
    }
    if (prepared) orb_settings_host_snapshot_end(w->settings);
    if (retained_face) lv_obj_set_hidden(page, true);
    if (fixture) orb_workspace_destroy(fixture);
    orb_neighbour_destroy(neighbour);
    if (ok) ok = orb_nav_compositor_capture_complete(w->compositor, slot);
    if (!ok) orb_nav_compositor_invalidate(w->compositor, slot);
    w->nav_cached[slot] = surface;
    return ok;
}

static bool nav_capture(orb_workspace_t *w, orb_shell_surface_t surface, unsigned slot)
{
    /* Private neighbours briefly create/show/delete full-size trees. Their
     * off-screen work must not dirty the visible page after its arrival. */
    bool private = surface != w->surface;
    bool invalidating = lv_display_is_invalidation_enabled(w->display);
    if (private) lv_display_enable_invalidation(w->display, false);
    bool ok = nav_capture_pixels(w, surface, slot);
    if (private) lv_display_enable_invalidation(w->display, invalidating);
    return ok;
}

static void nav_live_position(orb_workspace_t *w, bool restore)
{
    if (!w->nav_live || !w->page) return;
    int x = w->nav_live_x, y = w->nav_live_y;
    if (!restore && !orb_ui_reduce_motion() &&
        (w->nav_kind == ORB_NAV_TRANSITION_SLIDE || w->nav_kind == ORB_NAV_TRANSITION_SHEET)) {
        float progress = orb_nav_compositor_progress(w->compositor);
        float p = LV_CLAMP(0.f, progress, 1.f);
        int sign = w->nav_direction == ORB_NAV_DIRECTION_LEFT || w->nav_direction == ORB_NAV_DIRECTION_UP ? -1 : 1;
        int incoming = (int)lrintf(progress * 466 * sign) - sign * (466 + (int)lrintf(.06f * 466 * sinf((float)M_PI * p)));
        if (w->nav_direction == ORB_NAV_DIRECTION_LEFT || w->nav_direction == ORB_NAV_DIRECTION_RIGHT) x += incoming;
        else y += incoming;
    }
    /* Move hit geometry with the cached sheet. Its zero-opacity tree never
     * draws; the compositor still copies the same immutable screen pixels. */
    lv_obj_set_style_translate_x(w->page, x, 0);
    lv_obj_set_style_translate_y(w->page, y, 0);
}

static float nav_presented_drag(const orb_workspace_t *w, float progress)
{
    return w->nav_reverse_prefix + (1 - w->nav_reverse_prefix) * progress;
}

static float nav_logical_drag(const orb_workspace_t *w, float presentation)
{
    return (presentation - w->nav_reverse_prefix) / (1 - w->nav_reverse_prefix);
}

static bool nav_destination_visible(const orb_workspace_t *w, lv_point_t point)
{
    if (orb_ui_reduce_motion()) return true;
    if (w->nav_boundary) return false;
    float p = LV_CLAMP(0.f, orb_nav_compositor_progress(w->compositor), 1.f);
    if (w->nav_kind == ORB_NAV_TRANSITION_IRIS) {
        float cx = w->nav_token.x + w->nav_token.width * .5f, cy = w->nav_token.y + w->nav_token.height * .5f;
        float far_x = fmaxf(cx, 465 - cx), far_y = fmaxf(cy, 465 - cy);
        float initial = fmaxf(w->nav_token.width, w->nav_token.height) * .5f;
        float amount = w->nav_reverse ? 1 - p : p;
        float radius = initial + (hypotf(far_x, far_y) + 1 - initial) * amount;
        bool inside = hypotf(point.x - cx, point.y - cy) <= radius;
        return w->nav_reverse ? !inside : inside;
    }
    if (w->nav_kind == ORB_NAV_TRANSITION_BACK) {
        return w->nav_direction == ORB_NAV_DIRECTION_RIGHT ? point.x < p * 466 :
            w->nav_direction == ORB_NAV_DIRECTION_LEFT ? point.x > 466 * (1 - p) :
            w->nav_direction == ORB_NAV_DIRECTION_DOWN ? point.y < p * 466 : point.y > 466 * (1 - p);
    }
    return true;
}

static void nav_build_live(orb_workspace_t *w)
{
    orb_shell_open_metrics_t receipt = w->open;
    int64_t started = open_now();
    w->nav_applying = true;
    w->nav_speculative = true;
    if (!settings_prepare(w, w->nav_target, true)) { w->nav_applying = w->nav_speculative = false; return; }
    if (w->nav_back) orb_nav_back(&w->nav, NULL);
    navigate(w, w->nav_target, !w->nav_back);
    lv_async_call_cancel(render_async, w);
    w->render_pending = false;
    render_now(w);
    w->nav_speculative = false;
    w->nav_applying = false;
    w->nav_live = true;
    w->nav_live_x = lv_obj_get_style_translate_x(w->page, 0);
    w->nav_live_y = lv_obj_get_style_translate_y(w->page, 0);
    receipt.object_us += open_now() - started;
    receipt.max_slice_us = LV_MAX(receipt.max_slice_us, open_now() - started);
    receipt.build_slices++;
    receipt.content_ready = true;
    w->open = receipt;
}

static void nav_arrived(orb_workspace_t *w, bool fresh)
{
    w->open.active = false;
    w->open.transition_end_us = open_now();
    if (w->surface == ORB_SHELL_APPS)
        orb_apps_arrival_prepare(orb_apps_view_current(w->apps_view), fresh);
    orb_kit_arrival_complete(w->root, fresh);
}

static void nav_accept(orb_workspace_t *w)
{
    w->nav_local_object = false;
    orb_home_host_departure(w->home_host, w->nav_target,
        !w->fixture && !w->nav_benchmark, &w->home_fresh_entry);
    bool presented = w->nav_presenting;
    if (w->nav_source == ORB_SHELL_TIME && w->nav_target != ORB_SHELL_TIME)
        orb_apps_request_exit();
    lv_async_call_cancel(nav_prepare_async, w);
    if (w->nav_presenting) orb_display_set_snapshot_transition(false);
    nav_live_position(w, true);
    w->nav_live = w->nav_presenting = w->nav_pending = false;
    orb_nav_compositor_cancel(w->compositor);
    if (presented && orb_nav_compositor_swap(w->compositor, 0, w->nav_to)) {
        orb_shell_surface_t previous = w->nav_cached[0];
        w->nav_cached[0] = w->nav_cached[w->nav_to];
        w->nav_cached[w->nav_to] = previous;
        bool preview = w->nav_apps_preview[0];
        w->nav_apps_preview[0] = w->nav_apps_preview[w->nav_to];
        w->nav_apps_preview[w->nav_to] = preview;
    }
    if (w->nav_overlay) { lv_obj_delete(w->nav_overlay); w->nav_overlay = NULL; }
    if (w->page) lv_obj_set_style_opa(w->page, LV_OPA_COVER, 0);
    nav_arrived(w, false);
    /* The compositor painted pixels outside LVGL’s live damage history. Its
     * final overlay deletion must repaint the complete destination, including
     * content that overlapped a source widget (HIG §§1/6, settled truth). */
    lv_obj_invalidate(w->root);
}

static void nav_cancel(orb_workspace_t *w)
{
    orb_settings_host_cancel_staged(w->settings);
    /* A clicked Home object has already committed its local tree. An
     * interruption retires its visual iris at that endpoint, without replaying
     * a shell route or losing the Home-owned local history. */
    if (w->nav_local_object) w->nav_live = false;
    w->nav_local_object = false;
    orb_app_host_rim_finish(w->home_host, false);
    nav_preview_stop(w);
    w->nav_settle_capture = false;
    if (w->nav_presenting) orb_display_set_snapshot_transition(false);
    lv_async_call_cancel(nav_prepare_async, w);
    nav_arrived(w, false);
    nav_live_position(w, true);
    w->nav_pending = w->nav_presenting = false;
    orb_nav_compositor_cancel(w->compositor);
    orb_nav_touch(&w->nav, ORB_NAV_TOUCH_CANCEL, 0, 0, lv_tick_get(), false, false);
    if (w->nav_overlay) { lv_obj_delete(w->nav_overlay); w->nav_overlay = NULL; }
    if (w->page) {
        lv_obj_set_hidden(w->page, false);
        lv_obj_set_style_opa(w->page, LV_OPA_COVER, 0);
        if (w->nav_fallback) {
            lv_obj_set_style_translate_x(w->page, w->nav_fallback_x, 0);
            lv_obj_set_style_translate_y(w->page, w->nav_fallback_y, 0);
        }
    }
    w->nav_fallback = false;
    if (w->nav_live) {
        w->nav_live = false;
        w->nav = w->nav_origin_state;
        memset(&w->nav.touch, 0, sizeof(w->nav.touch));
        w->surface = w->nav_source;
        w->time_open = w->surface == ORB_SHELL_TIME;
        w->nav_applying = true;
        render(w);
        w->nav_applying = false;
    }
    if (w->compass) { lv_obj_delete(w->compass); w->compass = NULL; }
}

static void nav_suspend(orb_workspace_t *w)
{
    if (w->nav_pending || w->nav_presenting || w->nav_fallback) {
        float progress = w->nav_presenting ? nav_logical_drag(w,
            orb_nav_compositor_progress(w->compositor)) : w->nav_last_drag;
        bool commit = !w->nav_boundary && progress >= .5f;
        bool restore_scroll = w->nav_live && !commit, backwards = w->nav_back;
        orb_shell_surface_t target = w->nav_target;
        lv_async_call_cancel(nav_prepare_async, w);
        /* Sleep retires the presentation at its nearest endpoint, without
         * letting a paused spring or queued capture survive the wake. */
        if (commit && w->nav_live) nav_accept(w);
        else {
            nav_cancel(w);
            if (commit) {
                w->nav_applying = true;
                if (backwards && settings_prepare(w, target, true)) orb_nav_back(&w->nav, NULL);
                navigate(w, target, !backwards);
                w->nav_applying = false;
            }
        }
        if (w->render_pending) {
            lv_async_call_cancel(render_async, w);
            w->render_pending = false;
            render_now(w);
        }
        if (restore_scroll && w->list) lv_obj_scroll_to_y(w->list, w->nav_source_scroll, LV_ANIM_OFF);
    }
    orb_nav_touch(&w->nav, ORB_NAV_TOUCH_CANCEL, 0, 0, lv_tick_get(), false, false);
    memset(&w->nav.touch, 0, sizeof(w->nav.touch));
    orb_nav_rotary_stop(&w->nav);
    orb_app_time_rotary_finish(time_screen(w), false);
    orb_app_host_rim_finish(w->home_host, false);
    orb_round_list_scroll_finish(nav_scroll_owner(w));
    /* Ending crown ownership can begin a row-alignment animation. Sleep
     * freezes its current presentation too; it must not resume after wake. */
    lv_obj_t *list = nav_scroll_owner(w);
    if (list) lv_obj_stop_scroll_anim(list);
    if (w->compass) { lv_obj_delete(w->compass); w->compass = NULL; }
    if (w->rotary_mark) { lv_obj_delete(w->rotary_mark); w->rotary_mark = NULL; }
    w->nav_local_back = false;
    w->nav_local_object = false;
    w->suppress_click = true;
    if (w->nav_input) {
        lv_indev_reset(w->nav_input, NULL);
        lv_indev_wait_release(w->nav_input);
        w->nav_input = NULL;
    }
    if (w->nav_timer) lv_timer_pause(w->nav_timer);
}

static void nav_prepare_async(void *ctx)
{
    orb_workspace_t *w = ctx;
    if (!w->nav_pending || !w->shown) return;
    if (w->surface != ORB_SHELL_COMPASS && w->compass) { lv_obj_delete(w->compass); w->compass = NULL; }
    /* A cold admission is paid once and is separately reported by bench_nav.
     * At rest, Face's two neighbours are prepared by nav_tick below. */
    unsigned to = w->nav_boundary ? 0 : 1;
    for (unsigned slot = 1; !w->nav_boundary && slot < ORB_NAV_SNAPSHOT_SLOTS; slot++)
        if (w->nav_cached[slot] == w->nav_target &&
            orb_nav_compositor_valid(w->compositor, slot, w->nav_revision) &&
            (w->nav_target != ORB_SHELL_APPS || !w->nav_back || !w->nav_apps_preview[slot])) to = slot;
    w->nav_to = to;
    bool target_warm = w->nav_cached[to] == w->nav_target &&
        orb_nav_compositor_valid(w->compositor, to, w->nav_revision) &&
        (w->nav_target != ORB_SHELL_APPS || !w->nav_back || !w->nav_apps_preview[to]);
    orb_nav_admission_reason_t failure = ORB_NAV_ADMISSION_NONE;
#ifdef ESP_PLATFORM
    if (!orb_display_drain()) failure = ORB_NAV_ADMISSION_DRAIN;
    else
#endif
    if (!nav_capture(w, w->surface, 0)) failure = ORB_NAV_ADMISSION_SOURCE_CAPTURE;
    else if (!nav_capture(w, w->nav_target, to)) failure = ORB_NAV_ADMISSION_TARGET_CAPTURE;
    else if (!orb_nav_compositor_begin(w->compositor, w->nav_kind, 0, to,
        w->nav_direction, &w->nav_token, w->nav_reverse, orb_ui_reduce_motion()))
        failure = ORB_NAV_ADMISSION_COMPOSITOR_BEGIN;
    if (failure != ORB_NAV_ADMISSION_NONE) {
        orb_nav_admission_fail(failure);
        bool released = w->nav_released, commit = released && w->nav_commit;
        orb_shell_surface_t target = w->nav_target;
        bool backwards = w->nav_back;
        orb_nav_recognizer_t touch = w->nav.touch;
        nav_cancel(w);
        /* A failed admission cannot abandon a captured finger. Degrade to a
         * live translation until lift, retaining the same ownership/decision. */
        if (!released) {
            w->nav.touch = touch;
            w->nav_fallback = true;
            w->open.active = true;
            lv_timer_resume(w->nav_timer);
        }
        if (commit) {
            if (backwards && settings_prepare(w, target, true)) orb_nav_back(&w->nav, NULL);
            navigate(w, target, !backwards);
        }
        return;
    }
    w->nav_pending = false;
    /* A button Back already has its destination plane. Rebuilding that tree
     * here used to postpone the first changed band by ~210ms. Reveal cached
     * pixels immediately; build its live controls at the committed endpoint.
     * Interactive sheets keep their existing early hit-target contract. */
    if (!w->nav_benchmark && !w->nav_boundary &&
        !(w->nav_released && (w->nav_back || target_warm))) {
        nav_build_live(w);
    }
    /* A warm button entry needs no object work before its first completed
     * cached frame. Its controls are materialized at the following timer pass;
     * interactive sheets still construct their hit targets at admission. */
    w->open.content_ready = true;
    w->nav_presenting = true;
    /* Rectangle-corner coverage contains a long invisible reverse prefix on
     * the round panel. Start at its proven source-identical endpoint, keeping
     * the exact visible aperture/rim and mapping finger distance continuously. */
    w->nav_reverse_prefix = orb_nav_compositor_reverse_iris_round_start(w->compositor);
    if (w->nav_kind == ORB_NAV_TRANSITION_IRIS) orb_sound_play(w->nav_back ? ORB_SOUND_CLOSE : ORB_SOUND_OPEN);
    w->opening = false;
    w->open.active = true;
    lv_timer_pause(w->open_timer);
    lv_timer_pause(w->delight_timer);
    lv_obj_set_style_opa(w->page, LV_OPA_TRANSP, 0);
    w->nav_overlay = orb_place_layer_create(w->root);
    lv_obj_set_size(w->nav_overlay, 466, 466);
    lv_obj_set_style_bg_color(w->nav_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(w->nav_overlay, LV_OPA_COVER, 0);
    /* Only this opaque snapshot plane has no live blur dependency. Clear the
     * certification before any return to the live tree (HIG §2.2). */
    orb_display_set_snapshot_transition(true);
    orb_nav_compositor_drag(w->compositor, nav_presented_drag(w, w->nav_last_drag));
    nav_live_position(w, false);
    if (w->nav_released) {
        orb_nav_compositor_release(w->compositor, w->nav_commit, 0);
        /* Admission can run at the next refresh timer opportunity. Start from
         * the input's elapsed time instead of presenting an unchanged p=0
         * frame and waiting another full period (especially the 150ms fade). */
        orb_nav_compositor_tick(w->compositor, LV_MIN(lv_tick_get() - w->nav_at, 32u));
    }
    w->nav_at = lv_tick_get();
    lv_timer_resume(w->nav_timer);
    lv_obj_invalidate(w->root);
}

static bool nav_start(orb_workspace_t *w, orb_shell_surface_t destination,
                      orb_nav_transition_kind_t kind, orb_nav_direction_t direction,
                      bool backwards, const orb_nav_token_rect_t *token, bool interactive)
{
    if (!w || !w->compositor || !w->page || w->fixture ||
        (destination == w->surface && kind != ORB_NAV_TRANSITION_BOUNDARY) || orb_voice_flow_held(w->voice_flow)) {
        orb_nav_admission_fail(ORB_NAV_ADMISSION_WORKSPACE_HOLD);
        return false;
    }
    int64_t input_us = open_now();
    /* Finish focus before freezing the launch identity into frame zero. */
    if (kind == ORB_NAV_TRANSITION_IRIS && !backwards) {
        orb_kit_focus_finish(w->page);
        lv_obj_t *caption = orb_apps_view_caption(w->apps_view);
        if (caption) lv_obj_raster_prepare(caption);
    }
    nav_preview_stop(w);
    w->nav_settle_capture = false;
    if (w->nav_presenting || w->nav_pending || w->nav_fallback) nav_cancel(w);
    if (!settings_prepare(w, destination, true)) return false;
    w->nav_source = w->surface;
    workspace_time_save(w);
    w->nav_source_scroll = w->list ? lv_obj_get_scroll_y(w->list) : 0;
    w->nav_origin_state = w->nav;
    w->nav_fallback_x = lv_obj_get_style_translate_x(w->page, 0);
    w->nav_fallback_y = lv_obj_get_style_translate_y(w->page, 0);
    w->open = (orb_shell_open_metrics_t){.sequence = w->open.sequence + 1, .surface = destination,
        .cold = !(w->visited & (1u << destination)), .active = true, .input_us = input_us,
        .feedback_area = {w->launch.x - 24, w->launch.y - 24, w->launch.x + 24, w->launch.y + 24}};
    w->visited |= 1u << destination;
    w->nav_target = destination;
    w->nav_kind = kind; w->nav_direction = direction; w->nav_back = backwards;
    w->nav_boundary = kind == ORB_NAV_TRANSITION_BOUNDARY;
    w->nav_boundary_distance = 0;
    w->nav_first_presented = false;
    w->nav_reverse_prefix = 0;
    if (kind == ORB_NAV_TRANSITION_IRIS && !backwards) {
        orb_nav_compositor_invalidate(w->compositor, 1);
        orb_nav_compositor_invalidate(w->compositor, 2);
    }
    w->nav_token = token ? *token : (orb_nav_token_rect_t){197, 197, 72, 72};
    w->nav_reverse = backwards && kind == ORB_NAV_TRANSITION_IRIS;
    w->nav_pending = true;
    w->nav_released = !interactive;
    w->nav_commit = !interactive;
    w->nav_last_drag = w->nav_drag_origin = 0;
    w->suppress_click = interactive;
    w->nav_at = lv_tick_get();
    if (lv_async_call(nav_prepare_async, w) != LV_RESULT_OK) {
        orb_nav_admission_fail(ORB_NAV_ADMISSION_BUSY);
        w->nav_pending = false;
        return false;
    }
    lv_timer_resume(w->nav_timer);
    return true;
}

static bool nav_button(orb_workspace_t *w, orb_shell_surface_t surface, bool backwards)
{
    if (w->nav_applying || w->fixture) return false;
    orb_nav_transition_kind_t kind = backwards ? ORB_NAV_TRANSITION_BACK : ORB_NAV_TRANSITION_SLIDE;
    int side = w->surface == ORB_SHELL_APPS ? ORB_KIT_DISC : 72;
    orb_nav_token_rect_t token = {w->launch.x - side / 2, w->launch.y - side / 2, side, side};
    if (w->surface == ORB_SHELL_APPS && surface != ORB_SHELL_FACE) kind = ORB_NAV_TRANSITION_IRIS;
    if (surface == ORB_SHELL_STACK || surface == ORB_SHELL_NOTIFICATIONS || surface == ORB_SHELL_CONTROL_CENTER)
        kind = ORB_NAV_TRANSITION_SHEET;
    orb_nav_back_entry_t entry;
    if (backwards && orb_nav_back_peek(&w->nav, &entry) && entry.iris) {
        kind = ORB_NAV_TRANSITION_IRIS;
        token = (orb_nav_token_rect_t){entry.token.x, entry.token.y, entry.token.width, entry.token.height};
    }
    orb_nav_direction_t dir = backwards ?
        w->surface == ORB_SHELL_STACK ? ORB_NAV_DIRECTION_DOWN :
        w->surface == ORB_SHELL_NOTIFICATIONS ? ORB_NAV_DIRECTION_UP : ORB_NAV_DIRECTION_RIGHT :
        surface == ORB_SHELL_STACK ? ORB_NAV_DIRECTION_UP :
        surface == ORB_SHELL_NOTIFICATIONS ? ORB_NAV_DIRECTION_DOWN : ORB_NAV_DIRECTION_LEFT;
    return nav_start(w, surface, kind, dir, backwards, &token, false);
}

static lv_obj_t *nav_scroll_owner(orb_workspace_t *w)
{
    return time_screen(w) ? orb_app_time_scroll(time_screen(w)) :
        home_screen(w) ? orb_app_home_scroll(home_screen(w)) : w->list;
}

static lv_obj_t *nav_rotary_scroll_owner(orb_workspace_t *w)
{
    /* Stack's rim pages through hub cards and local activities. A card's
     * prose viewport keeps native touch scrolling without taking the crown. */
    return (w->surface == ORB_SHELL_STACK || w->surface == ORB_SHELL_AGENTS) ? NULL : nav_scroll_owner(w);
}

static void nav_rotary_changed(void *ctx, float value, float delta)
{
    orb_workspace_t *w = ctx;
    if (w->surface == ORB_SHELL_SOUND &&
        (orb_settings_host_page_for(w->settings, w->surface) == ORB_SETTINGS_SOUND_VOLUME_PAGE ||
         orb_settings_host_page_for(w->settings, w->surface) == ORB_SETTINGS_SOUND_START_PAGE ||
         orb_settings_host_page_for(w->settings, w->surface) == ORB_SETTINGS_SOUND_END_PAGE)) {
        orb_settings_host_sound_crown(w->settings, (int32_t)lrintf(value));
        return;
    }
    if (time_screen(w)) { orb_app_time_rotate(time_screen(w), value); return; }
    if (orb_app_home_dial(home_screen(w), NULL, NULL)) {
        /* Timer sampling runs outside LVGL's active-input dispatch. Keep the
         * original contact's provenance before a Home preview can become a
         * network write (HIG §1: responsibility). */
        if (!w->nav.touch.down || !orb_input_is_synthetic(w->nav_input) ||
            !orb_app_home_dial_writes(home_screen(w)))
            orb_app_home_rotate(home_screen(w), value);
        return;
    }
    if (orb_app_play_picker(play_screen(w))) {
        play_select(w, (int)lrintf(value) - (int)orb_app_play_selected(play_screen(w)));
        return;
    }
    if (w->surface == ORB_SHELL_CONTROL_CENTER && !w->page_index) {
        control_value_set(w, (int)lrintf(value));
        return;
    }
    if (w->surface == ORB_SHELL_AGENTS) {
        int target = (int)lrintf(value);
        agents_focus_change(w, target - (int)orb_app_agents_focus_index(agents_screen(w)));
        return;
    }
    lv_obj_t *list = nav_rotary_scroll_owner(w);
    if (list) {
        int target = (int)lrintf(value);
        if (!orb_round_list_scroll_to(list, target))
            lv_obj_scroll_to_y(list, target, LV_ANIM_OFF);
    } else if (home_screen(w)) orb_app_home_rotary(home_screen(w), delta);
    else {
        int target = (int)lrintf(value);
        change_page(w, target - (int)w->page_index);
    }
}

static void nav_rotary_tick(void *ctx)
{
    (void)ctx;
    orb_sound_play(ORB_SOUND_TICK);
}

static void nav_bind_rotary(orb_workspace_t *w)
{
    if (!w->compositor) return;
    if (orb_faces_host_rim(w->faces_host, &w->nav, w->surface)) return;
    if ((w->surface == ORB_SHELL_CONTROL_CENTER && !w->page_index) ||
        (w->surface == ORB_SHELL_SOUND && orb_settings_host_page_for(w->settings, w->surface) == ORB_SETTINGS_SOUND_VOLUME_PAGE)) {
        int percent = w->surface == ORB_SHELL_SOUND || w->control_volume ? orb_sound_volume() : w->status.brightness;
        orb_nav_rotary_owner_t owner = {.owner = w->surface, .ctx = w,
            .change = nav_rotary_changed, .tick = nav_rotary_tick,
            .minimum = 0, .maximum = 100, .value = percent,
            .detent = 1, .units_per_turn = 100};
        if (w->nav.rotary.bound && w->nav.rotary.owner.owner == owner.owner &&
            w->nav.rotary.owner.maximum == 100 && w->nav.rotary.owner.units_per_turn == 100) {
            if (!w->nav.touch.down && !w->nav.rotary.flinging)
                orb_nav_rotary_set(&w->nav, percent);
            return;
        }
        orb_nav_rotary_bind(&w->nav, &owner);
        return;
    }
    if (w->surface == ORB_SHELL_SOUND && (orb_settings_host_page_for(w->settings, w->surface) == ORB_SETTINGS_SOUND_START_PAGE || orb_settings_host_page_for(w->settings, w->surface) == ORB_SETTINGS_SOUND_END_PAGE)) {
        orb_sound_preferences_t prefs; orb_sound_preferences(&prefs);
        orb_nav_rotary_owner_t owner = {.owner = w->surface, .ctx = w,
            .change = nav_rotary_changed, .tick = nav_rotary_tick,
            .minimum = 0, .maximum = 23, .value = orb_settings_host_page_for(w->settings, w->surface) == ORB_SETTINGS_SOUND_START_PAGE ? prefs.quiet_start : prefs.quiet_end,
            .detent = 1, .units_per_turn = 24};
        orb_nav_rotary_bind(&w->nav, &owner); return;
    }
    if (time_screen(w)) {
        orb_time_rotary_t time;
        if (orb_app_time_rotary(time_screen(w), &time)) {
            orb_nav_rotary_owner_t owner = {.owner = w->surface, .ctx = w,
                .change = nav_rotary_changed, .tick = nav_rotary_tick,
                .minimum = time.minimum, .maximum = time.maximum, .value = time.value,
                .detent = time.detent, .units_per_turn = time.units_per_turn};
            orb_nav_rotary_bind(&w->nav, &owner);
            return;
        }
    }
    /* Adjustable Home accessories use a framework value lease. */
    if (orb_app_host_rim(w->home_host)) return;
    /* Embedded Home detail owns its native rim, including hold-to-unlock
     * cancellation. A system volume owner must not capture that contact. */
    if (home_screen(w) && !orb_app_home_scroll(home_screen(w))) {
        orb_nav_rotary_unbind(&w->nav, w->nav.rotary.owner.owner);
        return;
    }
    lv_obj_t *list = nav_rotary_scroll_owner(w);
    lv_obj_update_layout(w->page);
    float top = orb_app_play_picker(play_screen(w)) ? orb_app_play_selected(play_screen(w)) : list ? lv_obj_get_scroll_y(list) : w->surface == ORB_SHELL_AGENTS ? orb_app_agents_focus_index(agents_screen(w)) : w->page_index;
    float maximum = orb_app_play_picker(play_screen(w)) ? ORB_GAME_COUNT - 1 : list ? LV_MAX(0, top + lv_obj_get_scroll_bottom(list)) :
        w->surface == ORB_SHELL_AGENTS ? LV_MAX(0, (int)w->model->session_count - 1) :
        w->surface == ORB_SHELL_STACK ? LV_MAX(0, (int)stack_count(w) - 1) :
        w->surface == ORB_SHELL_CONTROL_CENTER || w->surface == ORB_SHELL_USAGE ? 1 : 0;
    if (maximum <= 0) {
        /* Design 2.0: the rim drives what is on screen, or nothing. It used to
         * fall back to system volume here, so a short list or a page with no
         * scroll silently turned the rim into a volume knob. Volume lives in
         * Control Center and Sound. */
        if (w->nav.rotary.bound && w->nav.rotary.owner.owner == w->surface)
            orb_nav_rotary_unbind(&w->nav, w->surface);
        return;
    }
    float detent = list ? 72 : 1;
    orb_nav_rotary_owner_t owner = {.owner = w->surface, .ctx = w, .change = nav_rotary_changed,
        .tick = nav_rotary_tick, .minimum = 0, .maximum = maximum, .value = top,
        .detent = detent, .units_per_turn = list ? 900 : 8};
    /* Page changes rebuild their tree during a captured crown turn. Keep its
     * angle, fractional detents and fling while the same owner remains bound. */
    if (w->nav.rotary.bound && w->nav.rotary.owner.owner == owner.owner &&
        w->nav.rotary.owner.detent == owner.detent && w->nav.rotary.owner.maximum == owner.maximum) {
        if (!w->nav.touch.down && !w->nav.rotary.flinging) orb_nav_rotary_set(&w->nav, top);
        return;
    }
    orb_nav_rotary_bind(&w->nav, &owner);
}

static void nav_compass_deleted(lv_event_t *event)
{
    orb_workspace_t *w = lv_event_get_user_data(event);
    if (lv_event_get_target_obj(event) == w->compass) w->compass = NULL;
}

static void nav_compass_show(orb_workspace_t *w)
{
    if (w->compass) return;
    if (w->surface == ORB_SHELL_COMPASS) {
        /* The four destinations are the heading; a generic page title would
         * overlap Settings at twelve o'clock (Design 1.0 Compass). */
        if (w->heading) { lv_obj_delete(w->heading); w->heading = NULL; }
    }
    w->compass_selection = ORB_NAV_COMPASS_CANCEL;
    w->compass = orb_place_layer_create(w->surface == ORB_SHELL_COMPASS ? w->page : w->root);
    /* The explicit compatibility route parents this to the page. Staged
     * navigation can delete that ancestor before nav_cancel/nav_suspend. */
    lv_obj_add_event_cb(w->compass, nav_compass_deleted, LV_EVENT_DELETE, w);
    lv_obj_set_size(w->compass, 466, 466);
    lv_obj_set_style_bg_color(w->compass, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(w->compass, LV_OPA_COVER, 0);
    const char *names[] = {"Settings", "Agents", "Play", "Home"};
    const int x[] = {233, 384, 233, 82}, y[] = {76, 220, 364, 220};
    const orb_shell_surface_t surfaces[] = {ORB_SHELL_SETTINGS, ORB_SHELL_AGENTS, ORB_SHELL_PLAY, ORB_SHELL_HOME};
    bool ax = orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3;
    /* Navigation only. The former decorative clock crossed the side discs
     * at AX3 (HIG §§3/4); leave the selector's centre clear. */
    for (unsigned i = 0; i < 4; i++) {
        int cx = ax && (i == 1 || i == 3) ? (i == 1 ? 365 : 101) : x[i];
        lv_color_t color = design_accent(w, orb_system_surface_accent(surfaces[i]));
        lv_obj_t *b = orb_kit_disc_create(w->compass, ORB_KIT_DISC_STANDARD,
            ORB_KIT_ACCENT_FILL, color, orb_system_surface_icon(surfaces[i]));
        lv_obj_set_pos(b, cx - ORB_KIT_DISC / 2, y[i] - ORB_KIT_DISC / 2);
        /* Explicit entry has no held origin and grows from the centre;
         * a held Compass uses the finger. Layout and targets stay fixed. */
        if (w->surface == ORB_SHELL_COMPASS) orb_kit_pop_in_arrival_from(b,
            w->nav.touch.down ? (int32_t)w->nav.touch.start_x : 233,
            w->nav.touch.down ? (int32_t)w->nav.touch.start_y : 233);
        else orb_kit_pop_in_from(b, (int32_t)w->nav.touch.start_x, (int32_t)w->nav.touch.start_y);
        lv_obj_set_user_data(b, (void *)(intptr_t)surfaces[i]);
        lv_obj_set_event_bubble(b, true);
        lv_obj_add_event_cb(b, clicked_event, LV_EVENT_CLICKED, w);
        orb_kit_page_label(w->compass, names[i], ORB_TEXT_FOOTNOTE2, true, cx, y[i] + ORB_KIT_DISC / 2,
              ax ? 146 : 130, 1, ORB_LABEL);
    }
    lv_obj_t *point = orb_place_layer_create(w->compass); lv_obj_set_size(point, 24, 24);
    lv_obj_set_pos(point, 221, 221); lv_obj_set_style_radius(point, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(point, ORB_LABEL, 0); lv_obj_set_style_bg_opa(point, LV_OPA_COVER, 0);
    w->suppress_click = w->surface != ORB_SHELL_COMPASS;
}

static void nav_rotary_contact(orb_workspace_t *w, orb_nav_event_t event)
{
    w->suppress_click = true;
    /* The rim owns this contact after tangential arbitration. Otherwise
     * LVGL applies its vertical delta as a second, opposite scroll while
     * the crown is moving the same cached rows. Raw point/state remain
     * available to nav_tick until physical lift. */
    if (event.type == ORB_NAV_EVENT_BEGIN && w->nav_input) {
        lv_indev_reset(w->nav_input, NULL);
        lv_indev_wait_release(w->nav_input);
        /* The arbitration sample may already have reached LVGL. Restore
         * the crown's absolute position and discard native throw state. */
        if (nav_scroll_owner(w)) nav_rotary_changed(w, w->nav.rotary.owner.value, 0);
    }
    if (event.type == ORB_NAV_EVENT_END || event.type == ORB_NAV_EVENT_CANCEL) {
        if (time_screen(w) || orb_app_home_dial(home_screen(w), NULL, NULL))
            orb_nav_rotary_stop(&w->nav);
        orb_app_host_rim_finish(w->home_host,
            event.type == ORB_NAV_EVENT_END && !orb_input_is_synthetic(w->nav_input));
        if (w->rotary_mark) { lv_obj_delete(w->rotary_mark); w->rotary_mark = NULL; }
        if (!w->nav.rotary.flinging) orb_round_list_scroll_finish(nav_scroll_owner(w));
    } else if (!faces_screen(w)) {
        if (!w->rotary_mark) w->rotary_mark = orb_kit_contact_mark_create(w->root);
        int x = (int)w->nav.touch.last_x, y = (int)w->nav.touch.last_y;
        orb_kit_contact_mark_move(w->rotary_mark, x, y);
    }
    return;
}

static void nav_drag(orb_workspace_t *w, orb_nav_event_t event)
{
    if (event.type == ORB_NAV_EVENT_BEGIN && !w->nav_presenting) {
        orb_nav_id_t target = orb_nav_neighbor(&w->nav, event.gesture);
        bool boundary = target == ORB_NAV_INVALID_ID &&
            (event.gesture == ORB_NAV_GESTURE_LEFT || event.gesture == ORB_NAV_GESTURE_RIGHT) &&
            (w->surface == ORB_SHELL_AGENTS || w->surface == ORB_SHELL_HOME);
        if (target == ORB_NAV_INVALID_ID && !boundary) return;
        if (boundary) target = w->surface;
        bool backwards = event.gesture == ORB_NAV_GESTURE_BACK ||
            (w->surface == ORB_SHELL_STACK && event.gesture == ORB_NAV_GESTURE_DOWN) ||
            (w->surface == ORB_SHELL_NOTIFICATIONS && event.gesture == ORB_NAV_GESTURE_UP);
        orb_nav_transition_kind_t kind = backwards ? ORB_NAV_TRANSITION_BACK :
            event.gesture == ORB_NAV_GESTURE_UP || event.gesture == ORB_NAV_GESTURE_DOWN ||
            event.gesture == ORB_NAV_GESTURE_CONTROL ? ORB_NAV_TRANSITION_SHEET : ORB_NAV_TRANSITION_SLIDE;
        if (boundary) kind = ORB_NAV_TRANSITION_BOUNDARY;
        orb_nav_direction_t dir = event.gesture == ORB_NAV_GESTURE_UP ? ORB_NAV_DIRECTION_UP :
            event.gesture == ORB_NAV_GESTURE_DOWN ? ORB_NAV_DIRECTION_DOWN :
            event.gesture == ORB_NAV_GESTURE_RIGHT || backwards ? ORB_NAV_DIRECTION_RIGHT : ORB_NAV_DIRECTION_LEFT;
        /* HIG §7: every dismissal reverses the layer's arrival, including
         * horizontal left-edge Back. Input distance remains on that finger's
         * own axis below; it must not change the visual departure axis. */
        if (backwards && w->surface == ORB_SHELL_STACK) dir = ORB_NAV_DIRECTION_DOWN;
        else if (backwards && w->surface == ORB_SHELL_NOTIFICATIONS) dir = ORB_NAV_DIRECTION_UP;
        orb_nav_back_entry_t entry;
        orb_nav_token_rect_t token = {197, 197, 72, 72};
        if (backwards && orb_nav_back_peek(&w->nav, &entry) && entry.iris) {
            kind = ORB_NAV_TRANSITION_IRIS;
            token = (orb_nav_token_rect_t){entry.token.x, entry.token.y, entry.token.width, entry.token.height};
        }
        nav_start(w, target, kind, dir, backwards, &token, true);
    }
    if (!w->nav_pending && !w->nav_presenting && !w->nav_fallback) return;
    float progress = (event.gesture == ORB_NAV_GESTURE_BACK ? event.dx :
        w->nav_direction == ORB_NAV_DIRECTION_LEFT ? -event.dx :
        w->nav_direction == ORB_NAV_DIRECTION_RIGHT ? event.dx :
        w->nav_direction == ORB_NAV_DIRECTION_UP ? -event.dy : event.dy) / 466.f;
    w->nav_last_drag = w->nav_drag_origin + progress;
    if (w->nav_boundary) {
        w->nav_boundary_distance = w->nav_last_drag * 466;
        w->nav_last_drag = orb_nav_rubber_band(w->nav_boundary_distance, 64) / 466.f;
    }
    if (w->nav_fallback) {
        float pixels = orb_nav_rubber_band(w->nav_last_drag * 466, 466);
        int x = w->nav_fallback_x, y = w->nav_fallback_y;
        if (w->nav_direction == ORB_NAV_DIRECTION_LEFT) x -= (int)pixels;
        else if (w->nav_direction == ORB_NAV_DIRECTION_RIGHT) x += (int)pixels;
        else if (w->nav_direction == ORB_NAV_DIRECTION_UP) y -= (int)pixels;
        else y += (int)pixels;
        lv_obj_set_style_translate_x(w->page, x, 0);
        lv_obj_set_style_translate_y(w->page, y, 0);
    }
    if (w->nav_presenting) {
        orb_nav_compositor_drag(w->compositor, nav_presented_drag(w, w->nav_last_drag));
        nav_live_position(w, false);
        lv_obj_invalidate(w->nav_overlay);
    }
    w->suppress_click = true;
    if (event.type == ORB_NAV_EVENT_END || event.type == ORB_NAV_EVENT_CANCEL) {
        w->nav_released = true;
        float velocity = (event.gesture == ORB_NAV_GESTURE_BACK ? event.vx :
            w->nav_direction == ORB_NAV_DIRECTION_LEFT ? -event.vx :
            w->nav_direction == ORB_NAV_DIRECTION_RIGHT ? event.vx :
            w->nav_direction == ORB_NAV_DIRECTION_UP ? -event.vy : event.vy) / 466.f;
        w->nav_commit = event.type == ORB_NAV_EVENT_END && !w->nav_boundary && event.commit;
        if (w->nav_boundary) {
            float divisor = 64 + .55f * fabsf(w->nav_boundary_distance);
            velocity *= .55f * 64 * 64 / (divisor * divisor);
        }
        if (w->nav_presenting) orb_nav_compositor_release(w->compositor, w->nav_commit,
            velocity * (1 - w->nav_reverse_prefix));
        if (w->nav_fallback) {
            bool commit = w->nav_commit, backwards = w->nav_back;
            orb_shell_surface_t destination = w->nav_target;
            nav_cancel(w);
            if (commit) {
                if (backwards && settings_prepare(w, destination, true)) orb_nav_back(&w->nav, NULL);
                navigate(w, destination, !backwards);
            }
        }
    }
}

static void nav_handle(orb_workspace_t *w, orb_nav_event_t event)
{
    if (event.type == ORB_NAV_EVENT_NONE) return;
    /* A button's ordinary lift has no gesture owner. It must not retarget the
     * iris that the same button just started from its CLICKED callback. */
    if (event.gesture == ORB_NAV_GESTURE_NONE) return;
    if (event.type == ORB_NAV_EVENT_BEGIN && event.gesture == ORB_NAV_GESTURE_BACK) {
        w->nav_local_back = (faces_screen(w) && orb_app_faces_mode(faces_screen(w)) != ORB_FACES_FACE) ||
            orb_app_time_local_back(time_screen(w)) ||
            orb_app_home_can_back(home_screen(w)) ||
            (settings_surface(w->surface) && orb_settings_host_page_for(w->settings, w->surface));
    }
    if (w->nav_local_back) {
        w->suppress_click = true;
        if (event.type == ORB_NAV_EVENT_END || event.type == ORB_NAV_EVENT_CANCEL) {
            w->nav_local_back = false;
            if (event.commit && event.type == ORB_NAV_EVENT_END) back(w);
        }
        return;
    }
    if (event.gesture == ORB_NAV_GESTURE_COMPASS && faces_screen(w)) {
        /* Design 2.0: holding the Face opens its gallery. Keep the recognizer's
         * existing hold arbitration but retire its former Compass route. */
        w->suppress_click = true;
        if (event.type == ORB_NAV_EVENT_BEGIN) {
            orb_app_faces_hold(faces_screen(w));
            nav_bind_rotary(w);
        }
        return;
    }
    if (faces_screen(w) && orb_app_faces_mode(faces_screen(w)) != ORB_FACES_FACE && !w->nav.touch.rim &&
        (event.gesture == ORB_NAV_GESTURE_CONTENT || event.gesture == ORB_NAV_GESTURE_LEFT ||
         event.gesture == ORB_NAV_GESTURE_RIGHT || event.gesture == ORB_NAV_GESTURE_UP ||
         event.gesture == ORB_NAV_GESTURE_DOWN)) {
        if (hypotf(event.dx, event.dy) >= 10) w->suppress_click = true;
        /* orb_faces owns the local carousel drag and its release. Swallow
         * only system place navigation; applying the delta here doubles it. */
        return;
    }
    if (event.gesture == ORB_NAV_GESTURE_COMPASS) {
        w->suppress_click = true;
        nav_compass_show(w);
        static const orb_nav_compass_selection_t positions[] = {
            ORB_NAV_COMPASS_SETTINGS, ORB_NAV_COMPASS_AGENTS,
            ORB_NAV_COMPASS_PLAY, ORB_NAV_COMPASS_HOME,
        };
        if (w->compass_selection != event.selection) {
            if (event.selection != ORB_NAV_COMPASS_CANCEL) orb_sound_play(ORB_SOUND_DETENT);
            w->compass_selection = event.selection;
        }
        for (unsigned i = 0; i < 4; i++) {
            /* Each identity disc is followed by its caption. */
            lv_obj_t *disc = lv_obj_get_child(w->compass, (int)i * 2);
            orb_kit_disc_set_focus(disc, event.selection == positions[i]);
        }
        if (event.type == ORB_NAV_EVENT_END || event.type == ORB_NAV_EVENT_CANCEL) {
            if (event.commit || w->surface != ORB_SHELL_COMPASS) {
                if (w->compass) { lv_obj_delete(w->compass); w->compass = NULL; }
            }
            if (event.commit && event.type == ORB_NAV_EVENT_END)
                nav_button(w, orb_system_compass_destination(event.selection), false);
        }
        return;
    }
    if (event.gesture == ORB_NAV_GESTURE_ROTARY) {
        nav_rotary_contact(w, event);
        return;
    }
    if (w->surface == ORB_SHELL_USAGE && !w->nav.touch.rim &&
        (event.gesture == ORB_NAV_GESTURE_CONTENT || event.gesture == ORB_NAV_GESTURE_UP ||
         event.gesture == ORB_NAV_GESTURE_DOWN)) {
        /* A non-scrolling centre has an at-top boundary in both directions.
         * Its vertical gesture still pages providers rather than leaving the
         * surface; BOOT and the inward edge gesture retain Back (HIG §7). */
        if (hypotf(event.dx, event.dy) >= 10) w->suppress_click = true;
        if (event.type == ORB_NAV_EVENT_END && fabsf(event.dy) > 70 && fabsf(event.dy) > fabsf(event.dx))
            change_page(w, event.dy < 0 ? 1 : -1);
        return;
    }
    if (orb_app_play_picker(play_screen(w)) && !w->nav.touch.rim &&
        (event.gesture == ORB_NAV_GESTURE_LEFT || event.gesture == ORB_NAV_GESTURE_RIGHT ||
         event.gesture == ORB_NAV_GESTURE_CONTENT)) {
        if (hypotf(event.dx, event.dy) >= 10) w->suppress_click = true;
        if (event.type == ORB_NAV_EVENT_END && fabsf(event.dx) > 70 && fabsf(event.dx) > fabsf(event.dy))
            play_select(w, event.dx < 0 ? 1 : -1);
        return;
    }
    if (event.gesture == ORB_NAV_GESTURE_CONTENT) {
        if (hypotf(event.dx, event.dy) >= 10) w->suppress_click = true;
        if (event.type == ORB_NAV_EVENT_END && fabsf(event.dy) > 70 && fabsf(event.dy) > fabsf(event.dx) &&
            ((w->surface == ORB_SHELL_STACK && !w->nav_stack_reader) ||
             w->surface == ORB_SHELL_CONTROL_CENTER || w->surface == ORB_SHELL_AGENTS)) {
            if (w->surface == ORB_SHELL_AGENTS) agents_focus_change(w, event.dy < 0 ? 1 : -1);
            else change_page(w, event.dy < 0 ? 1 : -1);
        }
        return;
    }
    nav_drag(w, event);
}

/* Keep the existing nav clock only until shared finite effects have a final
 * source pose; a named wait never holds up cache admission. */
static bool nav_final_pose_ready(orb_workspace_t *w)
{
    if (!orb_kit_motion_settling(w->page)) return true;
    w->nav_apps_preview[0] = true;
    orb_nav_compositor_invalidate(w->compositor, 0);
    return false;
}

static void nav_tick(lv_timer_t *timer)
{
    orb_workspace_t *w = lv_timer_get_user_data(timer);
    uint32_t now = lv_tick_get(), dt = now - w->nav_at;
    w->nav_at = now;
    if (!w->shown || !w->display_active) { lv_timer_pause(timer); return; }

    if (!w->nav_benchmark && w->nav.touch.down && w->nav_input) {
        lv_point_t point; lv_indev_get_point(w->nav_input, &point);
        orb_nav_touch_phase_t phase = lv_indev_get_state(w->nav_input) == LV_INDEV_STATE_PRESSED ?
            ORB_NAV_TOUCH_MOVE : ORB_NAV_TOUCH_UP;
        nav_handle(w, orb_nav_touch(&w->nav, phase, point.x, point.y, now, false, false));
        if (w->nav.touch.down) nav_handle(w, orb_nav_touch(&w->nav, ORB_NAV_TOUCH_TICK,
            point.x, point.y, now, false, false));
    }
    orb_nav_tick(&w->nav, now);
    if (!w->nav.touch.down && !w->nav.rotary.flinging) {
        orb_round_list_scroll_finish(nav_scroll_owner(w));
        orb_app_time_rotary_finish(time_screen(w), true);
        orb_app_host_rim_finish(w->home_host, true);
    }
    if (w->nav_presenting) {
        if (w->nav_first_presented && !w->nav_live && !w->nav_back &&
            !w->nav_benchmark && !w->nav_boundary) {
            nav_build_live(w);
            lv_obj_set_style_opa(w->page, LV_OPA_TRANSP, 0);
        }
        bool changed = orb_nav_compositor_tick(w->compositor, dt);
        nav_live_position(w, false);
        if (changed) lv_obj_invalidate(w->nav_overlay);
        if (w->nav_released && !orb_nav_compositor_active(w->compositor)) {
            bool commit = orb_nav_compositor_committed(w->compositor);
            if (commit) orb_home_host_departure(w->home_host, w->nav_target,
                !w->fixture && !w->nav_benchmark, &w->home_fresh_entry);
            if (commit && w->nav_live && w->nav_source == ORB_SHELL_TIME && w->nav_target != ORB_SHELL_TIME)
                orb_apps_request_exit();
            orb_display_set_snapshot_transition(false);
            nav_live_position(w, true);
            w->nav_presenting = false;
            if (w->nav_overlay) { lv_obj_delete(w->nav_overlay); w->nav_overlay = NULL; }
            lv_obj_set_style_opa(w->page, LV_OPA_COVER, 0);
            if (w->nav_live && !commit) {
                w->nav_live = false;
                w->nav = w->nav_origin_state;
                memset(&w->nav.touch, 0, sizeof(w->nav.touch));
                w->nav_applying = true;
                navigate(w, w->nav_source, false);
                lv_async_call_cancel(render_async, w);
                w->render_pending = false;
                render_now(w);
                if (w->list) lv_obj_scroll_to_y(w->list, w->nav_source_scroll, LV_ANIM_OFF);
                w->nav_applying = false;
            } else if (commit && !w->nav_live) {
                orb_shell_open_metrics_t receipt = w->open;
                int64_t started = open_now();
                if (w->nav_back && settings_prepare(w, w->nav_target, true)) orb_nav_back(&w->nav, NULL);
                w->nav_applying = true;
                navigate(w, w->nav_target, !w->nav_back);
                lv_async_call_cancel(render_async, w);
                w->render_pending = false;
                render_now(w);
                w->nav_applying = false;
                receipt.object_us += open_now() - started;
                receipt.max_slice_us = LV_MAX(receipt.max_slice_us, open_now() - started);
                receipt.build_slices++;
                receipt.content_ready = true;
                w->open = receipt;
            }
            w->nav_live = w->nav_local_object = false;
            /* Snapshot planes bypass live damage; repaint either endpoint before its arrival. */
            lv_obj_invalidate(w->root);
            nav_arrived(w, commit && !w->nav_back && !w->nav_boundary);
            orb_nav_compositor_cancel(w->compositor);
            if (commit && !w->nav_boundary && orb_nav_compositor_swap(w->compositor, 0, w->nav_to)) {
                orb_shell_surface_t previous = w->nav_cached[0];
                w->nav_cached[0] = w->nav_cached[w->nav_to];
                w->nav_cached[w->nav_to] = previous;
                bool preview = w->nav_apps_preview[0];
                w->nav_apps_preview[0] = w->nav_apps_preview[w->nav_to];
                w->nav_apps_preview[w->nav_to] = preview;
            }
            delight_wake(w);
        }
    }
    /* Warm neighbours are captured only while settled; no timer repaints an
     * unchanged screen, and a drag never constructs content. */
    if (!w->nav_presenting && !w->nav_pending && !w->nav.touch.down && !w->open.active &&
        !w->opening && !w->render_pending && !w->nav_benchmark && w->page &&
        !w->nav.rotary.flinging && !orb_round_list_scroll_active(nav_scroll_owner(w)) &&
        (!nav_scroll_owner(w) || !lv_obj_is_scrolling(nav_scroll_owner(w)))) {
        orb_nav_back_entry_t previous;
        if (!nav_final_pose_ready(w)) return;
        bool warm_back = orb_nav_back_peek(&w->nav, &previous) && previous.id != w->surface;
        bool back_valid = false;
        for (unsigned slot = 1; slot < ORB_NAV_SNAPSHOT_SLOTS; slot++)
            back_valid |= warm_back && w->nav_cached[slot] == previous.id && !w->nav_apps_preview[slot] &&
                orb_nav_compositor_valid(w->compositor, slot, w->nav_revision);
        if (w->nav_cached[0] != w->surface || !orb_nav_compositor_valid(w->compositor, 0, w->nav_revision))
            nav_capture(w, w->surface, 0);
        else if (warm_back && !back_valid) nav_capture(w, previous.id, 1);
        else if (w->surface == ORB_SHELL_FACE && !orb_nav_compositor_valid(w->compositor, 1, w->nav_revision))
            nav_capture(w, ORB_SHELL_AGENTS, 1);
        else if (w->surface == ORB_SHELL_FACE && !orb_nav_compositor_valid(w->compositor, 2, w->nav_revision))
            nav_capture(w, ORB_SHELL_HOME, 2);
        else if (!w->nav.rotary.flinging) lv_timer_pause(timer);
    } else if (!w->nav_presenting && !w->nav_pending && !w->nav.touch.down &&
               !w->nav.rotary.flinging && !w->open.active && !w->opening && !w->render_pending &&
               (!w->list || !lv_obj_is_scrolling(w->list)))
        lv_timer_pause(timer);
}

static void gesture(lv_event_t *e)
{
    orb_workspace_t *w = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) orb_kit_motion_interrupt(w->root);
    if (w->fixture || !w->compositor || !w->display_active) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p; lv_indev_get_point(indev, &p);
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && indev == w->nav_input) {
        float dx = p.x - w->nav.touch.start_x, dy = p.y - w->nav.touch.start_y;
        /* Object events precede CLICKED, while the navigation timer may run
         * after release. Latch movement here so a quick or out-and-back drag
         * cannot activate its original row, even without a scroll owner. */
        if (dx * dx + dy * dy >= 10 * 10) w->suppress_click = true;
    }
    /* Widget PRESS_LOST is not touch cancellation. The system recognizer
     * samples the indev until physical lift, even after leaving that widget. */
    if (code != LV_EVENT_PRESSED) return;
    w->nav_input = indev;
    w->suppress_click = false;
    if (workspace_level(w) && lv_event_get_target_obj(e) == workspace_level(w)) {
        /* The kit slider owns its visible stroke. A second relative rim
         * owner would move the value twice during one contact. */
        orb_nav_rotary_stop(&w->nav);
        return;
    }
    orb_app_time_rotary_finish(time_screen(w), false);
    orb_app_host_rim_finish(w->home_host, false);
    nav_bind_rotary(w);
    if (w->nav_presenting) {
        lv_obj_t *hit = lv_event_get_target_obj(e);
        bool revealed = nav_destination_visible(w, p);
        if (!revealed && hit != w->root && hit != w->page && lv_obj_is_clickable(hit)) {
            w->suppress_click = true;
            lv_indev_wait_release(indev);
        }
        if (w->nav_released && w->nav_live && revealed && hit != w->root && hit != w->page && lv_obj_is_clickable(hit)) {
            /* Do not make a button wait for an animation. Its target tree
             * already exists, and the current dispatch remains valid. */
            nav_accept(w);
        } else {
            /* Keep the current planes and position (HIG §6). The finger
             * takes over velocity; an unmoved lift retains the target. */
            float presentation = orb_nav_compositor_progress(w->compositor);
            w->nav_drag_origin = w->nav_boundary ? w->nav_boundary_distance / 466.f :
                nav_logical_drag(w, presentation);
        }
    }
    /* The Face gallery owns holds; other blank content stays content (Design 2.0). */
    bool blank = faces_screen(w) && orb_app_faces_mode(faces_screen(w)) == ORB_FACES_FACE;
    lv_obj_t *list = nav_scroll_owner(w);
    /* HIG §7: reader ownership starts at DOWN and survives either limit
     * and travel outside its viewport. The rim still owns card paging. */
    lv_area_t bounds = {0};
    if (list) lv_obj_get_coords(list, &bounds);
    w->nav_stack_reader = w->surface == ORB_SHELL_STACK && list &&
        lv_obj_get_scroll_top(list) + lv_obj_get_scroll_bottom(list) > 0 &&
        p.x >= bounds.x1 && p.x <= bounds.x2 && p.y >= bounds.y1 && p.y <= bounds.y2;
    bool top = !list || lv_obj_get_scroll_y(list) <= 0;
    orb_nav_event_t event = orb_nav_touch(&w->nav, ORB_NAV_TOUCH_DOWN, p.x, p.y, lv_tick_get(), blank, top);
    if (w->surface == ORB_SHELL_COMPASS) {
        /* Explicit Compass accepts the whole circle, including the rim.
         * A centre dead zone cancels; any selected quadrant commits on lift. */
        w->nav.touch.owner = ORB_NAV_GESTURE_COMPASS;
        w->nav.touch.start_x = w->nav.touch.start_y = 233;
    }
    if (w->nav_presenting) {
        orb_nav_gesture_t owner = w->nav_direction == ORB_NAV_DIRECTION_LEFT ? ORB_NAV_GESTURE_LEFT :
            w->nav_direction == ORB_NAV_DIRECTION_RIGHT ? ORB_NAV_GESTURE_RIGHT :
            w->nav_direction == ORB_NAV_DIRECTION_UP ? ORB_NAV_GESTURE_UP : ORB_NAV_GESTURE_DOWN;
        if (w->nav_back && w->nav.touch.rim &&
            w->nav.touch.radial_x < -fabsf(w->nav.touch.radial_y)) owner = ORB_NAV_GESTURE_BACK;
        else if (w->nav_target == ORB_SHELL_CONTROL_CENTER) owner = ORB_NAV_GESTURE_CONTROL;
        if (orb_nav_touch_resume(&w->nav, owner, w->nav_drag_origin, w->nav_commit)) {
            w->nav_released = false;
            orb_nav_compositor_drag(w->compositor, nav_presented_drag(w, w->nav_drag_origin));
        }
    }
    nav_handle(w, event);
    if (w->nav.touch.down || w->nav.rotary.flinging) lv_timer_resume(w->nav_timer);
}

static bool nav_bench_finish(orb_workspace_t *w)
{
    if (!w->nav_benchmark) return true;
    nav_cancel(w);
    w->nav_applying = true;
    navigate(w, w->nav_saved, false);
    lv_async_call_cancel(render_async, w);
    w->render_pending = false;
    /* navigate() deliberately resets page/detail state. Restore it before
     * building the saved surface, including the selected approval/session. */
    w->page_index = w->nav_saved_context.page_index;
    if (!agents_bench_restore(w)) return false;
    w->has_selection = w->nav_saved_context.has_selection;
    w->selection_external = w->nav_saved_context.selection_external;
    w->selection = w->nav_saved_context.selection;
    if (!orb_apps_view_copy(w->apps_view, w->apps_bench)) return false;
    w->launched_app = w->nav_saved_context.launched_app;
    w->time_open = w->nav_saved_context.time_open;
    w->time_launched = w->nav_saved_context.time_launched;
    w->launch = w->nav_saved_context.launch;
    w->icon_origin = w->nav_saved_context.icon_origin;
    memcpy(w->session, w->nav_saved_context.session, sizeof(w->session));
    memcpy(w->card, w->nav_saved_context.card, sizeof(w->card));
    memcpy(w->notice, w->nav_saved_context.notice, sizeof(w->notice));
    render_now(w);
    w->nav_applying = false;
    w->nav = w->nav_saved_state;
    if (w->list) lv_obj_scroll_to_y(w->list, w->nav_saved_scroll, LV_ANIM_OFF);
    w->nav_benchmark = false;
    return true;
}

bool orb_workspace_nav_bench(orb_workspace_t *w, const char *gesture, uint32_t elapsed_ms, bool finish)
{
    if (!w || !w->compositor) {
        orb_nav_admission_fail(ORB_NAV_ADMISSION_WORKSPACE_HOLD);
        return false;
    }
    if (finish) return nav_bench_finish(w);
    if (!gesture) return false;
    bool rotary = !strcmp(gesture, "rotary");
    if (!rotary && strcmp(gesture, "slide-left") && strcmp(gesture, "slide-right") &&
        strcmp(gesture, "stack-up") && strcmp(gesture, "shade-down") && strcmp(gesture, "back") && strcmp(gesture, "iris"))
        return false;
    if (!elapsed_ms && !w->nav_benchmark) {
        if (!w->shown || orb_voice_flow_held(w->voice_flow)) {
            orb_nav_admission_fail(ORB_NAV_ADMISSION_WORKSPACE_HOLD);
            return false;
        }
        /* Cancel an interrupted gesture before taking the stable source. A
         * saved down contact must never resume after a developer benchmark. */
        nav_cancel(w);
        w->nav_saved = w->surface;
        w->nav_saved_state = w->nav;
        w->nav_saved_scroll = w->list ? lv_obj_get_scroll_y(w->list) : 0;
        w->nav_saved_context.page_index = w->page_index;
        if (!agents_context_copy(w, &w->agents_bench, w->agents_host, true)) return false;
        if (!orb_app_host_clone(&w->play_bench, w->play_host, true)) return false;
        if (!orb_settings_host_clone(&w->settings_bench, w->settings, true)) return false;
        w->nav_saved_context.has_selection = w->has_selection;
        w->nav_saved_context.selection_external = w->selection_external;
        w->nav_saved_context.selection = w->selection;
        /* Like the original Home-only value copy, preserve Home's context only
         * when it is the benchmark source; previews of it stay read-only. */
        if (!orb_app_host_clone(&w->home_bench, home_screen(w) ? w->home_host : NULL, true)) return false;
        if (!w->apps_bench) w->apps_bench = orb_apps_view_create(true, NULL, NULL, NULL);
        if (!orb_apps_view_copy(w->apps_bench, w->apps_view)) return false;
        w->nav_saved_context.launched_app = w->launched_app;
        w->nav_saved_context.time_open = w->time_open;
        w->nav_saved_context.time_launched = w->time_launched;
        workspace_time_save(w);
        if (!orb_app_host_clone(&w->time_bench, w->time_host, true)) return false;
        w->nav_saved_context.launch = w->launch;
        w->nav_saved_context.icon_origin = w->icon_origin;
        memcpy(w->nav_saved_context.session, w->session, sizeof(w->session));
        memcpy(w->nav_saved_context.card, w->card, sizeof(w->card));
        memcpy(w->nav_saved_context.notice, w->notice, sizeof(w->notice));
        w->nav_benchmark = true;
        /* No stale Back/peer state may make this source resolve elsewhere. */
        w->nav.back_count = w->nav.layer_count = 0;
        orb_nav_set_current(&w->nav, ORB_SHELL_FACE);
        orb_shell_surface_t from = rotary ? ORB_SHELL_SETTINGS : !strcmp(gesture, "iris") ? ORB_SHELL_APPS :
            !strcmp(gesture, "back") ? ORB_SHELL_AGENTS : ORB_SHELL_FACE;
        orb_shell_surface_t to = !strcmp(gesture, "slide-right") ? ORB_SHELL_HOME :
            !strcmp(gesture, "stack-up") ? ORB_SHELL_STACK : !strcmp(gesture, "shade-down") ? ORB_SHELL_NOTIFICATIONS :
            !strcmp(gesture, "back") ? ORB_SHELL_FACE : ORB_SHELL_AGENTS;
        orb_nav_transition_kind_t kind = !strcmp(gesture, "iris") ? ORB_NAV_TRANSITION_IRIS :
            !strcmp(gesture, "stack-up") || !strcmp(gesture, "shade-down") ? ORB_NAV_TRANSITION_SHEET :
            !strcmp(gesture, "back") ? ORB_NAV_TRANSITION_BACK : ORB_NAV_TRANSITION_SLIDE;
        orb_nav_direction_t dir = !strcmp(gesture, "stack-up") ? ORB_NAV_DIRECTION_UP :
            !strcmp(gesture, "shade-down") ? ORB_NAV_DIRECTION_DOWN :
            !strcmp(gesture, "slide-right") || !strcmp(gesture, "back") ? ORB_NAV_DIRECTION_RIGHT : ORB_NAV_DIRECTION_LEFT;
        w->nav_applying = true;
        navigate(w, from, false);
        lv_async_call_cancel(render_async, w);
        w->render_pending = false;
        render_now(w);
        w->nav_applying = false;
        if (rotary) {
            nav_bind_rotary(w);
            orb_nav_touch(&w->nav, ORB_NAV_TOUCH_DOWN, 449, 233, lv_tick_get(), false, false);
            if (w->nav.rotary.bound) return true;
            orb_nav_admission_fail(ORB_NAV_ADMISSION_WORKSPACE_HOLD);
            orb_workspace_nav_bench(w, gesture, 0, true);
            return false;
        }
        orb_nav_token_rect_t token = {120, 160, 84, 84};
        if (!nav_start(w, to, kind, dir, !strcmp(gesture, "back"), &token, true)) {
            orb_workspace_nav_bench(w, gesture, 0, true);
            return false;
        }
        lv_async_call_cancel(nav_prepare_async, w);
        nav_prepare_async(w);
        if (w->nav_presenting) return true;
        orb_workspace_nav_bench(w, gesture, 0, true);
        return false;
    }
    if (!w->nav_benchmark) {
        orb_nav_admission_fail(ORB_NAV_ADMISSION_WORKSPACE_HOLD);
        return false;
    }
    /* A continuous triangular drag keeps every timed frame moving cached
     * pixels, including reversals. Cold admission and restoration are excluded. */
    float phase = (elapsed_ms % 1000u) / 1000.f;
    float progress = phase < .5f ? phase * 1.7f : (1.f - phase) * 1.7f;
    if (rotary) {
        float angle = progress * 2.f * (float)M_PI;
        orb_nav_touch(&w->nav, ORB_NAV_TOUCH_MOVE, 233 + 216 * cosf(angle),
            233 + 216 * sinf(angle), lv_tick_get(), false, false);
        return true;
    }
    if (!w->nav_presenting) {
        orb_nav_admission_fail(ORB_NAV_ADMISSION_WORKSPACE_HOLD);
        return false;
    }
    orb_nav_compositor_drag(w->compositor, progress);
    lv_obj_invalidate(w->nav_overlay);
    return true;
}
