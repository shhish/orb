/* Phase-zero evidence runner. One fresh process owns a virtual UI timeline.
 * No SDL window, board, sockets, filesystem persistence or wall-clock input. */
#include "parity.h"
#include "sim_display.h"
#include "orb_fonts.h"
#include "orb_icons.h"
#include "orb_home.h"
#include "orb_time.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include "orb_sound.h"
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int alive_os_capture(const char *sequence, const char *mode, const char *output);

orb_shell_t *parity_shell;
lv_display_t *parity_display;
orb_shell_snapshot_t parity_status;
uint32_t parity_now = 10000;
static lv_obj_t *s_root;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;
static const char *s_id, *s_profile, *s_output;
static const parity_scenario_t *s_scenario;
static char s_transition[64];
static unsigned s_sound_requested[ORB_SOUND_CUE_COUNT], s_sound_accepted[ORB_SOUND_CUE_COUNT];
static const char *s_cues[] = {"detent","tap","open","close","needs-you","done","sent",
    "listening-start","listening-stop","failed","home-on","home-off","timer","alarm",
    "game-roll","game-bead","game-pop","game-switch","game-collect"};
_Static_assert(sizeof(s_cues)/sizeof(s_cues[0])==ORB_SOUND_CUE_COUNT,"parity cue roles need updating");
typedef struct { uint32_t at; char kind[64], detail[1600]; } event_t;
static event_t s_events[8192];
static unsigned s_event_count;

/* Executable interposition affects only this test binary. time() is frozen;
 * the independent monotonic virtual UI tick controls every animation sample. */
