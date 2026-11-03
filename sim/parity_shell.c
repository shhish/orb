/* Phase-zero shell catalogue. All data is process-local, bounded and synthetic.
 * These adapters use public model/routes or the same pointer input as the owner. */
#include "parity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_home.h"
#include "orb_shell_services.h"
#include "orb_sound.h"
#include "orb_time.h"
#include "orb_ui.h"
#include "orb_kit.h"

/* The evidence binary supplies fixture read-model flags; production setters
 * and persistence code are untouched. Host sound normally completes saves
 * immediately, so these two board-worker states need an explicit projection. */
void parity_sound_storage_state(bool saving, bool error);
void parity_idle_failure(bool failed);

static bool has(const char *id, const char *part) { return strstr(id, part) != NULL; }
static void text(char *out, size_t capacity, const char *value)
{
    snprintf(out, capacity, "%s", value);
}
static void tap(const char *caption)
{
    parity_check(parity_tap_text(caption), caption);
    parity_advance(320);
}

/* Stable text discovery is used for fixture setup/geometry, never child indices. */
static lv_obj_t *find_label(lv_obj_t *root, const char *caption, bool exact, bool visible)
{
    if (lv_obj_is_hidden(root) || (visible && !lv_obj_is_visible(root))) return NULL;
    if (lv_obj_check_type(root,&lv_label_class)) {
        const char *value=lv_label_get_text(root);
        if (exact ? !strcmp(value,caption) : strstr(value,caption)!=NULL) return root;
        const char *dots=strstr(value,"...");
        if(!dots) dots=strstr(value,"…");
        if(dots && dots-value>=4 && !strncmp(value,caption,(size_t)(dots-value))) return root;
    }
    for (unsigned i=0;i<lv_obj_get_child_count(root);i++) {
        lv_obj_t *found=find_label(lv_obj_get_child(root,i),caption,exact,visible);
        if (found) return found;
    }
    return NULL;
}
static void expect(const char *caption)
{
    parity_check(find_label(lv_screen_active(),caption,false,true) ||
                 find_label(lv_layer_top(),caption,false,true),caption);
}
static void scroll_to(const char *caption)
{
    lv_obj_t *item=find_label(parity_root(),caption,true,false);
    parity_check(item!=NULL,caption);
    lv_obj_scroll_to_view_recursive(item,LV_ANIM_OFF);
    parity_event("input.scroll-target",caption);
    parity_advance(80);
    parity_check(find_label(parity_root(),caption,true,true)!=NULL,caption);
}
static void expect_disabled(const char *caption)
{
    lv_obj_t *label=find_label(parity_root(),caption,true,false);
    parity_check(label!=NULL,caption);
    lv_obj_t *control=label;
    while(control && !lv_obj_is_clickable(control)) control=lv_obj_get_parent(control);
    parity_check(control && lv_obj_has_state(control,LV_STATE_DISABLED),"explicit control disabled");
}
static void assert_pending(bool pending)
{
    orb_model_snapshot_t *model=calloc(1,sizeof(*model));
    parity_check(model && orb_model_snapshot(model),"pending model snapshot");
    parity_check(model->prefs_pending==pending,"intended pending model state");
    free(model);
}

static void seed(const char *id)
{
    orb_model_reset();
    /* ".empty-transcript" is a Voice state with a live session, not an empty model. */
    const bool empty = has(id, ".empty") && !has(id, ".empty-transcript");
    const bool offline = has(id, ".offline") || !strncmp(id,"offline.",8);
    if (!empty) {
        orb_session_t sessions[] = {
            {.id="parity-claude", .agent=ORB_AGENT_CLAUDE, .title="Orb firmware",
             .project="Orb", .state=ORB_SESSION_BUSY, .reason="Building", .summary="Running four build jobs.",
             .updated=1793196300, .can_message=true},
            {.id="parity-codex", .agent=ORB_AGENT_CODEX, .title="Media library",
             .project="Media", .state=ORB_SESSION_DONE, .reason="Tests passed", .summary="All checks completed.",
             .updated=1793196200, .can_message=true},
            {.id="dot", .agent=ORB_AGENT_DOT, .title="Dot", .state=ORB_SESSION_IDLE,
             .reason="Ready", .updated=1793196300, .can_message=true},
        };
        if (has(id, ".long")) {
            text(sessions[0].title, sizeof(sessions[0].title), "A very long firmware session title with every word retained");
            text(sessions[0].summary, sizeof(sessions[0].summary),
                "The navigation refactor is building with four workers. Keep this complete explanation available while the title and text extend beyond one round-display viewport.");
        }
        if (has(id, ".error")) sessions[0].state=ORB_SESSION_ERROR;
        if (has(id,".read-only")) sessions[0].can_message=false;
        parity_check(orb_model_sessions_chunk(sessions, 3, true, false), "sessions fixture");
        orb_agent_stream_t stream = {.session="parity-claude", .seq=0, .at=1793196300,
            .state=ORB_SESSION_BUSY, .activity="Working", .stale=has(id,".stale"), .steps_count=2,
            .steps={{.id="read",.kind=ORB_STEP_TOOL,.label="Read navigation",.state=ORB_STEP_DONE},
                    {.id="build",.kind=ORB_STEP_TOOL,.label="Build with four jobs",.state=ORB_STEP_RUNNING}},
            .assistant={.id="answer",.text="Building the shared screen framework, then running its tests.",.final=true}};
        if (has(id,".error")) { stream.state=ORB_SESSION_ERROR; stream.steps[1].state=ORB_STEP_FAILED; }
        if(has(id,".step-counts")) {
            stream.steps[0].has_added=stream.steps[0].has_removed=true;
            stream.steps[0].added=42;stream.steps[0].removed=8;
        }
        if (!has(id,".loading") && !has(id,".waiting")) parity_check(orb_model_apply_stream(&stream), "stream fixture");
        stream=(orb_agent_stream_t){.session="dot",.seq=0,.at=1793196300,.state=ORB_SESSION_IDLE,
            .activity="Ready",.assistant={.id="dot-answer",.text="Standup at 9:30, then a free afternoon. Rain after four.",.final=true}};
        if(has(id,"dot.long")) text(stream.assistant.text,sizeof(stream.assistant.text),
            "This answer preserves all of its words while the round reading viewport scrolls. Review the entire explanation before sending another voice request to Dot.");
        if (!has(id,".loading") && !has(id,".ask") && !has(id,".checking") && !has(id,".not-connected")) parity_check(orb_model_apply_stream(&stream), "Dot fixture");
        if(has(id,"dot.checking") || has(id,"dot.not-connected")) parity_check(orb_model_remove_session("dot"),"Dot not in focus");
        orb_usage_snapshot_t usage={.count=4,.windows={
            {.provider=ORB_AGENT_CLAUDE,.label="5 hours",.used_percent=62,.resets_at=1793196840,.observed_at=1793194860,.source=ORB_USAGE_USAGE},
            {.provider=ORB_AGENT_CLAUDE,.label="week",.used_percent=41,.resets_at=1793456400,.observed_at=1793194860,.source=ORB_USAGE_USAGE},
            {.provider=ORB_AGENT_CODEX,.label="5 hours",.used_percent=38,.resets_at=1793206200,.observed_at=1793194860,.source=ORB_USAGE_ROLLOUT},
            {.provider=ORB_AGENT_CODEX,.label="week",.used_percent=71,.resets_at=1793456400,.observed_at=1793194860,.source=ORB_USAGE_ROLLOUT}}};
        if (has(id,".stale")) for (unsigned i=0;i<usage.count;i++) usage.windows[i].stale=true;
        if (has(id,".unknown")) { usage.count=0; usage.unknown[0]=usage.unknown[1]=true; }
        if (has(id,".weekly-only")) { usage.windows[0]=usage.windows[1];usage.windows[1]=usage.windows[3];usage.count=2; }
        if (has(id,".no-reset")) for(unsigned i=0;i<usage.count;i++) usage.windows[i].resets_at=0;
        if (has(id,".minimum")) for(unsigned i=0;i<usage.count;i++) usage.windows[i].used_percent=0;
        if (has(id,".maximum")) for(unsigned i=0;i<usage.count;i++) usage.windows[i].used_percent=100;
        parity_check(orb_model_set_usage(&usage), "usage fixture");
        const uint32_t hidden[3]={0};
        parity_check(orb_model_set_summary(2, hidden), "idle summary");
    }
    orb_model_set_connection(!offline, !has(id,".loading") && !offline);
    if (offline) parity_status.link=ORB_SHELL_LINK_OFFLINE;
}

