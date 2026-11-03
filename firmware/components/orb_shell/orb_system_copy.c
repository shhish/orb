#include "orb_system_copy.h"


#include <stdio.h>
#include <string.h>
#include <time.h>

#include "orb_icons.h"

/* Titles stay within the AX3 status row (about 12 characters). U+2011 keeps
 * "orb‑hub" from breaking at its hyphen. */
#define HUB "orb\xE2\x80\x91hub"

bool orb_system_surface_uses_system_chrome(orb_shell_surface_t surface)
{
    switch (surface) {
    case ORB_SHELL_APPS: case ORB_SHELL_SETTINGS: case ORB_SHELL_DISPLAY:
    case ORB_SHELL_ACCESSIBILITY: case ORB_SHELL_ABOUT: case ORB_SHELL_UNITS:
    case ORB_SHELL_TEMPERATURE: case ORB_SHELL_SOUND: case ORB_SHELL_BATTERY:
    case ORB_SHELL_SESSION_SETTINGS: case ORB_SHELL_NOTIFICATIONS:
    case ORB_SHELL_NOTIFICATION_DETAIL: case ORB_SHELL_UPDATES: case ORB_SHELL_STACK:
    case ORB_SHELL_CONTROL_CENTER: return true;
    default: return false;
    }
}


orb_system_connection_copy_t orb_system_connection_copy(const orb_shell_snapshot_t *m)
{
    bool wifi = m->net == ORB_SHELL_NET_CONNECTED;
    bool unprovisioned = m->net == ORB_SHELL_NET_UNPROVISIONED || m->net == ORB_SHELL_NET_PROVISIONING;
    orb_system_connection_copy_t copy = {
        .wifi = wifi ? "On" : m->net == ORB_SHELL_NET_CONNECTING ? "Joining" : unprovisioned ? "Not set up" : "Off",
        .mac = !wifi ? "Off" : m->link == ORB_SHELL_LINK_ONLINE ? "On" :
               m->link == ORB_SHELL_LINK_PAIRING ? "Pairing" : m->link == ORB_SHELL_LINK_SEARCHING ? "Searching" :
               m->link == ORB_SHELL_LINK_CONNECTING ? "Connecting" :
               m->link == ORB_SHELL_LINK_UNAUTHORIZED ? "Not paired" : "Offline",
    };
    if (unprovisioned) { copy.status = "Set up Wi-Fi"; copy.fix = "Run " HUB " setup"; }
    else if (m->net == ORB_SHELL_NET_CONNECTING) { copy.status = "Joining Wi-Fi"; copy.fix = "Retries automatically"; }
    else if (!wifi) { copy.status = "Wi-Fi failed"; copy.fix = "Check your router"; }
    else if (m->link == ORB_SHELL_LINK_ONLINE) copy.status = NULL;
    else if (m->link == ORB_SHELL_LINK_PAIRING) { copy.status = "Pairing"; copy.fix = "Enter code on Mac"; }
    else if (m->link == ORB_SHELL_LINK_SEARCHING) { copy.status = "Finding Mac"; copy.fix = "Use Mac's Wi-Fi"; }
    else if (m->link == ORB_SHELL_LINK_CONNECTING) { copy.status = "Connecting"; copy.fix = "Retries automatically"; }
    else if (m->link == ORB_SHELL_LINK_UNAUTHORIZED) { copy.status = "Not accepted"; copy.fix = "Pair again"; }
    else { copy.status = "Mac offline"; copy.fix = "Open " HUB; }
    return copy;
}

const char *orb_system_connection_summary(const orb_shell_snapshot_t *status, bool online, bool synced)
{
    if (status->net != ORB_SHELL_NET_CONNECTED) return "Wi-Fi offline";
    if (!online || status->link != ORB_SHELL_LINK_ONLINE) return "Mac offline";
    return synced ? "Connected" : "Syncing";
}

/* Build branch/hash detail belongs in About (HIG §1: content hierarchy). */
void orb_system_software_version(char *out, size_t cap, const char *build)
{
    const char *start = build;
    if (*start == 'v') start++;
    size_t n = 0;
    unsigned dots = 0;
    while ((start[n] >= '0' && start[n] <= '9') || start[n] == '.') {
        if (start[n] == '.') dots++;
        n++;
    }
    if (dots && n && start[n - 1] != '.')
        snprintf(out, cap, "%.*s", (int)n, start);
    else snprintf(out, cap, "Development build");
}


