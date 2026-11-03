#include "agents_internal.h"

int agents_mono_command(lv_obj_t *chip, const char *command)
{
    /* HIG §4: use the signed Inter face with its normal tracking. Artificial
     * fixed glyph cells separated narrow letters and obscured command syntax. */
    lv_obj_t *text = lv_label_create(chip);
    orb_text_apply(text, ORB_TEXT_FOOTNOTE2, ORB_TEXT_EMPHASIZED, 0);
    lv_obj_set_style_text_color(text, ORB_LABEL, 0);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(text, 0, 0);
    lv_obj_set_width(text, 278);
    lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_WRAP);
    orb_kit_label_set_command(text, command);
    lv_obj_set_pos(text, 16, 14);
    lv_obj_update_layout(text);
    return LV_MAX(60, 28 + lv_obj_get_height(text));
}

void agents_nested_rows_scroll(lv_event_t *event)
{
    /* Forward travel through the kit's allocation-free scroll path. A direct
     * update can prepare sources inside an outer viewport's released fling. */
    if (lv_event_get_target_obj(event) == lv_event_get_current_target_obj(event))
        lv_obj_send_event(lv_event_get_user_data(event), LV_EVENT_SCROLL, NULL);
}

/* Choice rows share the card's reading viewport. Grant scopes are risky:
 * neutral grey with one unlock symbol, never provider-tinted (review #3). */
int agents_options(agents_state_t *w, lv_obj_t *area, const orb_card_t *c,
                          bool grants, int y, int skip, int second_skip)
{
    lv_obj_t *choices = agents_plain(area);
    unsigned count = c->options_count - (skip >= 0) - (second_skip >= 0);
    lv_obj_set_pos(choices, 0, y + 12);
    lv_obj_set_size(choices, 466, count * 84);
    const orb_session_t *session = agents_target(w, c->session);
    for (unsigned i = 0; i < c->options_count; i++) {
        if ((int)i == skip || (int)i == second_skip) continue;
        orb_kit_row_config_t config = {.title = c->options[i].label,
            .glyph = grants ? ORB_ICON_LOCK_OPEN : ORB_ICON_CHAT,
            .accent = grants || !session ? ORB_KIT_GREY : agents_colour(session->agent)};
        lv_obj_t *row = orb_kit_row_create(choices, &config);
        lv_obj_set_user_data(row, (void *)(intptr_t)(ACT_OPTION + i));
        lv_obj_set_event_bubble(row, true);
        lv_obj_add_event_cb(row, agents_clicked, LV_EVENT_CLICKED, w);
        if (!agents_ready(w)) orb_kit_row_set_enabled(row, false);
    }
    orb_kit_list_apply(choices);
    lv_obj_add_event_cb(area, agents_nested_rows_scroll, LV_EVENT_SCROLL, choices);
    /* Shared AX rows can grow beyond their minimum cadence. Keep the
     * following context after the resolved row group, never over its ink. */
    lv_obj_update_layout(choices);
    return lv_obj_get_y(choices) + lv_obj_get_height(choices);
}

/* Review #28: a selected card that left the model uses the status template,
 * titled by its source rather than a guessed card kind. Final states end on Done. */
void agents_card_gone(agents_state_t *w, const orb_notification_record_t *recent)
{
    agents_chrome_clear(w);
    if (recent && recent->source[0])
        w->heading = orb_kit_rim_title_create(w->page, recent->source, ORB_KIT_SECONDARY);
    orb_card_gone_t gone = orb_card_gone(w->model, w->card, agents_ready(w)); /* Review #14: Expired is final. */
    bool final = !strcmp(gone.title, "Expired") || !strcmp(gone.title, "Canceled");
    agents_status(w, gone.glyph, gone.title, gone.detail, final);
    if (final) agents_capsule(w, "Done", ACT_RESULT_DONE, 233, 380, false, ORB_KIT_GREY);
}

void agents_card_result(agents_state_t *w, const orb_card_t *c, const orb_notification_record_t *receipt)
{
    orb_answer_result_t none = {0};
    bool resolved = receipt && receipt->resolved, queued = receipt && receipt->queued;
    orb_result_copy_t copy = orb_answer_result(queued, resolved, resolved ? &receipt->result : &none);
    char identity[ORB_MODEL_CARD_ID_MAX + 12];
    snprintf(identity, sizeof(identity), "result:%s", c ? c->id : w->card);
    /* Try again only where the live card can still take a fresh answer. */
    agents_result(w, &copy, receipt ? receipt->decision : NULL,
                  resolved ? receipt->result.answer.text : NULL, ORB_SHELL_CARD, identity,
                  agents_ready(w));
}

