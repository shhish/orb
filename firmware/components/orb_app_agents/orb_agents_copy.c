#include "orb_agents_copy.h"

#include <stdio.h>
#include <string.h>

#include "orb_fonts.h"
#include "orb_kit.h"
#include "orb_kit_effects.h"
#include "orb_kit_motion.h"
#include "src/misc/lv_text_private.h"

const char *orb_agents_unavailable(bool online, bool ready)
{
    return !online ? ORB_KIT_OFFLINE_CAPTION : !ready ? ORB_KIT_SYNCING_CAPTION : NULL;
}

/* Retained Agents text and compact counters share the same presentation
 * helpers in the glance, stream, voice and Dot views. */
bool agents_set(lv_obj_t *obj, char *buffer, size_t capacity, const char *value)
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
void agents_fit(lv_obj_t *obj, char *buffer, size_t capacity, const char *value, bool wrap)
{
    if (!obj || !buffer || !capacity) return;
    lv_obj_update_layout(obj);
    if (wrap) {
        /* The common LVGL fitter measures the final line against the complete
         * height budget, including explicit newlines, without clipping ink. */
        lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(obj, value);
        lv_obj_update_layout(obj);
        snprintf(buffer, capacity, "%s", lv_label_get_text(obj));
        return;
    }
    char preview[ORB_MODEL_TEXT_MAX + 32];
    orb_kit_text_truncate(value, lv_obj_get_style_text_font(obj, 0),
        lv_obj_get_style_text_letter_space(obj, 0), lv_obj_get_width(obj), preview,
        LV_MIN(sizeof(preview), capacity));
    agents_set(obj, buffer, capacity, preview);
}

void agents_line(lv_obj_t *obj, char *buffer, size_t capacity, const char *value)
{
    agents_fit(obj, buffer, capacity, value, false);
}