static unsigned bars(int rssi) { return rssi >= -55 ? 3 : rssi >= -67 ? 2 : rssi >= -78 ? 1 : 0; }
bool orb_system_about_changed(const orb_shell_snapshot_t *a, const orb_shell_snapshot_t *b)
{
    return bars(a->rssi) != bars(b->rssi) || strcmp(a->ssid, b->ssid) ||
        strcmp(a->hub_name, b->hub_name) || strcmp(a->firmware_version, b->firmware_version);
}

/* Clock captions may use either hour cycle. Content decisions still need a
 * civil 0..23 hour; reading just the leading number turns evening into morning. */
bool orb_system_clock_parts(const char *clock, unsigned *hour, unsigned *minute)
{
    unsigned h, m;
    char period[24] = "", extra;
    int fields = sscanf(clock, "%u:%u %23s %c", &h, &m, period, &extra);
    if (fields < 2 || fields > 3 || m > 59) return false;
    if (fields == 3) {
        if (h < 1 || h > 12) return false;
        struct tm sample = {.tm_hour = 1};
        char am[24] = "", pm[24] = "";
        (void)strftime(am, sizeof(am), "%p", &sample);
        sample.tm_hour = 13;
        (void)strftime(pm, sizeof(pm), "%p", &sample);
        if (am[0] && !strcmp(period, am)) h %= 12;
        else if (pm[0] && !strcmp(period, pm)) h = h % 12 + 12;
        else return false;
    } else if (h > 23) return false;
    if (hour) *hour = h;
    if (minute) *minute = m;
    return true;
}


/* Legacy shell identity remains shared by launch feedback and Compass. */
orb_delight_color_t orb_system_surface_accent(orb_shell_surface_t surface)
{
    switch (surface) {
    case ORB_SHELL_AGENTS: return ORB_DELIGHT_ORANGE;
    case ORB_SHELL_DOT: return ORB_DELIGHT_INDIGO;
    case ORB_SHELL_VOICE: return ORB_DELIGHT_RED;
    case ORB_SHELL_PLAY: return ORB_DELIGHT_GREEN;
    case ORB_SHELL_UPDATES: return ORB_DELIGHT_BLUE;
    case ORB_SHELL_HOME: return ORB_DELIGHT_YELLOW;
    case ORB_SHELL_TIME: return ORB_DELIGHT_TEAL;
    case ORB_SHELL_NOTIFICATIONS: return ORB_DELIGHT_BLUE;
    case ORB_SHELL_STACK: return ORB_DELIGHT_GRAY;
    default: return ORB_DELIGHT_GRAY;
    }
}
const char *orb_system_surface_icon(orb_shell_surface_t surface)
{
    switch (surface) {
    case ORB_SHELL_AGENTS: return ORB_ICON_PROMPT;
    case ORB_SHELL_DOT: return ORB_ICON_CHAT;
    case ORB_SHELL_VOICE: return ORB_ICON_MIC;
    case ORB_SHELL_PLAY: return ORB_ICON_GAME_CONTROLLER;
    case ORB_SHELL_UPDATES: return ORB_ICON_UPLOAD;
    case ORB_SHELL_HOME: return ORB_ICON_HOUSE;
    case ORB_SHELL_TIME: return ORB_ICON_TIMER;
    case ORB_SHELL_SETTINGS: return ORB_ICON_SUN;
    default: return ORB_ICON_GEAR;
    }
}

const char *orb_system_surface_title(orb_shell_surface_t surface)
{
    static const char *s_titles[] = {"Orb", "Stack", "Apps", "Agents", "Session", "Review",
        "Notifications", "Controls", "Settings", "Display", "Accessibility", "About",
        "Dot", "Voice", "Play", "Updates", "Idle", "Sessions", "Home", "Units", "Temperature", "Sound", "Time", "Offline", "Charging", "Notification", "Compass", "Usage", "Sessions"};
    return (unsigned)surface < sizeof(s_titles) / sizeof(s_titles[0]) ? s_titles[surface] : "Orb";
}

orb_shell_surface_t orb_system_compass_destination(orb_nav_compass_selection_t selection)
{
    switch (selection) {
    case ORB_NAV_COMPASS_AGENTS: return ORB_SHELL_AGENTS;
    case ORB_NAV_COMPASS_HOME: return ORB_SHELL_HOME;
    case ORB_NAV_COMPASS_PLAY: return ORB_SHELL_PLAY;
    default: return ORB_SHELL_SETTINGS;
    }
}

unsigned orb_system_home_hour(const char *clock)
{
    unsigned hour = 12;
    (void)orb_system_clock_parts(clock, &hour, NULL);
    return hour;
}