static void start(const char *id)
{
    parity_shell_create(false);
    seed(id);
    if (!strcmp(id,"stack.empty")) { parity_status.clock_time[0]=0; parity_status.clock_date[0]=0; }
    orb_shell_update(parity_shell, &parity_status);
    parity_advance(64);
}

static void agents(const char *id)
{
    start(id);
    if(has(id,"dot.conversation") || has(id,"dot.long")) {
        const orb_message_t message={.target="dot",.text="What is on tomorrow?"};
        orb_model_complete_message_receipt(&message,ORB_SEND_DELIVERED,"Delivered",NULL);
    }
    if(has(id,"idle.allocation-error")) parity_idle_failure(true);
    orb_shell_surface_t route=ORB_SHELL_AGENTS;
    if (has(id,"sessions.")) route=ORB_SHELL_SESSIONS;
    if (has(id,"session.")) route=ORB_SHELL_SESSION;
    if (has(id,"usage.")) route=ORB_SHELL_USAGE;
    if (has(id,"dot.")) route=ORB_SHELL_DOT;
    if (has(id,"idle.")) route=ORB_SHELL_IDLE_SESSIONS;
    if (route==ORB_SHELL_SESSION) orb_shell_select_session(parity_shell,"parity-claude");
    else parity_navigate(route);
    parity_advance(720);
    /* Usage's new Sweep begins after the navigation transition. Ordinary
     * catalogue samples/assertions remain settled; timed Alive captures own
     * its intermediate arc and count samples. */
    if (route==ORB_SHELL_USAGE) parity_advance(450);
    if(has(id,"idle.allocation-error")) { expect("Couldn’t load sessions");return; }
    if (has(id,"idle.") && !has(id,".loading") && !has(id,".offline")) {
        orb_idle_request_t request;
        if (orb_model_take_idle_request(&request)) {
            orb_session_t idle={.id="parity-idle",.title="Archived firmware investigation",.project="Orb",
                .agent=ORB_AGENT_CODEX,.state=ORB_SESSION_IDLE,.reason="Idle",.updated=1793190000};
            parity_check(orb_model_complete_idle(&request, &idle, has(id,".empty") ? 0 : 1,
                has(id,".more") ? "parity-page-2" : NULL, has(id,".error") ? "unavailable" : NULL), "idle response");
        }
        parity_advance(80);
    }
    if (has(id,"usage.codex")) { orb_shell_twist(parity_shell,1); parity_advance(80); }
    if(has(id,"dot.not-connected")) {
        orb_idle_request_t request;
        parity_check(orb_model_take_idle_request(&request),"Dot discovery request");
        parity_check(orb_model_complete_idle(&request,NULL,0,NULL,NULL),"Dot discovery exhausted");
        parity_advance(80);expect("Dot isn’t connected");
    }
    if(has(id,"dot.checking")) expect("Loading…");
    if(has(id,"dot.ask")) expect("Ask Dot");
    if(has(id,"session.waiting")) expect("Waiting for activity");
    if (has(id,"session.outside-focus")) {
        parity_check(orb_model_remove_session("parity-claude"),"remove selected focus session");
        parity_advance(80);expect("Outside focus");
    }
    if (has(id,".weekly-only")) expect("Week");
    if (has(id,".minimum")) expect("0%");
    if (has(id,".maximum")) expect("100%");
    if (has(id,"agents.stale")) expect("Last known");
    if (has(id,".scroll-end")) scroll_to("Usage");
}

static orb_card_t card_fixture(const char *id)
{
    orb_card_t card={.id="parity-card",.session="parity-claude",.kind=ORB_CARD_APPROVAL,
        .title="Run this command?",.body="cmake --build sim/build -j4",.detail="Build the local simulator.",
        .reply=true,.options_count=2,.options={{.id="deny",.label="Deny",.role=ORB_OPTION_CANCEL},
                                            {.id="allow",.label="Allow",.role=ORB_OPTION_PRIMARY}}};
    if (has(id,"question")) {
        card.kind=ORB_CARD_CHOICE;
        text(card.title,sizeof(card.title),"Which checks should run?");
        text(card.body,sizeof(card.body),"Choose the next step explicitly.");
        text(card.options[1].id,sizeof(card.options[1].id),"tests");
        text(card.options[1].label,sizeof(card.options[1].label),"All tests");
    }
    if (has(id,"message")) {
        card.kind=ORB_CARD_MESSAGE; card.options_count=0;
        text(card.title,sizeof(card.title),"Build complete");
        text(card.body,sizeof(card.body),"The simulator build succeeded and is ready for review.");
    }
    if (has(id,".long")) {
        text(card.title,sizeof(card.title),"Review this complete command before approving the next operation");
        text(card.body,sizeof(card.body),"nice -n 10 cmake --build sim/build --target orb_parity -j4 && nice -n 10 ctest --test-dir sim/build -j4 --output-on-failure");
        text(card.detail,sizeof(card.detail),"All words remain available by scrolling. This command uses the repository's local simulator and changes no owner configurations or live services.");
    }
    if (has(id,".extra-options")) {
        card.options_count=4;
        card.options[2]=(orb_option_t){.id="turn",.label="Allow for turn",.role=ORB_OPTION_DEFAULT};
        card.options[3]=(orb_option_t){.id="session",.label="Allow for session",.role=ORB_OPTION_DEFAULT};
    }
    if (has(id,".turn-scope")) {
        card.scope=ORB_APPROVAL_TURN;
        text(card.options[1].label,sizeof(card.options[1].label),"Allow for turn");
    }
    if (has(id,".no-cancel")) {
        card.options[0]=(orb_option_t){.id="first",.label="First option",.role=ORB_OPTION_DEFAULT};
    }
    if (has(id,".no-reply")) card.reply=false;
    if (has(id,".expired")) { card.has_expiry=true;card.expires=1; }
    return card;
}
static void cards(const char *id)
{
    start(id);
    orb_card_t card=card_fixture(id);
    parity_check(orb_model_upsert_card(&card),"card fixture");
    if (has(id,".replayed-answer")) {
        const orb_answer_t answer={.card="parity-card",.option="deny"};
        parity_check(orb_model_answer(&answer),"pre-view recorded local answer");
        parity_check(orb_model_ack_answer("parity-card"),"pre-view answer handoff");
    }
    parity_advance(32);
    orb_shell_select_card(parity_shell,"parity-card");
    parity_advance(720);
    if (has(id,".missing") && !has(id,".missing-offline")) { orb_model_cancel_card("parity-card"); parity_advance(64);expect("Canceled"); }
    if (has(id,".expired")) {
        card.has_expiry=true;card.expires=1;
        parity_check(orb_model_upsert_card(&card),"expired prompt fixture");
        parity_check(orb_model_expire(2)==1,"expire without hub cancellation");
        parity_advance(80);expect("Expired");
    }
    if (has(id,".refreshing") || has(id,".missing-offline")) {
        /* A reconnect snapshot replays sessions and clears cards until the hub
         * re-sends them: the genuine refreshing state (not a local expiry). */
        orb_model_snapshot_t *model=calloc(1,sizeof(*model));
        parity_check(model && orb_model_snapshot(model),"replay snapshot");
        parity_check(orb_model_sessions_chunk(model->sessions,model->session_count,true,false),"session replay clears cards");
        free(model);
        if(has(id,".missing-offline")) orb_model_set_connection(false,false);
        parity_advance(80);expect(has(id,".missing-offline")?"Mac offline":"Loading…");
    }
    /* Extra scopes open on their request; review starts at the top. */
    if (has(id,".replayed-answer")) expect("Sending…");
    if (has(id,"receipt")) {
        tap("Deny");
        if (!has(id,".pending")) {
            orb_answer_t answer={.card="parity-card",.option="deny"};
            parity_check(orb_model_complete_answer("parity-card", has(id,".failed") || has(id,".cancelled") ? NULL : &answer, has(id,".delivered"),
                has(id,".failed") ? "unavailable" : has(id,".cancelled") ? "cancelled" : NULL),"answer completion");
            parity_advance(32);
            /* Record the immediate receipt before the existing 600ms return.
             * The separate delayed scenario intentionally preserves that bug. */
            if (has(id,".delayed")) parity_advance(1000);
        }
    }
}