time_t time(time_t *out)
{
    time_t fixed = 1793182140; /* 2026-10-28 10:09 UTC */
    if (out) *out = fixed;
    return fixed;
}
static uint32_t tick(void) { return parity_now; }
void parity_check(bool ok, const char *why)
{
    if (!ok) { fprintf(stderr, "parity %s/%s @%u: %s\n", s_id, s_profile, parity_now, why); exit(1); }
}
void parity_event(const char *kind, const char *detail)
{
    parity_check(s_event_count < sizeof(s_events)/sizeof(s_events[0]), "event capacity");
    event_t *e = &s_events[s_event_count++]; e->at = parity_now;
    snprintf(e->kind, sizeof(e->kind), "%s", kind);
    snprintf(e->detail, sizeof(e->detail), "%s", detail ? detail : "");
}
void parity_sound_event(const char *op, unsigned cue, bool accepted)
{
    if (cue < ORB_SOUND_CUE_COUNT && strcmp(op,"stop")) {
        s_sound_requested[cue]++; if (accepted) s_sound_accepted[cue]++;
    }
    char detail[120]; snprintf(detail,sizeof(detail),"%s; role=%s; accepted=%d",op,
        cue < ORB_SOUND_CUE_COUNT ? s_cues[cue] : "invalid", accepted);
    parity_event("sound",detail);
}
void parity_set_root(lv_obj_t *root) { s_root = root; }
lv_obj_t *parity_root(void) { return s_root ? s_root : lv_screen_active(); }
void parity_advance(unsigned ms)
{
    for (unsigned elapsed=0; elapsed<ms;) {
        unsigned step = LV_MIN(16u,ms-elapsed); elapsed+=step; parity_now+=step;
        if (parity_shell) orb_shell_poll(parity_shell);
        parity_apps_poll();
        lv_timer_handler();
    }
    lv_refr_now(parity_display);
}
static void read_pointer(lv_indev_t *input, lv_indev_data_t *out) { (void)input; *out=s_input; }
static void action(orb_shell_action_t value, void *context)
{
    (void)context;
    char detail[40]; snprintf(detail,sizeof(detail),"%u",value); parity_event("shell.action",detail);
}
void parity_shell_create(bool fixture)
{
    if (parity_shell) { orb_shell_destroy(parity_shell); parity_shell=NULL; }
    orb_model_reset(); orb_home_reset();
    if (fixture) { orb_model_load_demo(); orb_home_load_demo(); }
    orb_model_set_connection(true,true);
    parity_status=(orb_shell_snapshot_t){.net=ORB_SHELL_NET_CONNECTED,.link=ORB_SHELL_LINK_ONLINE,
        .battery_pct=80,.rssi=-54,.brightness=70,.usb=true};
    snprintf(parity_status.clock_time,sizeof(parity_status.clock_time),"10:09");
    snprintf(parity_status.clock_date,sizeof(parity_status.clock_date),"Wednesday, October 28");
    snprintf(parity_status.hub_name,sizeof(parity_status.hub_name),"Fixture Mac");
    snprintf(parity_status.device_id,sizeof(parity_status.device_id),"orb-parity-fixture");
    snprintf(parity_status.firmware_version,sizeof(parity_status.firmware_version),"parity-v1");
    parity_shell=orb_shell_create(lv_screen_active(),action,NULL);
    parity_check(parity_shell!=NULL,"shell create");
    parity_set_root(orb_shell_root(parity_shell));
    orb_shell_update(parity_shell,&parity_status);
    orb_shell_environment(parity_shell,0,0,1,false);
    parity_advance(800);
}
void parity_navigate(orb_shell_surface_t route)
{
    char detail[40]; snprintf(detail,sizeof(detail),"%u",route); parity_event("route",detail);
    orb_shell_navigate(parity_shell,route);
}
static lv_obj_t *find_text(lv_obj_t *root, const char *text)
{
    if (lv_obj_is_hidden(root)) return NULL;
    const char *rim=orb_kit_rim_text_get(root);
    if (rim && !strcmp(rim,text)) return root;
    if (lv_obj_check_type(root,&lv_label_class)) {
        const char *shown=lv_label_get_text(root);
        if (!strcmp(shown,text)) return root;
        const char *dots=strstr(shown,"...");
        if(!dots) dots=strstr(shown,"\xe2\x80\xa6");
        if (dots && dots-shown>=4 && !strncmp(shown,text,(size_t)(dots-shown))) return root;
    }
    for (unsigned i=0;i<lv_obj_get_child_count(root);i++) {
        lv_obj_t *found=find_text(lv_obj_get_child(root,i),text); if (found) return found;
    }
    return NULL;
}
void parity_tap(int x,int y)
{
    char detail[80]; snprintf(detail,sizeof(detail),"physical x=%d y=%d",x,y); parity_event("input.tap",detail);
    s_input.point=(lv_point_t){x,y}; s_input.state=LV_INDEV_STATE_PRESSED; lv_indev_read(s_pointer);
    parity_advance(32); s_input.state=LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); parity_advance(32);
}
bool parity_tap_text(const char *text)
{
    lv_obj_t *label=find_text(parity_root(),text);
    if(!label)label=find_text(lv_layer_top(),text);
    if (!label) return false;
    lv_obj_t *control=label;
    while (control && !lv_obj_is_clickable(control)) control=lv_obj_get_parent(control);
    if (!control) return false;
    lv_obj_scroll_to_view_recursive(control,LV_ANIM_OFF); parity_advance(32);
    label=find_text(parity_root(),text);
    if(!label)label=find_text(lv_layer_top(),text);
    if (!label) return false;
    control=label; while (control && !lv_obj_is_clickable(control)) control=lv_obj_get_parent(control);
    if (!control) return false;
    lv_area_t a; lv_obj_get_coords(control,&a); lv_area_t presented;
    if (orb_round_list_child_bounds(control,control,&presented)) a=presented;
    parity_event("input.target",text);
    parity_tap((a.x1+a.x2)/2,(a.y1+a.y2)/2); return true;
}
void parity_drag(int x1,int y1,int x2,int y2,unsigned ms)
{
    char detail[120]; snprintf(detail,sizeof(detail),"physical %d,%d -> %d,%d duration=%u",x1,y1,x2,y2,ms);
    parity_event("input.drag",detail);
    s_input.point=(lv_point_t){x1,y1}; s_input.state=LV_INDEV_STATE_PRESSED; lv_indev_read(s_pointer); parity_advance(32);
    for (unsigned i=1;i<=8;i++) {
        s_input.point=(lv_point_t){x1+(x2-x1)*(int)i/8,y1+(y2-y1)*(int)i/8};
        lv_indev_read(s_pointer); parity_advance(LV_MAX(1,ms/8));
    }
    s_input.state=LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); parity_advance(32);
}
static void quoted(FILE *file,const char *str)
{
    fputc('"',file);
    for (const unsigned char *p=(const unsigned char *)(str ? str : "");*p;p++) {
        if (*p=='"' || *p=='\\') fprintf(file,"\\%c",*p);
        else if (*p<32) fprintf(file,"\\u%04x",*p);
        else fputc(*p,file);
    }
    fputc('"',file);
}
/* Full traversal is allowed for geometry evidence. Identity never depends on
 * child indices or pointers: semantic text + class + role + presented bounds. */