int agents_ink(lv_obj_t *label, const char *text)
{
    lv_point_t size;
    lv_text_get_size(&size, text, lv_obj_get_style_text_font(label, 0),
                     lv_obj_get_style_text_letter_space(label, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

/* A fixed five-character rail covers the full wire uint32 range. The full
 * stream retains exact counts; the glance uses ordinary K/M/B compact units. */
void agents_count(char *text, size_t capacity, const char *sign, uint32_t count)
{
    if (count < 1000) snprintf(text, capacity, "%s%lu", sign, (unsigned long)count);
    else if (count < 1000000) snprintf(text, capacity, "%s%luK", sign, (unsigned long)(count / 1000));
    else if (count < 1000000000) snprintf(text, capacity, "%s%luM", sign, (unsigned long)(count / 1000000));
    else snprintf(text, capacity, "%s%luB", sign, (unsigned long)(count / 1000000000));
}


const char *agents_provider(orb_agent_t agent)
{
    return agent == ORB_AGENT_CLAUDE ? "Claude" : agent == ORB_AGENT_CODEX ? "Codex" : "Dot";
}

lv_color_t agents_colour(orb_agent_t agent)
{
    return agent == ORB_AGENT_CLAUDE ? ORB_KIT_CLAUDE : agent == ORB_AGENT_CODEX ? ORB_KIT_CODEX : ORB_KIT_DOT;
}

unsigned agents_priority(const orb_session_t *session)
{
    return session->state == ORB_SESSION_WAITING ? 0 : session->state == ORB_SESSION_BUSY ? 1 : 2;
}

/* AX3 keeps two step rows so the quote stays below them, as at every size. */
unsigned agents_step_rows(void)
{
    return orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 ? 2 : 3;
}

static lv_color_t agents_step_color(orb_agent_t agent, orb_step_state_t state)
{
    return state == ORB_STEP_DONE ? ORB_GREEN :
           state == ORB_STEP_FAILED ? ORB_RED : agents_colour(agent);
}

/* Review #31: the one step-row style shared by the Agents glance and the
 * Session stream. A done or failed step is a secondary record with its green
 * or red symbol; the running step is the white, emphasized current line. */
const char *agents_step_glyph(orb_step_state_t state)
{
    return state == ORB_STEP_DONE ? ORB_ICON_CHECK : state == ORB_STEP_FAILED ? ORB_ICON_WARNING : ORB_ICON_TERMINAL;
}

int agents_step_gap(void)
{
    const lv_font_t *font = orb_icon_font_variant(ORB_TEXT_CAPTION2, ORB_TEXT_EMPHASIZED);
    int width = 0;
    for (orb_step_state_t state = ORB_STEP_RUNNING; state <= ORB_STEP_FAILED; state++) {
        lv_point_t ink;
        lv_text_get_size(&ink, agents_step_glyph(state), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        width = LV_MAX(width, ink.x);
    }
    return width + 8;
}

static void agents_step_style(lv_obj_t *icon, lv_obj_t *text, orb_agent_t agent, orb_step_state_t state)
{
    bool current = state == ORB_STEP_RUNNING;
    orb_text_apply(text, ORB_TEXT_CAPTION2, current ? ORB_TEXT_EMPHASIZED : ORB_TEXT_REGULAR, 0);
    lv_obj_set_style_text_color(text, current ? ORB_LABEL : ORB_KIT_SECONDARY, 0);
    orb_kit_live_ink_opa(text, current ? LV_OPA_COVER : ORB_KIT_SECONDARY_OPA);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_LEFT, 0);
    if (!icon) return;
    orb_icon_apply(icon, ORB_TEXT_CAPTION2, ORB_TEXT_EMPHASIZED, 0);
    lv_obj_set_style_text_color(icon, agents_step_color(agent, state), 0);
    orb_kit_live_ink_opa(icon, LV_OPA_COVER);
}

/* Both the glance and the complete stream bind this same step row. Only
 * their container and available width differ; current-step emphasis, symbol,
 * line height and text fitting must never drift between the two views. */
void agents_step_bind(lv_obj_t *icon, lv_obj_t *text, char *buffer, size_t capacity,
                              orb_agent_t agent, orb_step_state_t state, const char *value,
                              int width, bool present)
{
    bool completed = present && state == ORB_STEP_DONE && lv_label_get_text(icon)[0] &&
        strcmp(lv_label_get_text(icon), agents_step_glyph(ORB_STEP_DONE));
    const char *glyph = present ? agents_step_glyph(state) : "";
    if (strcmp(lv_label_get_text(icon), glyph)) orb_kit_motion_finish(icon);
    agents_step_style(icon, text, agent, state);
    lv_label_set_long_mode(icon, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(icon, LV_SIZE_CONTENT);
    if (strcmp(lv_label_get_text(icon), glyph)) lv_label_set_text(icon, glyph);
    if (completed) orb_kit_check_label_draw_on(icon);
    lv_obj_set_height(text, lv_obj_get_style_text_font(text, 0)->line_height);
    lv_obj_set_width(text, LV_MAX(32, width));
    lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_CLIP);
    agents_line(text, buffer, capacity, value);
}

/* HIG §7: waiting and working remain first after incremental updates. Preserve
 * the hub's snapshot order within each group: updated is metadata time, not
 * state-entry time, and must never move an already waiting prompt. */
unsigned agents_order(const orb_model_snapshot_t *model, unsigned *indices)
{
    unsigned count = 0;
    for (unsigned i = 0; i < model->session_count; i++) {
        const orb_session_t *session = &model->sessions[i];
        if (!model->prefs.sources[session->agent]) continue;
        unsigned at = count;
        while (at) {
            const orb_session_t *previous = &model->sessions[indices[at - 1]];
            if (agents_priority(previous) <= agents_priority(session)) break;
            indices[at] = indices[at - 1];
            at--;
        }
        indices[at] = i;
        count++;
    }
    return count;
}

/* One glyph per meaning and the glossary's "Couldn’t" error pattern. An
 * offline Mac is never also "Loading…"; only More and Try again act. */
orb_idle_more_copy_t orb_idle_more_copy(bool allocated, bool online, bool ready, bool pending, bool failed)
{
    if (!allocated) return (orb_idle_more_copy_t){"Couldn’t load sessions", "Not enough memory", ORB_ICON_WARNING};
    if (!online) return (orb_idle_more_copy_t){"Mac offline", "Connect your Mac", ORB_ICON_DESKTOP};
    if (!ready) return (orb_idle_more_copy_t){"Loading…", "Syncing with your Mac", ORB_ICON_HOURGLASS};
    if (pending) return (orb_idle_more_copy_t){"Loading…", "Next sessions", ORB_ICON_HOURGLASS};
    if (failed) return (orb_idle_more_copy_t){"Try again", "Couldn’t load sessions", ORB_ICON_ARROW_CLOCKWISE};
    return (orb_idle_more_copy_t){"More", "Next sessions", ORB_ICON_DOTS_THREE};
}
