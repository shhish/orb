/* Notification identities and receipt ages through the production shell.
 * The virtual clock and all cards are isolated synthetic data. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_fonts.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include <strings.h>
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks;
static const char *s_case = "initialization";
static lv_display_t *s_display;
static orb_shell_t *s_shell;

#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "Notifications %s at %u: %s:%d: %s\n", s_case, s_now, __FILE__, __LINE__, #value); \
    exit(1); \
} } while (0)

static uint32_t tick(void) { return s_now; }
static void settle(void)
{
    for (unsigned i = 0; i < 60; i++) {
        s_now += 20; orb_shell_poll(s_shell); lv_timer_handler(); lv_refr_now(s_display);
    }
}

static lv_obj_t *find(lv_obj_t *root, const char *text)
{
    const char *rim = orb_kit_rim_text_get(root);
    if (rim && !strcasecmp(rim, text)) return root;
    if (lv_obj_check_type(root, &lv_label_class)) {
        const char *shown = lv_label_get_text(root);
        if (!strcmp(shown, text)) return root;
        const char *dots = strstr(shown, "…");
        if (!dots) dots = strstr(shown, "...");
        if (dots && dots > shown && !strncmp(shown, text, dots - shown)) return root;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}

static void fresh(const orb_card_t *card)
{
    if (s_shell) orb_shell_destroy(s_shell);
    orb_model_reset();
    const orb_session_t sessions[] = {
        {.id="claude-test", .agent=ORB_AGENT_CLAUDE, .title="esp32s3-firmware", .state=ORB_SESSION_DONE},
        {.id="codex-test", .agent=ORB_AGENT_CODEX, .title="Checks", .state=ORB_SESSION_DONE},
        {.id="dot-test", .agent=ORB_AGENT_DOT, .title="Dot", .state=ORB_SESSION_DONE},
    };
    CHECK(orb_model_sessions_chunk(sessions, 3, true, false));
    if (card) CHECK(orb_model_upsert_card(card));
    orb_model_set_connection(true, true);
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL); CHECK(s_shell);
    orb_shell_snapshot_t status = {.net=ORB_SHELL_NET_CONNECTED, .link=ORB_SHELL_LINK_ONLINE,
        .battery_pct=80, .rssi=-54, .brightness=70};
    strcpy(status.clock_time, "10:09");
    orb_shell_update(s_shell, &status); settle();
}

static lv_obj_t *notice_row(const char *title)
{
    lv_obj_t *name = find(orb_shell_root(s_shell), title);
    if (!name) fprintf(stderr, "Missing notice title: %s\n", title);
    CHECK(name);
    return lv_obj_get_parent(name);
}
static lv_obj_t *live_row_value(lv_obj_t *row, const char *text)
{
    /* Live values float beside the prepared row. Its immutable source keeps
     * only the reserved lane, so semantic text is deliberately not a child. */
    lv_obj_t *value = find(lv_obj_get_parent(row), text);
    CHECK(value);
    orb_kit_value_info_t info;
    CHECK(orb_kit_value_info(value, &info) && info.tabular);
    return value;
}

static void check_identity(const char *title, const char *source, const char *glyph, uint32_t color)
{
    lv_obj_t *row = notice_row(title);
    lv_obj_t *disc = lv_obj_get_child(row, 0); CHECK(disc);
    CHECK(find(disc, glyph));
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(disc, 0), lv_color_hex(color)));
    CHECK(find(row, source));
}

static void source_identities(void)
{
    const struct { const char *id, *session, *source, *glyph; uint32_t color; } cases[] = {
        {"home-doorbell-source", "", "Home", ORB_ICON_HOUSE, 0xFFD60A},
        {"provider-claude", "claude-test", "Claude", ORB_ICON_PROMPT, 0xFF9F0A},
        {"provider-codex", "codex-test", "Codex", ORB_ICON_CODE, 0x0A84FF},
        {"provider-dot", "dot-test", "Dot", ORB_ICON_CHAT, 0x5E5CE6},
        {"unowned-notice", "", "System", ORB_ICON_BELL, 0x0A84FF},
        {"system-update-finished", "", "Updates", ORB_ICON_DOWNLOAD, 0x0A84FF},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        s_case = cases[i].source;
        orb_card_t card = {.kind=ORB_CARD_ATTENTION, .title="Source notice", .body="Notification details."};
        snprintf(card.id, sizeof(card.id), "%s", cases[i].id);
        snprintf(card.session, sizeof(card.session), "%s", cases[i].session);
        fresh(&card);
        orb_card_t newer = {.id="newer-card", .kind=ORB_CARD_ATTENTION, .title="Newer card"};
        CHECK(orb_model_upsert_card(&newer)); settle();
        orb_shell_navigate(s_shell, ORB_SHELL_NOTIFICATIONS); settle();
        check_identity(card.title, cases[i].source, cases[i].glyph, cases[i].color);
        CHECK(!find(notice_row(card.title), "now")); /* Snapshot age is unknown. */
        if (card.session[0]) {
            CHECK(orb_model_remove_session(card.session)); settle();
            check_identity(card.title, cases[i].source, cases[i].glyph, cases[i].color);
        }
        orb_shell_select_card(s_shell, card.id);
        orb_shell_navigate(s_shell, ORB_SHELL_NOTIFICATION_DETAIL); settle();
        CHECK(find(orb_shell_root(s_shell), cases[i].source));
        lv_obj_t *icon = find(orb_shell_root(s_shell), cases[i].glyph); CHECK(icon);
        CHECK(lv_color_eq(lv_obj_get_style_bg_color(lv_obj_get_parent(icon), 0), lv_color_hex(cases[i].color)));
    }
}