typedef struct { char type[48],role[32],text[1600]; lv_area_t box; unsigned state; int value; } node_t;
static node_t s_nodes[8192]; static unsigned s_node_count;
static const char *type(lv_obj_t *o)
{
    if(lv_obj_check_type(o,&lv_label_class)) return "label";
    if(lv_obj_check_type(o,&lv_arc_class)) return "arc";
    if(lv_obj_check_type(o,&lv_switch_class)) return "switch";
    if(lv_obj_check_type(o,&lv_slider_class)) return "slider";
    if(lv_obj_check_type(o,&lv_button_class)) return "button";
    if(lv_obj_check_type(o,&lv_line_class)) return "line";
    if(lv_obj_check_type(o,&lv_canvas_class)) return "canvas";
    if(lv_obj_check_type(o,&lv_image_class)) return "image";
    if(lv_obj_check_type(o,&lv_arclabel_class)) return "arclabel";
    return "object";
}
static void nodes(lv_obj_t *o)
{
    if (!lv_obj_is_visible(o)) return;
    lv_area_t b; lv_obj_get_coords(o,&b);
    if (b.x2<0 || b.y2<0 || b.x1>=SIM_D || b.y1>=SIM_D) return;
    const char *t=type(o),*text=orb_kit_rim_text_get(o);
    /* Live-label anchors reserve geometry with transparent text. Compositor
     * semantics can have transparent objects, so filter only text opacity. */
    if (!text && lv_obj_check_type(o,&lv_label_class) && lv_obj_get_style_text_opa(o,0)!=LV_OPA_TRANSP)
        text=lv_label_get_text(o);
    const char *target_text;
    if (orb_kit_ring_semantic_text(o,&target_text) ||
        orb_kit_value_projection_semantic_text(o,&target_text) ||
        orb_kit_value_roll_semantic_text(o,&target_text)) text=target_text;
    bool primary=orb_kit_is_primary(o),click=lv_obj_is_clickable(o),scroll=lv_obj_is_scrollable(o);
    if (text || click || scroll || strcmp(t,"object")) {
        parity_check(s_node_count<8192,"node capacity"); node_t *n=&s_nodes[s_node_count++];
        memset(n,0,sizeof(*n)); snprintf(n->type,sizeof(n->type),"%s",t);
        snprintf(n->role,sizeof(n->role),"%s",primary?"primary":click?"control":scroll?"scroll":text?"text":"content");
        snprintf(n->text,sizeof(n->text),"%s",text?text:""); n->box=b; n->state=lv_obj_get_state(o);
        if (!strcmp(t,"arc") && !orb_kit_ring_semantic_value(o,&n->value)) n->value=lv_arc_get_value(o);
        if (!strcmp(t,"slider")) n->value=lv_slider_get_value(o);
    }
    for(unsigned i=0;i<lv_obj_get_child_count(o);i++) nodes(lv_obj_get_child(o,i));
}
static void trace_text_test(void)
{
    s_id="trace-text";s_profile="test";
    lv_init();parity_display=sim_display_create();parity_check(parity_display!=NULL,"test display");
    const char *texts[]={"visible ink","layout reservation","compositor semantics","hidden label"};
    for(unsigned i=0;i<4;i++) {
        lv_obj_t *label=lv_label_create(lv_screen_active());lv_label_set_text(label,texts[i]);
        lv_obj_set_pos(label,100,80+40*(int)i);
        if(i==1)lv_obj_set_style_text_opa(label,LV_OPA_TRANSP,0);
        if(i==2)lv_obj_set_style_opa(label,LV_OPA_TRANSP,0);
        if(i==3)lv_obj_set_hidden(label,true);
    }
    lv_obj_update_layout(lv_screen_active());nodes(lv_screen_active());
    unsigned found[4]={0};
    for(unsigned i=0;i<s_node_count;i++)
        for(unsigned j=0;j<4;j++)if(!strcmp(s_nodes[i].text,texts[j]))found[j]++;
    parity_check(found[0]==1,"visible text retained");
    parity_check(found[1]==0,"transparent layout reservation excluded");
    parity_check(found[2]==1,"compositor semantic text retained");
    parity_check(found[3]==0,"hidden text excluded");
    puts("parity trace text: visible ink and compositor semantics retained; layout reservations excluded");
}
static int node_compare(const void *a,const void *b)
{
    const node_t *x=a,*y=b; int c=strcmp(x->role,y->role); if(c)return c;
    c=strcmp(x->type,y->type); if(c)return c; c=strcmp(x->text,y->text); if(c)return c;
    if(x->box.y1!=y->box.y1)return x->box.y1<y->box.y1?-1:1;
    if(x->box.x1!=y->box.x1)return x->box.x1<y->box.x1?-1:1;
    if(x->box.y2!=y->box.y2)return x->box.y2<y->box.y2?-1:1;
    if(x->box.x2!=y->box.x2)return x->box.x2<y->box.x2?-1:1;
    if(x->state!=y->state)return x->state<y->state?-1:1;
    return (x->value>y->value)-(x->value<y->value);
}
static void capture(const char *frame)
{
    char path[4096]; snprintf(path,sizeof(path),"%s/%s.png",s_output,frame);
    lv_refr_now(parity_display); parity_check(sim_display_save_png(path),"write PNG");
    snprintf(path,sizeof(path),"%s/%s.json",s_output,frame);
    FILE *f=fopen(path,"wb"); parity_check(f!=NULL,"write trace");
    fprintf(f,"{\n\"schema\":1,\n\"scenario\":");quoted(f,s_id);fprintf(f,",\n\"profile\":");quoted(f,s_profile);
    fprintf(f,",\n\"frame\":");quoted(f,frame);fprintf(f,",\n\"virtual_ms\":%u,\n\"seed\":1330790992,\n",parity_now);
    fprintf(f,"\"rim_catalogue_hint\":");quoted(f,s_scenario->rim);
    char rim[512]; bool app_rim=parity_apps_rim(rim,sizeof(rim));
    fprintf(f,",\n\"rim_observed\":");quoted(f,app_rim?rim:parity_rim_observed());
    char app_state[2048];
    if(parity_apps_semantic(app_state,sizeof(app_state))) { fprintf(f,",\n\"app_state\":");quoted(f,app_state); }
    fprintf(f,",\n\"accessibility\":{\"text_size\":%u,\"bold\":%s,\"reduce_motion\":%s,\"reduce_transparency\":%s}",
        orb_fonts_content_size(),orb_fonts_bold_text()?"true":"false",orb_ui_reduce_motion()?"true":"false",orb_ui_reduce_transparency()?"true":"false");
    orb_model_snapshot_t *model=calloc(1,sizeof(*model)); parity_check(model!=NULL,"model trace allocate");
    parity_check(orb_model_snapshot(model),"model trace snapshot");
    fprintf(f,",\n\"model\":{\"revision\":%llu,\"online\":%s,\"synced\":%s,\"prefs_pending\":%s,\"pending_answers\":%zu,\"voice_phase\":%u,\"send_status\":%u},\n",
        (unsigned long long)model->revision,model->online?"true":"false",model->synced?"true":"false",
        model->prefs_pending?"true":"false",orb_model_pending_answers(),model->voice.phase,model->last_message.status);
    free(model);
    orb_time_snapshot_t time_state;orb_time_snapshot(orb_time_now_us(),&time_state);
    fprintf(f,"\"time\":{\"generation\":%u,\"timer_state\":%u,\"timer_remaining_us\":%lld,\"stopwatch_state\":%u,\"alarm_ringing\":%s,\"reminder_due\":%s,\"saving\":%s,\"save_error\":%s},\n",
        time_state.generation,time_state.timer_state,(long long)time_state.timer_remaining_us,
        time_state.stopwatch_state,time_state.alarm_ringing?"true":"false",time_state.reminder_due?"true":"false",
        time_state.saving?"true":"false",time_state.save_error?"true":"false");
    if(parity_shell) {
        orb_power_snapshot_t p=orb_shell_power_snapshot(parity_shell);
        fprintf(f,"\"power\":{\"enabled\":%s,\"state\":%u,\"redacted\":%s,\"quiet\":%s,\"brightness\":%u},\n",
            p.revision?"true":"false",p.state,p.revision && p.redact?"true":"false",orb_shell_quiet(parity_shell)?"true":"false",p.brightness_pct);
    }
    fprintf(f,"\"surface\":%d,\n\"controls\":[",parity_shell?(int)orb_shell_surface(parity_shell):-1);
    s_node_count=0;
    orb_power_snapshot_t power=parity_shell?orb_shell_power_snapshot(parity_shell):(orb_power_snapshot_t){0};
    bool redacted=power.revision && power.redact;
    if(!redacted)nodes(lv_screen_active());
    nodes(lv_layer_top()); nodes(lv_layer_sys());
    qsort(s_nodes,s_node_count,sizeof(*s_nodes),node_compare);
    for(unsigned i=0;i<s_node_count;i++) {
        const node_t *n=&s_nodes[i];if(i)fputc(',',f);
        fprintf(f,"\n{\"proxy_id\":%u,\"type\":",i);quoted(f,n->type);fprintf(f,",\"role\":");quoted(f,n->role);
        fprintf(f,",\"text\":");quoted(f,n->text);fprintf(f,",\"bounds\":[%d,%d,%d,%d],\"state\":%u,\"value\":%d}",
            n->box.x1,n->box.y1,n->box.x2,n->box.y2,n->state,n->value);
    }
    fprintf(f,"\n],\n\"sound\":[");
    for(unsigned i=0;i<ORB_SOUND_CUE_COUNT;i++) {
        if(i)fputc(',',f);fprintf(f,"{\"role\":");quoted(f,s_cues[i]);
        fprintf(f,",\"requested\":%u,\"accepted\":%u}",s_sound_requested[i],s_sound_accepted[i]);
    }
    fprintf(f,"],\n\"events\":[");
    for(unsigned i=0;i<s_event_count;i++) {
        if(i)fputc(',',f);fprintf(f,"\n{\"at_ms\":%u,\"kind\":",s_events[i].at);quoted(f,s_events[i].kind);
        fprintf(f,",\"detail\":");quoted(f,s_events[i].detail);fputc('}',f);
    }
    fprintf(f,"\n]}\n");parity_check(fclose(f)==0,"close trace");
}
static void transition_setup(const char *id)
{
    parity_shell_create(true);parity_navigate(ORB_SHELL_FACE);parity_advance(800);
    snprintf(s_transition,sizeof(s_transition),"%s",id+strlen("transition."));
}
#define TRANS(id,state) {"transition." id,"Transitions",state,"runtime-observed",transition_setup}
static const parity_scenario_t s_transitions[]={
    TRANS("iris-open","iris-open"),TRANS("iris-close","iris-close"),TRANS("slide-left","slide-left"),
    TRANS("slide-right","slide-right"),TRANS("sheet-arrival","sheet-arrival"),TRANS("sheet-dismissal","sheet-dismissal"),
    TRANS("back","Back"),TRANS("layer-arrival","layer-arrival"),TRANS("layer-departure","layer-departure"),
};
static bool transition_click(const char *text)
{
    lv_obj_t *label=find_text(parity_root(),text);if(!label)return false;
    lv_obj_t *control=label;
    while(control && !lv_obj_is_clickable(control))control=lv_obj_get_parent(control);
    if(!control)return false;
    lv_obj_scroll_to_view_recursive(control,LV_ANIM_OFF);parity_advance(32);
    label=find_text(parity_root(),text);if(!label)return false;
    control=label;while(control && !lv_obj_is_clickable(control))control=lv_obj_get_parent(control);
    if(!control)return false;
    lv_area_t a;lv_obj_get_coords(control,&a);lv_area_t presented;
    if(orb_round_list_child_bounds(control,control,&presented))a=presented;
    s_input.point=(lv_point_t){(a.x1+a.x2)/2,(a.y1+a.y2)/2};
    s_input.state=LV_INDEV_STATE_PRESSED;lv_indev_read(s_pointer);parity_advance(32);
    s_input.state=LV_INDEV_STATE_RELEASED;lv_indev_read(s_pointer);
    parity_event("input.target",text);return true;
}
static bool transition_iris_launch(void)
{
    return transition_click("Agents") || transition_click(ORB_ICON_PROMPT);
}
static void transitions(void)
{
    bool close_iris=!strcmp(s_transition,"iris-close");
    bool dismiss_sheet=!strcmp(s_transition,"sheet-dismissal");
    bool depart_layer=!strcmp(s_transition,"layer-departure");
    if (!strncmp(s_transition,"iris",4)) {
        parity_navigate(ORB_SHELL_APPS);parity_advance(800);
        if(close_iris) { parity_check(transition_iris_launch(),"launch iris source");parity_advance(800); }
    } else if(!strcmp(s_transition,"back")) {
        parity_navigate(ORB_SHELL_SETTINGS);parity_advance(800);
        parity_check(parity_tap_text("Display"),"Back source");parity_advance(800);
    } else if(dismiss_sheet) {
        parity_drag(233,430,233,80,160);parity_advance(800);
    } else if(depart_layer) {
        orb_button_event_t e={.button=ORB_BUTTON_PWR,.gesture=ORB_BUTTON_DOUBLE,.at_ms=parity_now};
        orb_shell_button(parity_shell,&e);parity_advance(800);
    }
    capture("source");
    if(close_iris || dismiss_sheet || depart_layer || !strcmp(s_transition,"back")) {
        parity_event("input.back","BOOT-equivalent public Back");orb_shell_back(parity_shell);
    } else if(!strcmp(s_transition,"iris-open")) parity_check(transition_iris_launch(),"launch Agents iris");
    else if(!strncmp(s_transition,"slide",5) || !strcmp(s_transition,"sheet-arrival")) {
        /* The contact and path are virtual physical-provenance input. Capture
         * the admitted live drag itself, then its release/spring endpoint. */
        s_input.point=(lv_point_t){!strcmp(s_transition,"slide-left")?400:!strcmp(s_transition,"slide-right")?60:233,
            !strcmp(s_transition,"sheet-arrival")?430:233};
        s_input.state=LV_INDEV_STATE_PRESSED;lv_indev_read(s_pointer);
        parity_event("input.drag.begin",s_transition);
    } else {
        orb_button_event_t e={.button=ORB_BUTTON_PWR,.gesture=ORB_BUTTON_DOUBLE,.at_ms=parity_now};
        parity_event("input.button","physical-semantics PWR double");orb_shell_button(parity_shell,&e);
    }
    const unsigned start=parity_now;
    bool drag=!strncmp(s_transition,"slide",5) || !strcmp(s_transition,"sheet-arrival");
    static const unsigned sample[]={0,40,80,120,160,240,400,800};
    for(unsigned i=0;i<sizeof(sample)/sizeof(sample[0]);i++) {
        parity_advance(start+sample[i]-parity_now);
        if(drag && sample[i]<=160) {
            unsigned at=sample[i];
            s_input.point=(lv_point_t){!strcmp(s_transition,"slide-left")?400-330*(int)at/160:
                !strcmp(s_transition,"slide-right")?60+340*(int)at/160:233,
                !strcmp(s_transition,"sheet-arrival")?430-350*(int)at/160:233};
            lv_indev_read(s_pointer);
            if(at==160) {
                s_input.state=LV_INDEV_STATE_RELEASED;lv_indev_read(s_pointer);
                parity_event("input.drag.end",s_transition);
            }
        }
        char frame[64];snprintf(frame,sizeof(frame),"sample-%03u",sample[i]);capture(frame);
    }
    parity_advance(800);capture("settled");
}
static void privacy_setup(const char *id)
{
    parity_shell_create(true);
    orb_shell_surface_t route=strstr(id,"notifications")?ORB_SHELL_NOTIFICATIONS:
        strstr(id,"stack")?ORB_SHELL_STACK:strstr(id,"agents")?ORB_SHELL_AGENTS:
        strstr(id,"session")?ORB_SHELL_SESSION:strstr(id,"card")?ORB_SHELL_CARD:
        strstr(id,"voice")?ORB_SHELL_VOICE:strstr(id,"home")?ORB_SHELL_HOME:
        strstr(id,"settings")?ORB_SHELL_SETTINGS:strstr(id,"control")?ORB_SHELL_CONTROL_CENTER:ORB_SHELL_FACE;
    if(route==ORB_SHELL_SESSION || route==ORB_SHELL_CARD) {
        orb_model_snapshot_t *m=calloc(1,sizeof(*m));parity_check(m!=NULL,"privacy fixture allocate");
        parity_check(orb_model_snapshot(m),"privacy fixture");
        if(route==ORB_SHELL_CARD) { parity_check(m->card_count>0,"private card fixture");orb_shell_select_card(parity_shell,m->cards[0].id); }
        else { parity_check(m->session_count>0,"private session fixture");orb_shell_select_session(parity_shell,m->sessions[0].id); }
        free(m);
    } else parity_navigate(route);
    parity_advance(800);orb_shell_power_enable(parity_shell,true);
    if(strstr(id,"always-on"))orb_shell_set_quiet(parity_shell,true);
    else {
        orb_shell_set_quiet(parity_shell,false);
        orb_button_event_t e={.button=ORB_BUTTON_PWR,.gesture=ORB_BUTTON_SHORT,.at_ms=parity_now};
        orb_shell_button(parity_shell,&e);
    }
    parity_advance(800);
    orb_power_snapshot_t power=orb_shell_power_snapshot(parity_shell);
    parity_check(power.redact && power.state==ORB_POWER_ALWAYS_ON,"private content redacted");
}
#define PRIVACY(place) {"privacy." place ".locked","Privacy","locked " place,"none-redacted",privacy_setup}, \
    {"privacy." place ".always-on","Privacy","always-on " place,"none-redacted",privacy_setup}