static void voice(const char *id)
{
    start(id);
    orb_voice_state_t state={.target="parity-claude",.stream=1,.level=66};
    text(state.text,sizeof(state.text),has(id,".long") ?
         "Please review the complete shared screen framework, run every simulator scenario with four workers, and preserve every visible pixel and semantic action before the next structural phase begins. Keep the receipt visible until I explicitly close it." :
         "Build with four jobs and run the tests.");
    state.phase=ORB_VOICE_IDLE;
    if (has(id,".starting")) state.phase=ORB_VOICE_STARTING;
    if (has(id,".recording")) state.phase=ORB_VOICE_RECORDING;
    if (has(id,".transcribing")) state.phase=ORB_VOICE_TRANSCRIBING;
    if (has(id,".review")) state.phase=ORB_VOICE_CONFIRMING;
    if (has(id,".sending")) state.phase=ORB_VOICE_SENDING;
    if (has(id,".error") || has(id,".no-speech")) {
        state.phase=ORB_VOICE_ERROR;
        text(state.error_code,sizeof(state.error_code),has(id,".no-speech") ? "no_speech" : "unavailable");
        text(state.error,sizeof(state.error),has(id,".no-speech") ? "No speech detected" : "Session inbox is not listening.");
    }
    if (has(id,".empty-transcript") || has(id,".no-text")) state.text[0]=0;
    if (has(id,".timeout")) {
        state.phase=ORB_VOICE_ERROR;
        text(state.error_code,sizeof(state.error_code),"timeout");
        text(state.error,sizeof(state.error),"Check the session before trying again.");
    }
    if (has(id,"voice.time.")) text(state.target,sizeof(state.target),"time");
    if (has(id,".ended")) text(state.target,sizeof(state.target),"ended-session");
    parity_check(orb_model_voice_focus(state.target,NULL),"voice focus");
    parity_check(orb_model_set_voice(&state),"voice fixture");
    if (has(id,"send.") || has(id,"voice.time.saved") || has(id,"voice.time.failed")) {
        orb_message_t message={.target="parity-claude",.stream=1};
        text(message.target,sizeof(message.target),state.target);
        text(message.text,sizeof(message.text),state.text);
        orb_send_status_t result=has(id,".queued") ? ORB_SEND_QUEUED : has(id,".delivered") || has(id,"voice.time.saved") ? ORB_SEND_DELIVERED :
            has(id,".failed") ? ORB_SEND_FAILED : ORB_SEND_UNKNOWN;
        orb_model_complete_message_receipt(&message,result,
            has(id,".failed") ? "Session inbox is not listening." : has(id,".delivered") ? "Received by the session." :
            has(id,".queued") ? "Waiting in the session inbox." : "Check the session before trying again.",
            has(id,".unknown") ? "timeout" : has(id,".failed") ? "unavailable" : NULL);
    }
    parity_navigate(ORB_SHELL_VOICE);
    parity_advance(720);
    if (has(id,"voice.starting.no-text")) expect("Starting microphone");
    if (has(id,"voice.recording.no-text")) expect("Listening");
    if (has(id,".empty-transcript")) expect("No transcript");
    if (has(id,".ended") || has(id,".read-only") || has(id,".empty-transcript")) expect_disabled("Send");
    if (has(id,".timeout")) expect("Unknown");
    if (has(id,"voice.time.review")) expect("Save");
    if (has(id,"voice.time.saved")) expect("Saved");
    if (has(id,"voice.time.failed")) expect("Not saved");
    if (has(id,".send-action")) { tap("Send"); parity_advance(32); }
    if (has(id,".cancel-action")) { tap("Cancel"); parity_advance(32); }
}