/* HIG §§7/9: the exact request stays above equal-status neutral Deny/Allow
 * capsules with no default, tint, timer or synthetic grant. Extra grant
 * scopes follow the command as neutral rows in the same reading flow. */
lv_obj_t *agents_approval_review(agents_state_t *w, const orb_card_t *c, int deny, int allow)
{
    unsigned extras = c->options_count - (deny >= 0) - (allow >= 0);
    lv_obj_t *denial = deny >= 0 ? agents_capsule(w, "Deny", ACT_OPTION + deny, 233, 380, false, ORB_KIT_GREY) : NULL;
    lv_obj_t *permission = allow >= 0 ? agents_capsule(w, c->options[allow].label, ACT_OPTION + allow,
                                                       233, 380, false, ORB_KIT_GREY) : NULL;
    int top = denial && permission ? agents_pair(denial, permission) : 380 - ORB_KIT_SECONDARY_H / 2;
    int reading_top = agents_ready(w) ? 112 : 148;
    lv_obj_t *area = agents_reading(w, extras ? 0 : 60, reading_top,
                                   extras ? 466 : 346, top - 12 - reading_top);
    lv_obj_t *content = area;
    if (extras) {
        content = agents_plain(area); lv_obj_set_width(content, 346); lv_obj_set_pos(content, 60, 0);
    }
    /* Risk context is reviewable in full, including a long title. */
    int y = agents_orb_kit_paragraph(content, c->title, ORB_TEXT_HEADLINE, true, 0, ORB_LABEL, true) + 12;
    lv_obj_t *chip = agents_plain(content);
    lv_obj_set_width(chip, 310); lv_obj_set_pos(chip, 18, y);
    /* The command is plain content, never a bespoke action capsule. */
    int h = agents_mono_command(chip, c->body[0] ? c->body : c->detail);
    lv_obj_set_height(chip, h); y += h + 10;
    if (c->body[0] && c->detail[0])
        y = agents_orb_kit_paragraph(content, c->detail, ORB_TEXT_FOOTNOTE2, false, y, ORB_KIT_SECONDARY, true);
    if (c->reply && agents_ready(w))
        y = agents_orb_kit_paragraph(content, "Hold BOOT to talk", ORB_TEXT_FOOTNOTE2, false, y + 8, ORB_KIT_SECONDARY, true);
    if (extras) {
        lv_obj_set_height(content, y);
        agents_options(w, area, c, true, y, deny, allow);
    }
    orb_kit_scroll_arc_attach(area);
    agents_reading_restore(w, area, ORB_SHELL_CARD, c->id);
    return denial ? denial : permission;
}

/* Review #26: a question leads with its choices, right under the title, so
 * the first one is visible at rest at every text size; its context follows.
 * The cancel role is Dismiss (glossary §3.4: Deny is for approvals). */
lv_obj_t *agents_question_review(agents_state_t *w, const orb_card_t *c, int cancel)
{
    bool message = c->kind == ORB_CARD_MESSAGE || c->kind == ORB_CARD_ATTENTION;
    bool fixed_cancel = cancel >= 0 && !(message && c->reply);
    lv_obj_t *bottom = NULL;
    if (fixed_cancel) bottom = agents_capsule(w, "Dismiss", ACT_OPTION + cancel, 233, 380, false, ORB_KIT_GREY);
    if (message && c->reply) bottom = agents_capsule(w, "Reply", ACT_REPLY, 233, 380, false, ORB_KIT_GREY);
    lv_obj_t *area = agents_reading(w, 0, 140, 466, bottom ? 190 : 266);
    lv_obj_t *question = agents_plain(area);
    lv_obj_set_pos(question, 60, 0); lv_obj_set_width(question, 346);
    int y = agents_orb_kit_paragraph(question, c->title, ORB_TEXT_HEADLINE, true, 0, ORB_LABEL, true);
    /* A long question must not consume the first choice's reading lane.
     * Keep its complete title with the context below the options. */
    bool long_title = y > 96;
    if (long_title) {
        lv_obj_clean(question);
        y = agents_orb_kit_paragraph(question, "Choose an option", ORB_TEXT_HEADLINE, true, 0, ORB_LABEL, true);
    }
    lv_obj_set_height(question, y);
    y = agents_options(w, area, c, false, y, fixed_cancel ? cancel : -1, -1) + 4;
    if (long_title || c->body[0] || c->detail[0]) {
        lv_obj_t *context = agents_plain(area);
        lv_obj_set_pos(context, 60, y); lv_obj_set_width(context, 346);
        int z = 0;
        if (long_title) z = agents_orb_kit_paragraph(context, c->title, ORB_TEXT_HEADLINE, true, z, ORB_LABEL, true);
        if (c->body[0]) z = agents_orb_kit_paragraph(context, c->body, ORB_TEXT_CAPTION1, false, z, ORB_LABEL, true);
        if (c->detail[0])
            z = agents_orb_kit_paragraph(context, c->detail, ORB_TEXT_FOOTNOTE2, false, z + 4, ORB_KIT_SECONDARY, true);
        lv_obj_set_height(context, z);
    }
    orb_kit_scroll_arc_attach(area);
    agents_reading_restore(w, area, ORB_SHELL_CARD, c->id);
    return bottom;
}