static const parity_scenario_t s_privacy[]={
    PRIVACY("face"),PRIVACY("agents"),PRIVACY("session"),PRIVACY("card"),PRIVACY("notifications"),
    PRIVACY("stack"),PRIVACY("voice"),PRIVACY("home"),PRIVACY("settings"),PRIVACY("control"),
};
static void time_layer_setup(const char *id)
{
    parity_shell_create(true);orb_time_init();
    time_t wall=time(NULL);int64_t now=(int64_t)parity_now*1000;
    orb_time_poll(now,wall);
    if(strstr(id,"timer")) {
        parity_check(orb_time_timer_set(1) && orb_time_timer_start(now),"layer timer start");
        orb_time_poll(now+INT64_C(2000000),wall+2);
    } else if(strstr(id,"reminder")) {
        uint32_t reminder;
        parity_check(orb_time_reminder_set("Take the morning medicine",wall+1,&reminder),"layer reminder");
        orb_time_poll(now+INT64_C(2000000),wall+2);
    } else {
        parity_check(orb_time_alarm_set(10,10) && orb_time_alarm_enable(true,wall),"layer alarm enable");
        orb_time_snapshot_t snapshot;orb_time_snapshot(now,&snapshot);
        orb_time_poll(now+INT64_C(60000000),snapshot.alarm_next);
    }
    parity_advance(800);
    if(strstr(id,"dismiss")) { orb_shell_back(parity_shell);parity_advance(800); }
    else if(strstr(id,"snooze")) {
        /* The real layer accepts the same physical-semantics pointer. */
        parity_check(parity_tap_text("Snooze"),"alarm layer Snooze");parity_advance(800);
    }
}
static const parity_scenario_t s_time_layers[]={
    {"time.alarm-layer","Time layers","AlarmRinging","none-alarm-layer",time_layer_setup},
    {"time.timer-layer","Time layers","timer-due-layer","none-alarm-layer",time_layer_setup},
    {"time.reminder-layer","Time layers","reminder-due-layer","none-alarm-layer",time_layer_setup},
    {"time.alarm-layer-dismiss","Time layers","Back-dismiss-preserves-ringing","none-alarm-layer",time_layer_setup},
    {"time.alarm-layer-snooze","Time layers","Snooze-action","none-alarm-layer",time_layer_setup},
};
static const parity_scenario_t *scenario_at(unsigned i)
{
    if(i<parity_shell_count)return &parity_shell_scenarios[i]; i-=parity_shell_count;
    if(i<parity_app_count)return &parity_app_scenarios[i]; i-=parity_app_count;
    if(i<sizeof(s_transitions)/sizeof(s_transitions[0]))return &s_transitions[i];
    i-=sizeof(s_transitions)/sizeof(s_transitions[0]);
    if(i<sizeof(s_privacy)/sizeof(s_privacy[0]))return &s_privacy[i];
    i-=sizeof(s_privacy)/sizeof(s_privacy[0]);return &s_time_layers[i];
}