static void settings(const char *id)
{
    start(id);
    if (has(id,"sound.saving")) parity_sound_storage_state(true,false);
    if (has(id,"sound.save-error")) parity_sound_storage_state(false,true);
    if (has(id,"glass.clear")) parity_check(orb_glass_set_preference(ORB_GLASS_PREFERENCE_CLEAR),"Glass Clear fixture");
    if (has(id,"glass.tinted")) parity_check(orb_glass_set_preference(ORB_GLASS_PREFERENCE_TINTED),"Glass Tinted fixture");
    /* "sources.offline" must not match "sources.off": offline before the hub
     * confirmed preferences is the unknown state, not a confirmed Off. */
    if (has(id,".owner-f") || has(id,".owner-c") || (has(id,"sources.off") && !has(id,".offline")) || has(id,"sources.recent-off")) {
        orb_prefs_t prefs;orb_model_prefs_defaults(&prefs);
        if(has(id,".owner-f") || has(id,".owner-c")) {
            prefs.temperature_owner=true;
            prefs.temperature=has(id,".owner-f")?ORB_TEMPERATURE_F:ORB_TEMPERATURE_C;
        }
        if(has(id,"sources.off") && !has(id,".offline")) prefs.sources[0]=prefs.sources[1]=prefs.sources[2]=false;
        if(has(id,"sources.recent-off")) prefs.idle_minutes=0;
        parity_check(orb_model_set_prefs(&prefs),"confirmed setting fixture");
    }
    if (has(id,"quiet.enabled") || has(id,"quiet.minimum") || has(id,"quiet.maximum") ||
        has(id,"sound.silent") || has(id,"sound.spoken-off")) {
        orb_sound_preferences_t prefs;orb_sound_preferences(&prefs);
        prefs.quiet_enabled=has(id,"quiet.enabled");
        if(has(id,"quiet.minimum")) prefs.quiet_start=prefs.quiet_end=0;
        if(has(id,"quiet.maximum")) prefs.quiet_start=prefs.quiet_end=23;
        if(has(id,"sound.silent")) prefs.silent=true;
        if(has(id,"sound.spoken-off")) prefs.spoken_replies=false;
        parity_check(orb_sound_set_preferences(&prefs),"confirmed sound fixture");
        if(prefs.silent) orb_shell_set_silent(parity_shell,true);
    }
    if (has(id,"quiet.saving")) parity_sound_storage_state(true,false);
    if (has(id,"quiet.save-error")) parity_sound_storage_state(false,true);
    if (has(id,"display.aod-off") || has(id,"display.pickup-off")) {
        orb_power_preferences_t prefs=orb_power_default_preferences();
        prefs.brightness_pct=70;
        if(has(id,"display.aod-off")) prefs.always_on=false;
        if(has(id,"display.pickup-off")) prefs.motion_wake=false;
        orb_shell_set_power_preferences(parity_shell,&prefs);
        parity_check(orb_shell_power_wake(parity_shell,ORB_POWER_WAKE_PWR).accepted,"reveal settings after controller boot");
        parity_advance(400);
        parity_check(!orb_shell_power_snapshot(parity_shell).redact,"display prefs remain visible");
    }
    orb_shell_surface_t route=ORB_SHELL_SETTINGS;
    if (has(id,"display")) route=ORB_SHELL_DISPLAY;
    if (has(id,"access")) route=ORB_SHELL_ACCESSIBILITY;
    if (has(id,"units")) route=ORB_SHELL_UNITS;
    if (has(id,"temperature")) route=ORB_SHELL_TEMPERATURE;
    if (has(id,"sources")) route=ORB_SHELL_SESSION_SETTINGS;
    if (has(id,"sound") || has(id,"quiet") || has(id,"palette")) route=ORB_SHELL_SOUND;
    if (has(id,"battery")) route=ORB_SHELL_BATTERY;
    if (has(id,"about") || has(id,"licences")) route=ORB_SHELL_ABOUT;
    parity_navigate(route); parity_advance(720);
    if (has(id,".glass")) tap("Glass");
    if (has(id,".licences")) tap("Licenses");
    if (has(id,".text-size")) tap("Text size");
    if (has(id,".bold")) tap("Bold text");
    if (has(id,".reduce-motion")) tap("Reduce motion");
    if (has(id,".reduce-transparency")) tap("Reduce transparency");
    if (has(id,".contrast")) tap("Increase contrast");
    if (has(id,"sound.volume")) tap("Volume");
    /* Sound root shows the worker's save state on its own rows (review #14);
     * the Quiet Hours editor has its own settings.quiet.saving scenario. */
    if (has(id,"sound.saving")) expect("Saving");
    if (has(id,"palette")) tap("System sounds");
    if (has(id,"quiet")) {
        tap("Quiet hours");
        if (has(id,".starts") || has(id,"quiet.minimum") || has(id,"quiet.maximum") || has(id,"quiet.saving") || has(id,"quiet.save-error")) tap("Starts");
        if (has(id,".ends")) tap("Ends");
    }
    if ((has(id,"temperature") || has(id,"sources")) && (has(id,".pending") || has(id,".error"))) {
        if (has(id,"sources")) tap("Claude");
        else tap("°C");
        if (has(id,".error")) orb_model_complete_prefs(NULL,"unavailable");
        parity_advance(64);
        assert_pending(has(id,".pending"));
        expect(has(id,".pending")?"Saving":"Not saved");
    }
    if (has(id,"battery")) {
        parity_status.battery_detail=(orb_battery_detail_t){.present=true,.battery_pct=80,.usb=true,
            .charging=true,.health_valid=true,.health_pct=96,.temperature_valid=true,.temperature_c=25};
        if (has(id,".unknown")) parity_status.battery_detail=(orb_battery_detail_t){.present=false,.battery_pct=-1};
        if(has(id,".desk-limit")) { parity_status.battery_detail.desk_limit_active=true;parity_status.desk_charge_limit=true; }
        if(has(id,".on-battery")) { parity_status.usb=false;parity_status.battery_detail.usb=false;parity_status.battery_detail.charging=false; }
        if(has(id,".invalid-health")) { parity_status.battery_detail.health_valid=false;parity_status.battery_detail.temperature_valid=false; }
        parity_status.battery_setting_error=has(id,".error");
        orb_shell_update(parity_shell,&parity_status); parity_advance(64);
        if(has(id,".desk-limit")) expect("Desk limit reached");
        if(has(id,".on-battery")) expect("On battery");
    }
    if(has(id,".scroll-end")) scroll_to(has(id,"palette")?"Collect":has(id,"access")?"Increase contrast":
        has(id,"display")?"Quiet mode":has(id,"sources")?"Includes recently changed sessions":has(id,"sound")?"Always On":
        has(id,"about")?"Licenses":"Glass");
    if(has(id,"quiet.saving")) expect("Saving");
    if(has(id,"quiet.save-error")) expect("Not saved");
}

