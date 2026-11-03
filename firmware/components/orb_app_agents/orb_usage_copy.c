#include "orb_usage_copy.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "orb_time.h"

void orb_usage_time(char *out, size_t cap, uint64_t at, bool day)
{
    time_t seconds = (time_t)at;
    struct tm local;
    if (!at || !localtime_r(&seconds, &local)) { snprintf(out, cap, "—"); return; }
    char clock[16]; orb_time_format_clock(clock, sizeof(clock), local.tm_hour, local.tm_min);
    static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    snprintf(out, cap, "%s%s%s", day ? days[local.tm_wday] : "", day ? " " : "", clock);
}

/* "5 hours" -> "5 hr", "30 minutes" -> "30 min"; names without a duration
 * (a model's own week, "Week") stay as the hub sent them. */
void orb_usage_window_name(char *out, size_t cap, const char *label)
{
    unsigned count;
    char unit[16];
    if (sscanf(label, "%u %15s", &count, unit) == 2 && (unit[0] == 'h' || unit[0] == 'm' || unit[0] == 'd'))
        snprintf(out, cap, "%u %s", count, unit[0] == 'h' ? "hr" : unit[0] == 'm' ? "min" : "d");
    else snprintf(out, cap, "%s", label);
}

void orb_usage_page(const orb_usage_snapshot_t *usage, unsigned p, bool ready, bool ax3, orb_usage_page_t *out)
{
    (void)ax3; /* Review #34: one wording at every text size. */
    const orb_usage_window_t *main = NULL, *week = NULL, *fallback = NULL;
    for (unsigned i = 0; i < usage->count; i++) {
        const orb_usage_window_t *win = &usage->windows[i];
        if ((unsigned)win->provider != p) continue;
        if (strstr(win->label, "week") || strstr(win->label, "Week") || strstr(win->label, "7 day")) week = win;
        else if (!strcmp(win->label, "5 hours")) main = win;
        else if (!fallback || strstr(win->label, " hours") || strstr(win->label, " minutes")) fallback = win;
    }
    /* Providers do not always report a 5-hour window: Codex has run with
     * only its weekly cap. Then the week is the number that matters, so it
     * becomes the focal value instead of the whole page reading
     * "Not available". */
    bool other_main = !main && fallback && (!week || strstr(fallback->label, " hours") || strstr(fallback->label, " minutes"));
    bool weekly_main = !main && week && !other_main;
    if (weekly_main) main = week;
    /* Protocol v1.1 labels duration windows and Claude model weeks by
     * their actual names. A changed duration or model-only grant is still
     * usable quota data; do not invent a five-hour cap or hide the value. */
    if (other_main) main = fallback;
    bool known = main && !usage->unknown[p];
    *out = (orb_usage_page_t){.main = known ? main : NULL,
        .week_ring = known && week && week != main ? week : NULL, .credits = ""};
    char time[24], name[24];
    if (known) snprintf(out->value, sizeof(out->value), "%.0f%%", main->used_percent);
    else snprintf(out->value, sizeof(out->value), "Not available");
    /* Review #34: each line names its window (glossary §1.6 units), and a
     * missing reset drops "· resets" rather than ending on a lone dash. */
    orb_usage_window_name(name, sizeof(name), weekly_main ? "Week" : known ? main->label : "");
    bool day = weekly_main || (other_main && !week);
    if (known && main->resets_at) {
        orb_usage_time(time, sizeof(time), main->resets_at, day);
        if (day) {
            snprintf(out->reset, sizeof(out->reset), "%s", name);
            snprintf(out->week_reset, sizeof(out->week_reset), "resets %s", time);
        } else snprintf(out->reset, sizeof(out->reset), "%s · resets %s", name, time);
    } else snprintf(out->reset, sizeof(out->reset), "%s", name);
    bool stale = (known && main->stale) || (week && week->stale) || !ready;
    out->stale = stale;
    if (week && week != main && !usage->unknown[p]) {
        if (week->resets_at) {
            orb_usage_time(time, sizeof(time), week->resets_at, true);
            snprintf(out->week_reset, sizeof(out->week_reset), "resets %s", time);
        }
        snprintf(out->weekly, sizeof(out->weekly), "Week %.0f%%", week->used_percent);
    }
    if (p == ORB_AGENT_CODEX && ((main && main->on_credits) || (week && week->on_credits))) out->credits = "Using credits";
}