/* Exercise the same catalogue fixtures without taking screenshots. Refresh
 * each virtual frame: a redraw that ends on the original pixels still spends
 * panel work, and must fail the idle-is-still contract (delight §6 rule 2). */
static unsigned settle_advance(unsigned milliseconds, bool check_clock)
{
    unsigned frames = 0;
    while (milliseconds) {
        unsigned step = LV_MIN(16u, milliseconds);
        parity_now += step;
        if (parity_shell) orb_shell_poll(parity_shell);
        parity_apps_poll();
        lv_timer_handler();
        lv_refr_now(parity_display);
        frames += sim_display_take_changed();
        if (check_clock) parity_check(!orb_kit_motion_timer_running(), "idle motion clock restarted");
        milliseconds -= step;
    }
    return frames;
}

static void settle_check(const char *mode)
{
    bool dimmed = !strcmp(mode, "dimmed") || !strcmp(mode, "dimmed-ticking");
    bool always_on = !strcmp(mode, "always-on") || !strcmp(mode, "always-on-ticking");
    bool waiting = !strcmp(mode, "active-wait");
    bool ticking = !strcmp(mode, "active-ticking") || !strcmp(mode, "dimmed-ticking") || !strcmp(mode, "always-on-ticking");
    parity_check(dimmed || always_on || waiting || ticking || !strcmp(mode, "active"), "unknown settle mode");
    if (parity_shell) {
        if (always_on) orb_shell_environment(parity_shell, 0, 0, 1, true);
        if (dimmed) orb_shell_display_active(parity_shell, false);
    } else if (dimmed || always_on) {
        /* Standalone presenters use the same host visibility capability that
         * dimmed/Always On app hosts deliver in production. */
        orb_kit_motion_set_active(parity_root(), false);
    }
    /* Normal arrivals get their timeout fallback plus full settling time.
     * Only explicitly named waits get their documented 30 s breath budget. */
    /* Notification receipt ages are genuine changing data: their first
     * minute reads in seconds. Exercise an aged, populated list/detail so
     * the two-second idle window falls between actual minute-age changes. */
    bool receipt_age = !strncmp(s_id, "notifications.", 14);
    settle_advance(receipt_age ? 61000 : waiting ? 31000 : 1600, false);
    parity_check(!orb_kit_motion_timer_running(), "settled surface retained motion clock");
    sim_display_take_changed();
    unsigned frames = settle_advance(2000, true);
    if (!ticking) parity_check(frames == 0, "settled surface rendered during two idle seconds");
    printf("Alive settle %s/%s: clock stopped; %u rendered frames in 2000 ms%s\n",
        s_id, mode, frames, ticking ? " (local second ticks allowed)" : "");
}