static void surfaces(const char *id)
{
    start(id);
    orb_shell_surface_t route=ORB_SHELL_APPS;
    if (has(id,"stack")) route=ORB_SHELL_STACK;
    if (has(id,"notifications")) route=ORB_SHELL_NOTIFICATIONS;
    if (!strncmp(id,"offline.",8)) route=ORB_SHELL_OFFLINE;
    if (has(id,"charging")) route=ORB_SHELL_CHARGING;
    if (has(id,"onboarding")) route=ORB_SHELL_ONBOARDING;
    if (has(id,"compass")) route=ORB_SHELL_COMPASS;
    if (has(id,"control")) route=ORB_SHELL_CONTROL_CENTER;
    if (has(id,"notifications") && !has(id,".empty")) {
        orb_card_t card=card_fixture(has(id,".long") ? "message.long" : "message");
        if(has(id,".source-home") || has(id,".source-system") || has(id,".source-updates")) card.session[0]=0;
        if(has(id,".source-home")) text(card.id,sizeof(card.id),"home-parity");
        if(has(id,".source-updates")) text(card.id,sizeof(card.id),"update-parity");
        if(has(id,".source-codex")) text(card.session,sizeof(card.session),"parity-codex");
        if(has(id,".source-dot")) text(card.session,sizeof(card.session),"dot");
        parity_check(orb_model_upsert_card(&card),"notification fixture");
        parity_advance(64);
    }
    if (has(id,"stack.media")) {
        orb_model_nowplaying_capability(true);
        const orb_nowplaying_t media={.app="Music",.title="Midnight City",.artist="M83",
            .playing=true,.state=ORB_MEDIA_PLAYING,.volume=60,.duration=243,.position=121};
        parity_check(orb_model_set_nowplaying(&media),"media fixture");
        if (has(id,".pending") || has(id,".error")) parity_check(orb_model_nowplaying_request(ORB_MEDIA_PAUSE,0),"media pending");
        if (has(id,".error")) orb_model_complete_nowplaying("unavailable");
    }
    if (has(id,"stack.card")) {
        orb_card_t card=card_fixture(id);
        parity_check(orb_model_upsert_card(&card),"Stack card fixture");
    }
    if (has(id,"stack.home")) { orb_home_load_demo(); orb_home_set_connection(true,true); }
    if (has(id,"stack.timer")) {
        parity_check(orb_time_timer_set(210),"Stack timer fixture");
        parity_check(orb_time_timer_start((int64_t)parity_now*1000),"Stack timer start");
        if (has(id,".paused")) orb_time_timer_pause((int64_t)parity_now*1000);
    }
    if (has(id,"stack.date")) {
        const orb_usage_snapshot_t usage={0};
        parity_check(orb_model_set_usage(&usage),"Stack date-only usage");
    }
    if (has(id,"control.silent")) { orb_shell_set_silent(parity_shell,true);orb_sound_set_silent(true); }
    if (has(id,"control.brightness.minimum") || has(id,"control.brightness.maximum")) {
        parity_status.brightness=has(id,".minimum")?0:100;orb_shell_update(parity_shell,&parity_status);
    }
    parity_navigate(route); parity_advance(720);
    if (has(id,"notifications.cleared") || has(id,"notifications.undo")) {
        tap("Clear all");expect("Undo clear");expect("No notifications");
        if(has(id,"notifications.undo")) { tap("Undo clear");expect("Build complete"); }
    }
    if (has(id,"notifications.detail")) {
        orb_shell_select_card(parity_shell,has(id,".source-home")?"home-parity":has(id,".source-updates")?"update-parity":"parity-card");
        parity_navigate(ORB_SHELL_NOTIFICATION_DETAIL); parity_advance(720);
        if(has(id,".retained")) { parity_check(orb_model_cancel_card("parity-card"),"retire detail live card");parity_advance(80);expect("Build"); }
        if(has(id,".unavailable")) {
            orb_shell_select_card(parity_shell,"never-seen-notification");parity_navigate(ORB_SHELL_NOTIFICATION_DETAIL);parity_advance(720);expect("Notification unavailable");
        }
    }
    if (has(id,"control.volume")) { parity_tap(323,326); parity_advance(320); }
    if (has(id,"control.home")) {
        orb_home_load_demo();
        if (has(id,".empty") || has(id,".unavailable"))
            parity_check(orb_home_snapshot_chunk_available(NULL,0,NULL,0,NULL,0,!has(id,".unavailable"),true,false),"Control Home fixture");
        orb_home_set_connection(!has(id,".offline"),!has(id,".loading") && !has(id,".offline"));
        parity_advance(64);
        /* Centre content paging; edge navigation remains owned by the shell. */
        parity_drag(233,300,233,170,240);
        parity_advance(320);
    }
    if (has(id,"control.volume.turn")) { orb_shell_twist(parity_shell,3); parity_advance(80); }
    if (has(id,"control.brightness.turn")) { orb_shell_twist(parity_shell,3); parity_advance(80); }
    if (has(id,"control.flashlight")) { parity_tap(333,132);parity_advance(320);parity_check(orb_shell_brightness(parity_shell)==100,"flashlight owns full brightness"); }
    if (has(id,"control.quiet")) {
        /* Quiet is an explicit lock/desk action. Its true settled endpoint is
         * the public face; a manual wake clears Quiet before revealing apps. */
        parity_tap(233,132);
        parity_advance(800);
        orb_power_snapshot_t power=orb_shell_power_snapshot(parity_shell);
        parity_check(orb_shell_quiet(parity_shell) && power.redact && power.state==ORB_POWER_ALWAYS_ON,
                     "Quiet action settles to the public face");
    }
    if (has(id,"volume-hud")) {
        if(has(id,".minimum") && !has(id,".media")) orb_sound_set_volume(0);
        if(has(id,".maximum") && !has(id,".media")) orb_sound_set_volume(100);
        if(has(id,".silent")) { orb_sound_set_silent(true);orb_shell_set_silent(parity_shell,true); }
        if(has(id,".media")) orb_shell_media_volume(parity_shell,has(id,".minimum")?0:has(id,".maximum")?100:45);
        else orb_shell_show_volume(parity_shell);
        /* The gauge spring settles before its 1500ms display-only expiry. */
        parity_advance(800);expect(has(id,".media")?"Mac volume":has(id,".silent")?"Silent":"Speaker");
    }
    if (has(id,"spoken")) {
        orb_shell_spoken_reply(parity_shell,true,has(id,".buffering"),
            has(id,".buffering") || has(id,".empty") ? "" : has(id,".long") ? "This spoken answer contains a long explanation that remains fully available as captions while the shell keeps the underlying app context." :
            "Standup at 9:30, then a free afternoon.");
        parity_advance(80);
    }
    if (has(id,"power-sheet")) {
        orb_shell_button(parity_shell,&(orb_button_event_t){.button=ORB_BUTTON_PWR,.gesture=ORB_BUTTON_HOLD,.synthetic=true});
        parity_advance(320);
    }
    if (has(id,"charging")) {
        parity_status.battery_pct=has(id,".unknown") ? -1 : has(id,".full") ? 100 : has(id,".low")?9:47;
        parity_status.charging=!has(id,".full") && !has(id,".unplugged") && !has(id,".low");
        if(has(id,".unplugged") || has(id,".low")) parity_status.usb=false;
        orb_shell_update(parity_shell,&parity_status); parity_advance(64);
    }
    if(has(id,"onboarding.scroll-end")) scroll_to("Lock");
}

static void system_views(const char *id)
{
    start(id);
    if (has(id,"setup")) {
        parity_status.net=has(id,".error") ? ORB_SHELL_NET_FAILED : has(id,".connecting") ?
            ORB_SHELL_NET_CONNECTING : ORB_SHELL_NET_PROVISIONING;
        text(parity_status.service_name,sizeof(parity_status.service_name),"ORB_PARITY");
        text(parity_status.pop,sizeof(parity_status.pop),has(id,".loading") ? "" : "12345678");
    } else if (has(id,"pairing")) {
        parity_status.link=ORB_SHELL_LINK_PAIRING;
        text(parity_status.pairing_code,sizeof(parity_status.pairing_code),has(id,".loading") ? "" : "482915");
    } else if (has(id,"starting")) parity_status.net=ORB_SHELL_NET_STARTING;
    else if (has(id,"service-error")) text(parity_status.service_error,sizeof(parity_status.service_error),"Audio service unavailable. Reconnect your Mac and retry.");
    else if (has(id,"identify")) parity_status.identify=true;
    else if (has(id,"updates")) {
        if (has(id,".offered")) parity_status.ota=ORB_SHELL_OTA_OFFERED;
        if (has(id,".downloading")) { parity_status.ota=ORB_SHELL_OTA_DOWNLOADING; parity_status.progress=42; }
        if (has(id,".verifying")) parity_status.ota=ORB_SHELL_OTA_VERIFYING;
        if (has(id,".ready")) parity_status.ota=ORB_SHELL_OTA_READY;
        if (has(id,".failed")) { parity_status.ota=ORB_SHELL_OTA_FAILED; text(parity_status.error,sizeof(parity_status.error),"Mac disconnected during verification. Reconnect and try again."); }
        text(parity_status.version,sizeof(parity_status.version),"0.5.3");
        text(parity_status.notes,sizeof(parity_status.notes),"Shared screen framework and reliable navigation.");
        if (has(id,".current")) text(parity_status.update_check_result,sizeof(parity_status.update_check_result),"Your Orb is up to date.");
        if (has(id,".error")) text(parity_status.update_check_result,sizeof(parity_status.update_check_result),"Could not check. Reconnect your Mac and try again.");
    }
    if(has(id,".low-battery")) { parity_status.battery_pct=15;parity_status.charging=false;parity_status.usb=false; }
    if(has(id,"connection.connecting")) parity_status.net=ORB_SHELL_NET_CONNECTING;
    if(has(id,"connection.failed")) parity_status.net=ORB_SHELL_NET_FAILED;
    if(has(id,"connection.unprovisioned")) parity_status.net=ORB_SHELL_NET_UNPROVISIONED;
    if(has(id,"connection.searching")) parity_status.link=ORB_SHELL_LINK_SEARCHING;
    if(has(id,"connection.link-connecting")) parity_status.link=ORB_SHELL_LINK_CONNECTING;
    if(has(id,"connection.unauthorized")) parity_status.link=ORB_SHELL_LINK_UNAUTHORIZED;
    if(has(id,".no-name")) parity_status.hub_name[0]=0;
    /* As orb_link delivers an over-long hub name: whole words, one ellipsis (polish #10). */
    if(has(id,".long-name")) text(parity_status.hub_name,sizeof(parity_status.hub_name),"A Mac computer with a maximum length name and many complete\u2026");
    if(has(id,".start-zero")) parity_status.progress=0;
    if(has(id,".complete")) parity_status.progress=100;
    orb_shell_update(parity_shell,&parity_status); parity_advance(720);
    if(has(id,".low-battery")) scroll_to("Charge Orb, or choose Update to continue.");
    if(has(id,"updates.mandatory.downloading.background")) {
        orb_shell_back(parity_shell);parity_advance(800);
        parity_check(orb_shell_surface(parity_shell)!=ORB_SHELL_UPDATES,"OTA background preserves workspace");
    }
    if (has(id,"connection") || has(id,"maintenance") || has(id,"forget") || has(id,"unpair")) {
        parity_navigate(ORB_SHELL_SETTINGS); parity_advance(720); tap("Mac");
        if (!has(id,"connection")) tap("Maintenance");
        if (has(id,"forget")) tap("Forget Wi-Fi");
        if (has(id,"unpair")) tap("Unpair Mac");
    }
    if (has(id,"updates.ordinary")) {
        parity_navigate(ORB_SHELL_UPDATES); parity_advance(720);
        if (has(id,".pending")) tap("Check now");
    }
}