static void receipt_ages(const char *out)
{
    s_case = "receipt ages";
    orb_card_t update = {.id="system-update-fixture", .kind=ORB_CARD_ATTENTION,
        .title="Updated to 0.4.0", .body="Your Orb is up to date."};
    fresh(&update);
    orb_card_t home = {.id="home-fixture-door", .kind=ORB_CARD_ATTENTION,
        .title="Front door unlocked", .body="Front door unlocked"};
    CHECK(orb_model_upsert_card(&home)); settle();
    s_now += 7 * 60000;
    orb_card_t build = {.id="build-fixture", .session="claude-test", .kind=ORB_CARD_ATTENTION,
        .title="Build finished", .body="Firmware 0.4.1 built in 2 min 14 s. 638 tests passed. Ready to flash to the dev board."};
    CHECK(orb_model_upsert_card(&build)); settle();
    s_now += 2 * 60000;
    orb_card_t dot = {.id="dot-fixture", .session="dot-test", .kind=ORB_CARD_MESSAGE,
        .title="Dot replied", .body="Your answer is ready."};
    CHECK(orb_model_upsert_card(&dot)); settle();
    orb_shell_navigate(s_shell, ORB_SHELL_NOTIFICATIONS); settle();
    settle(); /* Capture after the transient new-card hand has expired. */
    CHECK(!find(orb_shell_root(s_shell), ORB_ICON_HAND));
    if (out) CHECK(sim_display_save_png(out));
    check_identity(build.title, "Claude", ORB_ICON_PROMPT, 0xFF9F0A);
    live_row_value(notice_row(build.title), "2 min ago");
    lv_obj_t *home_source = find(orb_shell_root(s_shell), "Home"); CHECK(home_source);
    lv_obj_t *home_row = lv_obj_get_parent(home_source);
    live_row_value(home_row, "9 min ago");
    CHECK(find(lv_obj_get_child(home_row, 0), ORB_ICON_HOUSE));
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(lv_obj_get_child(home_row, 0), 0), lv_color_hex(0xFFD60A)));
    /* Age and both title/source lines have separate lanes at Default and AX3. */
    lv_obj_t *age = lv_obj_get_child(home_row, -1); /* immutable reserved lane */
    for (unsigned i = 2; i + 1 < lv_obj_get_child_count(home_row); i++) {
        lv_obj_t *text = lv_obj_get_child(home_row, i);
        if (lv_obj_get_style_text_align(age, 0) == LV_TEXT_ALIGN_LEFT) {
            CHECK(orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3);
            CHECK(lv_obj_get_x(text) == lv_obj_get_x(age) && lv_obj_get_y(text) < lv_obj_get_y(age));
        } else CHECK(lv_obj_get_x(text) + lv_obj_get_width(text) <= lv_obj_get_x(age));
        CHECK(lv_label_get_long_mode(text) == LV_LABEL_LONG_MODE_DOTS);
    }
    CHECK(!find(notice_row(update.title), "9 min ago"));
    s_now += 60000; settle();
    live_row_value(notice_row(build.title), "3 min ago");
    live_row_value(lv_obj_get_parent(find(orb_shell_root(s_shell), "Home")), "10 min ago");
    orb_shell_select_card(s_shell, home.id);
    orb_shell_navigate(s_shell, ORB_SHELL_NOTIFICATION_DETAIL); settle();
    CHECK(find(orb_shell_root(s_shell), "10 min ago"));
    s_now += 60000; settle(); CHECK(find(orb_shell_root(s_shell), "11 min ago"));
    orb_shell_select_card(s_shell, build.id);
    orb_shell_navigate(s_shell, ORB_SHELL_NOTIFICATION_DETAIL); settle();
    CHECK(find(orb_shell_root(s_shell),
        "Firmware 0.4.1 built in 2 min 14 s. 638 tests passed. Ready to flash to the dev board."));
    /* A reconnected snapshot is not a new observed arrival. */
    orb_model_set_connection(false, false); settle();
    orb_model_set_connection(true, true);
    orb_card_t replayed = {.id="replayed-card", .kind=ORB_CARD_ATTENTION, .title="Replayed card"};
    CHECK(orb_model_upsert_card(&replayed)); settle();
    orb_shell_navigate(s_shell, ORB_SHELL_NOTIFICATIONS); settle();
    CHECK(!find(notice_row(replayed.title), "now"));
}

int main(int argc, char **argv)
{
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); CHECK(orb_model_init());
    if (argc > 1 && !strcmp(argv[1], "ax3-bold-rt")) {
        CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
        CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK);
        orb_ui_set_reduce_transparency(true);
    }
    source_identities(); receipt_ages(argc > 2 ? argv[2] : NULL);
    orb_shell_destroy(s_shell);
    printf("Notifications: %u checks passed\n", s_checks);
    return 0;
}