int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"--test-trace-text")) {trace_text_test();return 0;}
    unsigned count=parity_shell_count+parity_app_count+sizeof(s_transitions)/sizeof(s_transitions[0])+sizeof(s_privacy)/sizeof(s_privacy[0])+sizeof(s_time_layers)/sizeof(s_time_layers[0]);
    if(argc==2 && !strcmp(argv[1],"--list")) {
        fputs("[",stdout);
        for(unsigned i=0;i<count;i++) {
            const parity_scenario_t *s=scenario_at(i);if(i)fputc(',',stdout);
            fputs("{\"id\":",stdout);quoted(stdout,s->id);fputs(",\"family\":",stdout);quoted(stdout,s->family);
            fputs(",\"state\":",stdout);quoted(stdout,s->state);fputs(",\"rim\":",stdout);quoted(stdout,s->rim);fputc('}',stdout);
        }
        puts("]");return 0;
    }
    bool settling=argc==4 && !strcmp(argv[1],"--settle-check");
    bool alive=argc==5 && !strcmp(argv[1],"--alive-capture");
    if(argc!=4 && !alive){fprintf(stderr,"usage: orb_parity SCENARIO PROFILE EXISTING_OUT_DIR | --settle-check SCENARIO MODE | --alive-capture SEQUENCE PROFILE OUT | --list\n");return 2;}
    s_id=argv[settling||alive?2:1];s_profile=settling?"default":argv[alive?3:2];
    s_output=settling?NULL:argv[alive?4:3];
    if(!alive) {
        for(unsigned i=0;i<count;i++)if(!strcmp(s_id,scenario_at(i)->id))s_scenario=scenario_at(i);
        parity_check(s_scenario!=NULL,"unknown scenario");
    }
    parity_check(!strcmp(s_profile,"default")||!strcmp(s_profile,"ax3")||!strcmp(s_profile,"ax3-bold-rt")||!strcmp(s_profile,"reduce-motion"),"unknown profile");
    setenv("TZ","UTC",1);tzset();setlocale(LC_ALL,"C");srand(1330790992);
    lv_init();lv_tick_set_cb(tick);lv_rand_set_seed(1330790992);
    parity_display=sim_display_create();orb_ui_init(NULL);
    parity_check(orb_model_init(),"model init");parity_check(orb_home_init(),"Home init");orb_time_init();
    parity_check(orb_sound_init(),"sound init");
    if(!strncmp(s_profile,"ax3",3))parity_check(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3)==ORB_FONTS_OK,"AX fonts");
    if(!strcmp(s_profile,"ax3-bold-rt")) {
        parity_check(orb_fonts_set_bold_text(true)==ORB_FONTS_OK,"bold fonts");orb_ui_set_reduce_transparency(true);
    }
    if(!strcmp(s_profile,"reduce-motion"))orb_ui_set_reduce_motion(true);
    s_pointer=lv_indev_create();lv_indev_set_type(s_pointer,LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_pointer,read_pointer);lv_indev_set_display(s_pointer,parity_display);
#ifdef ORB_ALIVE_PARITY
    if(alive) return alive_os_capture(s_id,s_profile,s_output);
#else
    parity_check(!alive, "Alive captures require orb_parity");
#endif
    s_scenario->setup(s_id);
    if(settling) { settle_check(argv[3]);return 0; }
    if(s_transition[0])transitions();
    else { parity_advance(0);capture("settled");parity_advance(1600);capture("late-1600"); }
    return 0;
}