#define S(id,family,state,rim,fn) {id,family,state,rim,fn}
const parity_scenario_t parity_shell_scenarios[]={
    S("apps.default","Apps","catalogue","app-focus",surfaces),
    S("stack.default","Stack","populated","card-page",surfaces),
    S("stack.empty","Stack","empty","card-page",surfaces),
    S("stack.offline","Stack","offline","card-page",surfaces),
    S("stack.card.approval","Stack","approval-card","card-page",surfaces),
    S("stack.card.question","Stack","question-card","card-page",surfaces),
    S("stack.card.message","Stack","message-card","card-page",surfaces),
    S("stack.home","Stack","home-card","card-page",surfaces),
    S("stack.timer","Stack","running-timer-card","card-page",surfaces),
    S("stack.timer.paused","Stack","paused-timer-card","card-page",surfaces),
    S("stack.date","Stack","date-floor","card-page",surfaces),
    S("stack.media","Stack","now-playing","card-page",surfaces),
    S("stack.media.pending","Stack","media-pending","card-page",surfaces),
    S("stack.media.error","Stack","media-error","card-page",surfaces),
    S("notifications.default","Notifications","populated","list-scroll",surfaces),
    S("notifications.empty","Notifications","empty","list-scroll",surfaces),
    S("notifications.offline","Notifications","offline","list-scroll",surfaces),
    S("notifications.detail","Notification detail","message","reading-scroll",surfaces),
    S("notifications.detail.long","Notification detail","long-text","reading-scroll",surfaces),
    S("control.default","Control","brightness","brightness",surfaces),
    S("control.volume","Control Volume","volume","volume",surfaces),
    S("control.home","Control Home","home-page","home-focus",surfaces),
    S("control.home.empty","Control Home","empty","home-focus",surfaces),
    S("control.home.loading","Control Home","loading","home-focus",surfaces),
    S("control.home.offline","Control Home","offline","home-focus",surfaces),
    S("control.home.unavailable","Control Home","unavailable","home-focus",surfaces),
    S("control.brightness.turn","Control","brightness-detents","brightness",surfaces),
    S("control.volume.turn","Control Volume","volume-detents","volume",surfaces),
    S("agents.default","Agents","live","session-focus",agents),
    S("agents.empty","Agents","empty","session-focus",agents),
    S("agents.offline","Agents","offline","session-focus",agents),
    S("agents.loading","Agents","syncing","session-focus",agents),
    S("agents.error","Agents","session-error","session-focus",agents),
    S("agents.long","Agents","long-text","session-focus",agents),
    S("sessions.default","Sessions","populated","list-scroll",agents),
    S("sessions.empty","Sessions","empty","list-scroll",agents),
    S("sessions.offline","Sessions","offline","list-scroll",agents),
    S("sessions.long","Sessions","long-text","list-scroll",agents),
    S("idle.default","Idle Sessions","page","list-scroll",agents),
    S("idle.empty","Idle Sessions","empty","list-scroll",agents),
    S("idle.loading","Idle Sessions","pending","list-scroll",agents),
    S("idle.error","Idle Sessions","error","list-scroll",agents),
    S("idle.offline","Idle Sessions","offline","list-scroll",agents),
    S("idle.more","Idle Sessions","next-page-available","list-scroll",agents),
    S("session.default","Session","stream","reading-scroll",agents),
    S("session.loading","Session","waiting-for-stream","reading-scroll",agents),
    S("session.stale","Session","stale","reading-scroll",agents),
    S("session.offline","Session","offline","reading-scroll",agents),
    S("session.long","Session","long-text","reading-scroll",agents),
    S("usage.default","Usage","Claude","provider-page",agents),
    S("usage.codex","Usage","Codex","provider-page",agents),
    S("usage.stale","Usage","stale","provider-page",agents),
    S("usage.unknown","Usage","unknown","provider-page",agents),
    S("usage.offline","Usage","offline","provider-page",agents),
    S("dot.default","Dot","reply","reading-scroll",agents),
    S("dot.loading","Dot","waiting","reading-scroll",agents),
    S("dot.empty","Dot","empty","reading-scroll",agents),
    S("dot.offline","Dot","offline","reading-scroll",agents),
    S("approval.default","Approval","review","reading-scroll",cards),
    S("approval.long","Approval","long-text","reading-scroll",cards),
    S("approval.offline","Approval","offline-disabled","reading-scroll",cards),
    S("approval.missing","Approval","cancelled","reading-scroll",cards),
    S("question.default","Question","review","reading-scroll",cards),
    S("question.long","Question","long-text","reading-scroll",cards),
    S("question.offline","Question","offline-disabled","reading-scroll",cards),
    S("message.default","Message card","message","reading-scroll",cards),
    S("message.long","Message card","long-text","reading-scroll",cards),
    S("receipt.pending","Card receipt","queued","reading-scroll",cards),
    S("receipt.recorded","Card receipt","accepted-undelivered","reading-scroll",cards),
    S("receipt.delivered","Card receipt","delivered","reading-scroll",cards),
    S("receipt.failed","Card receipt","failed","reading-scroll",cards),
    S("receipt.cancelled","Card receipt","cancelled","reading-scroll",cards),
    S("receipt.delivered.delayed","Card receipt","after-existing-auto-return","reading-scroll",cards),
    S("voice.idle","Voice","idle","none",voice),
    S("voice.starting","Voice","starting","none",voice),
    S("voice.recording","Voice","recording","none",voice),
    S("voice.transcribing","Voice","transcribing","none",voice),
    S("voice.review","Voice review","review","reading-scroll",voice),
    S("voice.review.long","Voice review","long-text","reading-scroll",voice),
    S("voice.review.offline","Voice review","offline-disabled","reading-scroll",voice),
    S("voice.review.send-action","Voice review","explicit-send","reading-scroll",voice),
    S("voice.review.cancel-action","Voice review","explicit-cancel","reading-scroll",voice),
    S("voice.sending","Voice send result","pending","reading-scroll",voice),
    S("voice.error","Voice","error","reading-scroll",voice),
    S("voice.no-speech","Voice","no-speech","none",voice),
    S("send.queued","Voice send result","queued","reading-scroll",voice),
    S("send.delivered","Voice send result","delivered","reading-scroll",voice),
    S("send.failed","Voice send result","failed","reading-scroll",voice),
    S("send.unknown","Voice send result","delivery-unknown","reading-scroll",voice),
    S("spoken.default","Spoken reply","playing","none-passive",surfaces),
    S("spoken.buffering","Spoken reply","buffering","none-passive",surfaces),
    S("spoken.empty","Spoken reply","waiting-for-captions","none-passive",surfaces),
    S("spoken.long","Spoken reply","long-text","none-passive",surfaces),
    S("volume-hud.default","Volume HUD","visible","none-passive",surfaces),
    S("settings.root","Settings","root","list-scroll",settings),
    S("settings.display","Display","root","list-scroll",settings),
    S("settings.access","Accessibility","root","list-scroll",settings),
    S("settings.access.text-size","Accessibility","text-size","list-scroll",settings),
    S("settings.access.bold","Accessibility","bold-text","none",settings),
    S("settings.access.reduce-motion","Accessibility","reduce-motion","none",settings),
    S("settings.access.reduce-transparency","Accessibility","reduce-transparency","none",settings),
    S("settings.access.contrast","Accessibility","increase-contrast","none",settings),
    S("settings.glass","Glass","preference","list-scroll",settings),
    S("settings.units","Units","root","list-scroll",settings),
    S("settings.temperature","Temperature","root","list-scroll",settings),
    S("settings.temperature.pending","Temperature","pending","list-scroll",settings),
    S("settings.temperature.error","Temperature","error","list-scroll",settings),
    S("settings.temperature.offline","Temperature","offline-disabled","list-scroll",settings),
    S("settings.sources","Session sources","root","list-scroll",settings),
    S("settings.sources.pending","Session sources","pending-disabled","list-scroll",settings),
    S("settings.sources.error","Session sources","error","list-scroll",settings),
    S("settings.sources.offline","Session sources","offline-disabled","list-scroll",settings),
    S("settings.sound","Sound","root","list-scroll",settings),
    S("settings.sound.saving","Sound","quiet-hours-start-saving-projection","list-scroll",settings),
    S("settings.sound.save-error","Sound","storage-error-projection","list-scroll",settings),
    S("settings.sound.volume","Sound volume","volume","volume",settings),
    S("settings.palette","Sound palette","palette","list-scroll",settings),
    S("settings.quiet","Quiet Hours","root","list-scroll",settings),
    S("settings.quiet.starts","Quiet Hours","start-hour","hour",settings),
    S("settings.quiet.ends","Quiet Hours","end-hour","hour",settings),
    S("settings.battery","Battery","telemetry","list-scroll",settings),
    S("settings.battery.unknown","Battery","unknown","list-scroll",settings),
    S("settings.battery.error","Battery","setting-error","list-scroll",settings),
    S("charging.default","Charging","charging","none",surfaces),
    S("charging.full","Charging","full","none",surfaces),
    S("charging.unknown","Charging","unknown","none",surfaces),
    S("settings.about","About","root","list-scroll",settings),
    S("settings.licences","Licences","list","list-scroll",settings),
    S("updates.ordinary","Updates ordinary","idle","reading-scroll",system_views),
    S("updates.ordinary.pending","Updates ordinary","checking","reading-scroll",system_views),
    S("updates.ordinary.current","Updates ordinary","up-to-date","reading-scroll",system_views),
    S("updates.ordinary.error","Updates ordinary","check-error","reading-scroll",system_views),
    S("updates.ordinary.offline","Updates ordinary","offline","reading-scroll",system_views),
    S("updates.mandatory.offered","Updates mandatory","offered","reading-scroll",system_views),
    S("updates.mandatory.downloading","Updates mandatory","downloading","reading-scroll",system_views),
    S("updates.mandatory.verifying","Updates mandatory","verifying","reading-scroll",system_views),
    S("updates.mandatory.ready","Updates mandatory","ready","reading-scroll",system_views),
    S("updates.mandatory.failed","Updates mandatory","failed-long-error","reading-scroll",system_views),
    S("system.starting","Setup","starting","none",system_views),
    S("system.setup","Setup","provisioning","reading-scroll",system_views),
    S("system.setup.loading","Setup","preparing-code","reading-scroll",system_views),
    S("system.setup.connecting","Setup","connecting","reading-scroll",system_views),
    S("system.setup.error","Setup","failed","reading-scroll",system_views),
    S("system.pairing","Pairing","pair-code","reading-scroll",system_views),
    S("system.pairing.loading","Pairing","preparing-code","reading-scroll",system_views),
    S("system.connection","Connection","connected","list-scroll",system_views),
    S("system.connection.offline","Connection","offline","list-scroll",system_views),
    S("system.maintenance","Maintenance","root","none",system_views),
    S("system.forget","Forget Wi-Fi","confirmation","reading-scroll",system_views),
    S("system.unpair","Unpair Mac","confirmation","reading-scroll",system_views),
    S("system.service-error","Service failure","error","reading-scroll",system_views),
    S("system.identify","Identify","identify","none",system_views),
    S("onboarding.default","Onboarding","tutorial","list-scroll",surfaces),
    S("power-sheet.default","Power sheet","actions","none",surfaces),
    S("compass.default","Compatibility Compass","compatibility","none",surfaces),
    S("offline.default","Offline","disconnected","none",surfaces),
    S("notifications.cleared","Notifications","cleared-undo-available","list-scroll",surfaces),
    S("notifications.undo","Notifications","undo-restored","list-scroll",surfaces),
    S("notifications.detail.retained","Notification detail","live-card-retired","reading-scroll",surfaces),
    S("notifications.detail.unavailable","Notification detail","unavailable","reading-scroll",surfaces),
    S("approval.extra-options","Approval","extra-options","reading-scroll",cards),
    S("approval.turn-scope","Approval","turn-scope","reading-scroll",cards),
    S("approval.replayed-answer","Approval","replayed-answer","reading-scroll",cards),
    S("approval.refreshing","Approval","refreshing","reading-scroll",cards),
    S("approval.missing-offline","Approval","missing-offline","reading-scroll",cards),
    S("approval.expired","Approval","expired","reading-scroll",cards),
    S("question.no-cancel","Question","no-cancel","reading-scroll",cards),
    S("question.no-reply","Question","no-reply","reading-scroll",cards),
    S("agents.stale","Agents","stale","session-focus",agents),
    S("agents.read-only","Agents","read-only","session-focus",agents),
    S("session.outside-focus","Session","outside-focus","reading-scroll",agents),
    S("session.read-only","Session","read-only","reading-scroll",agents),
    S("sessions.scroll-end","Sessions","end-of-list","list-scroll",agents),
    S("usage.weekly-only","Usage","weekly-only","provider-page",agents),
    S("usage.no-reset","Usage","no-reset","provider-page",agents),
    S("usage.minimum","Usage","minimum","provider-page",agents),
    S("usage.maximum","Usage","maximum","provider-page",agents),
    S("voice.review.empty-transcript","Voice review","empty-transcript","reading-scroll",voice),
    S("voice.review.ended","Voice review","ended","reading-scroll",voice),
    S("voice.review.read-only","Voice review","read-only","reading-scroll",voice),
    S("voice.timeout","Voice","timeout-before-send","reading-scroll",voice),
    S("voice.time.review","Voice local Time","review","reading-scroll",voice),
    S("voice.time.saved","Voice local Time","saved","reading-scroll",voice),
    S("voice.time.failed","Voice local Time","failed","reading-scroll",voice),
    S("volume-hud.minimum","Volume HUD","minimum","none-passive",surfaces),
    S("volume-hud.maximum","Volume HUD","maximum","none-passive",surfaces),
    S("volume-hud.silent","Volume HUD","silent","none-passive",surfaces),
    S("volume-hud.media","Volume HUD","media","none-passive",surfaces),
    S("volume-hud.media.minimum","Volume HUD","media.minimum","none-passive",surfaces),
    S("volume-hud.media.maximum","Volume HUD","media.maximum","none-passive",surfaces),
    S("control.silent","Control","silent","runtime-observed",surfaces),
    S("control.flashlight","Control","flashlight","runtime-observed",surfaces),
    S("control.quiet","Control","quiet-to-public-face","runtime-observed",surfaces),
    S("control.brightness.minimum","Control","brightness.minimum","runtime-observed",surfaces),
    S("control.brightness.maximum","Control","brightness.maximum","runtime-observed",surfaces),
    S("settings.battery.desk-limit","Battery","desk-limit","list-scroll",settings),
    S("settings.battery.on-battery","Battery","on-battery","list-scroll",settings),
    S("settings.battery.invalid-health","Battery","invalid-health","list-scroll",settings),
    S("charging.low","Charging","low","none",surfaces),
    S("charging.unplugged","Charging","unplugged","none",surfaces),
    S("settings.glass.clear","Glass","clear","list-scroll",settings),
    S("settings.glass.tinted","Glass","tinted","list-scroll",settings),
    S("settings.temperature.owner-f","Temperature","owner-f","list-scroll",settings),
    S("settings.temperature.owner-c","Temperature","owner-c","list-scroll",settings),
    S("settings.sources.off","Session sources","off","list-scroll",settings),
    S("settings.sources.recent-off","Session sources","recent-off","list-scroll",settings),
    S("settings.sound.silent","Sound","silent","list-scroll",settings),
    S("settings.sound.spoken-off","Sound","spoken-off","list-scroll",settings),
    S("settings.quiet.enabled","Quiet Hours","enabled","runtime-observed",settings),
    S("settings.quiet.minimum","Quiet Hours","minimum","runtime-observed",settings),
    S("settings.quiet.maximum","Quiet Hours","maximum","runtime-observed",settings),
    S("settings.quiet.saving","Quiet Hours","saving","runtime-observed",settings),
    S("settings.quiet.save-error","Quiet Hours","save-error","runtime-observed",settings),
    S("settings.display.aod-off","Display","aod-off","list-scroll",settings),
    S("settings.display.pickup-off","Display","pickup-off","list-scroll",settings),
    S("settings.palette.scroll-end","Sound palette","end-of-list","list-scroll",settings),
    S("settings.sound.scroll-end","Sound","end-of-list","list-scroll",settings),
    S("settings.root.scroll-end","Settings","end-of-list","list-scroll",settings),
    S("settings.display.scroll-end","Display","end-of-list","list-scroll",settings),
    S("settings.access.scroll-end","Accessibility","end-of-list","list-scroll",settings),
    S("settings.sources.scroll-end","Session sources","end-of-list","list-scroll",settings),
    S("settings.about.scroll-end","About","end-of-list","list-scroll",settings),
    S("onboarding.scroll-end","Onboarding","final-instructions","list-scroll",surfaces),
    S("system.connection.connecting","Connection","connecting","list-scroll",system_views),
    S("system.connection.failed","Connection","failed","list-scroll",system_views),
    S("system.connection.unprovisioned","Connection","unprovisioned","list-scroll",system_views),
    S("system.connection.searching","Connection","searching","list-scroll",system_views),
    S("system.connection.link-connecting","Connection","link-connecting","list-scroll",system_views),
    S("system.connection.unauthorized","Connection","unauthorized","list-scroll",system_views),
    S("system.connection.no-name","Connection","no-name","list-scroll",system_views),
    S("system.connection.long-name","Connection","long-name","list-scroll",system_views),
    S("system.pairing.no-name","Pairing","no-name","reading-scroll",system_views),
    S("system.pairing.long-name","Pairing","long-name","reading-scroll",system_views),
    S("updates.mandatory.offered.low-battery","Updates mandatory","offered-low-battery","reading-scroll",system_views),
    S("updates.mandatory.downloading.start-zero","Updates mandatory","downloading-start-zero","reading-scroll",system_views),
    S("updates.mandatory.downloading.complete","Updates mandatory","downloading-complete","reading-scroll",system_views),
    S("updates.mandatory.downloading.background","Updates mandatory","downloading-background","reading-scroll",system_views),
    S("agents.step-counts","Agents","added-removed-step-counts","session-focus",agents),
    S("session.waiting","Session","synced-waiting-stream","reading-scroll",agents),
    S("dot.ask","Dot","ask","reading-scroll",agents),
    S("dot.checking","Dot","checking","reading-scroll",agents),
    S("dot.not-connected","Dot","not-connected","reading-scroll",agents),
    S("dot.conversation","Dot","conversation","reading-scroll",agents),
    S("dot.long","Dot","long","reading-scroll",agents),
    S("voice.starting.no-text","Voice","starting-no-transcript","none",voice),
    S("voice.recording.no-text","Voice","recording-no-transcript","none",voice),
    S("notifications.source-home","Notifications","source-home","list-scroll",surfaces),
    S("notifications.detail.source-home","Notification detail","source-home","reading-scroll",surfaces),
    S("notifications.source-system","Notifications","source-system","list-scroll",surfaces),
    S("notifications.detail.source-system","Notification detail","source-system","reading-scroll",surfaces),
    S("notifications.source-updates","Notifications","source-updates","list-scroll",surfaces),
    S("notifications.detail.source-updates","Notification detail","source-updates","reading-scroll",surfaces),
    S("notifications.source-codex","Notifications","source-codex","list-scroll",surfaces),
    S("notifications.detail.source-codex","Notification detail","source-codex","reading-scroll",surfaces),
    S("notifications.source-dot","Notifications","source-dot","list-scroll",surfaces),
    S("notifications.detail.source-dot","Notification detail","source-dot","reading-scroll",surfaces),
    S("idle.allocation-error","Idle Sessions","allocation-failure-projection","unbound",agents),
};
const unsigned parity_shell_count=sizeof(parity_shell_scenarios)/sizeof(parity_shell_scenarios[0]);