bool agents_approval(agents_state_t *w, const orb_card_t *c)
{
    /* Stack owns its own pager. Detailed approval/question review is wholly
     * kit-backed here, including resolved receipts; it never falls through to
     * the old card controls in the workspace. */

    /* Home owns its event presentation. Agent/MCP messages use the same kit
     * reader and explicit option boundary as questions, including Dismiss. */

    const orb_notification_record_t *receipt = w->receipt;
    bool has_receipt = receipt && (receipt->queued || receipt->resolved);
    /* card.cancel retires the live prompt after a successful answer too.
     * Its retained authoritative receipt must outrank the missing-card state. */
    if (!c && !has_receipt && w->surface == ORB_SHELL_CARD) { agents_card_gone(w, receipt); return true; }
    if (!c && !has_receipt) return false;
    agents_chrome_clear(w);
    const orb_session_t *session = c ? agents_target(w, c->session) : NULL;
    lv_color_t colour = session ? agents_colour(session->agent) : ORB_KIT_GREY;
    w->heading = orb_kit_rim_title_create(w->page, session ? session->title :
                                         !c && receipt ? receipt->source :
                                         c && c->kind == ORB_CARD_APPROVAL ? "Approval" :
                                         c && c->kind == ORB_CARD_MESSAGE ? "Message" : "Question", colour);
    if (has_receipt || (c && c->answered)) { agents_card_result(w, c, receipt); return true; }
    bool approval = c->kind == ORB_CARD_APPROVAL;
    int deny = -1, allow = -1;
    for (unsigned i = 0; i < c->options_count; i++) {
        if (c->options[i].role == ORB_OPTION_CANCEL || !strcmp(c->options[i].id, "deny")) deny = i;
        else if (allow < 0) allow = i;
    }
    lv_obj_t *bottom_control = approval ? agents_approval_review(w, c, deny, allow) :
        agents_question_review(w, c, deny);
    /* Review #12: offline, the review stays readable, its controls dim and the shared caption says why. */
    if (!agents_ready(w)) orb_kit_status_caption_create(w->page, orb_agents_unavailable(w->model->online, false), 100);
    unsigned index = 0, count = 0;
    for (unsigned i = 0; i < w->model->card_count; i++) {
        const orb_card_t *pending = &w->model->cards[i];
        if (pending->answered || (pending->kind != ORB_CARD_APPROVAL && pending->kind != ORB_CARD_CHOICE)) continue;
        if (!strcmp(pending->id, c->id)) index = count;
        count++;
    }
    if (count > 1 && (approval || c->kind == ORB_CARD_CHOICE)) {
        lv_obj_t *position = orb_kit_page_dots_create(w->page, count);
        orb_kit_page_dots_set_active(position, index, colour);
        /* Dots use a full-screen presentation wrapper. Its hit target must
         * stay on their bottom chord, or it swallows Allow/Deny above it. */
        lv_obj_t *target = agents_plain(w->page);
        int width = (int)count * 14 + 38;
        lv_obj_set_size(target, width, 46);
        lv_obj_set_pos(target, (466 - width) / 2, 396);
        lv_obj_set_clickable(target, true);
        lv_obj_set_user_data(target, (void *)(intptr_t)ACT_APPROVAL_NEXT);
        lv_obj_set_event_bubble(target, true);
        orb_kit_press_apply(target);
        lv_obj_add_event_cb(target, agents_clicked, LV_EVENT_CLICKED, w);
        /* The dots' accessible hit areas never steal a Cancel edge. */
        if (bottom_control) lv_obj_move_foreground(bottom_control);
    }
    return true;
}
