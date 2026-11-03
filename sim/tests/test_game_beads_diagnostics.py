"""Execute the exact staged preparation/draw paths with deterministic receipts."""

import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[2]
view = (root / "firmware/components/orb_games/orb_game_view.c").read_text()
header = (root / "firmware/components/orb_games/include/orb_game_view.h").read_text()
a = header.index("typedef struct {")
b = header.index("} orb_game_view_stats_t;", a) + len("} orb_game_view_stats_t;")
stats = header[a:b]
a = view.index("static bool draw_bead_batch(")
b = view.index("\nstatic void draw(lv_event_t *e)", a)
draw = view[a:b]
a = view.index("static void prepare_bead_batch(")
b = view.index("\nstatic void submit_sprites(", a)
prepare = view[a:b]
# Frozen pre-diagnostic eligibility loop, independent of the new counters.
reference = r"""
static void prepare_reference(orb_game_view_t *v, size_t n)
{
    if (v->kind == ORB_GAME_BEADS) {
        v->bead_batch_eligible = true;
        v->bead_batch_count = 0;
        for (size_t i = 0; i < n; ++i) {
            const orb_game_sprite_t *sprite = &v->next[i];
            if (!sprite->radius || sprite->opacity <= LV_OPA_MIN) {
                if (i < ORB_GAME_BEAD_COUNT) v->preview.game.beads.target_opacity[i] = 0;
                continue;
            }
            if (i >= ORB_GAME_BEAD_COUNT || sprite->shape != ORB_GAME_BEAD ||
                sprite->radius != ORB_GAME_BEAD_DRAW_RADIUS) {
                v->bead_batch_eligible = false;
                break;
            }
            v->preview.game.beads.target_x[i] = (int16_t)lroundf(sprite->x);
            v->preview.game.beads.target_y[i] = (int16_t)lroundf(sprite->y);
            v->preview.game.beads.target_opacity[i] = sprite->opacity;
            v->bead_batch_count = (uint16_t)(i + 1);
        }
    }
}
"""

prelude = r"""
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "orb_game_logic.h"
#define LV_OPA_MIN 2
static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) {fprintf(stderr,"view-counter:%d: %s\n",__LINE__,#c);exit(1);} } while(0)
"""
objects = r"""
typedef int lv_layer_t;
typedef struct {
    orb_game_kind_t kind;
    orb_game_sprite_t next[ORB_GAME_MAX_SPRITES], sprites[ORB_GAME_MAX_SPRITES];
    struct { struct { struct {int16_t target_x[ORB_GAME_BEAD_COUNT],target_y[ORB_GAME_BEAD_COUNT];uint8_t target_opacity[ORB_GAME_BEAD_COUNT];}beads;}game;}preview;
    uint64_t canary;
    uint16_t bead_batch_count;
    bool bead_batch_eligible;
    int bead_masks;
    void *beads_draw;
    orb_game_view_stats_t stats;
} orb_game_view_t;
static bool s_bead_masks_ready, accept_dispatch;
static uint64_t clock_us;
static unsigned dispatch_calls, drain_calls, native_calls;
static uint64_t now_us(void) { return clock_us; }
static bool orb_game_beads_draw_dispatch(void *source, lv_layer_t *layer, size_t count,
 const int16_t *x,const int16_t *y,const uint8_t *opacity,const orb_game_sprite_t *sprites,uint64_t revision)
{ (void)layer;(void)count;(void)x;(void)y;(void)opacity;(void)sprites;(void)revision;dispatch_calls++;return source && accept_dispatch; }
static void finish_layer(lv_layer_t *layer) { (void)layer;drain_calls++;clock_us+=37; }
static void orb_game_native_beads(lv_layer_t *layer,const int *mask,size_t count,const int16_t *x,const int16_t *y,const uint8_t *opacity,const orb_game_sprite_t *sprites)
{ (void)layer;(void)mask;(void)count;(void)x;(void)y;(void)opacity;(void)sprites;native_calls++;clock_us+=91; }
"""
tests = r"""
static unsigned random_value=17;
static unsigned random_next(void) { random_value^=random_value<<13;random_value^=random_value>>17;random_value^=random_value<<5;return random_value; }
static void preparation_oracle(void)
{
    orb_game_view_t actual={.kind=ORB_GAME_BEADS},expected;
    for (unsigned sample=0;sample<10000;sample++) {
        memset(&actual.stats,0,sizeof(actual.stats));
        for (unsigned i=0;i<ORB_GAME_MAX_SPRITES;i++) {
            actual.next[i]=(orb_game_sprite_t){.x=(int)(random_next()%60000)/16.f-1000,
                .y=(int)(random_next()%60000)/16.f-1000,.radius=5,.shape=ORB_GAME_BEAD,.opacity=(uint8_t)random_next()};
            if(i>=ORB_GAME_BEAD_COUNT || random_next()%19==0)actual.next[i].radius=0;
            if(random_next()%101==0)actual.next[i].shape=ORB_GAME_PARTICLE;
            if(random_next()%137==0)actual.next[i].radius=3;
        }
        actual.canary=UINT64_C(0xa5a5a5a55a5a5a5a);
        expected=actual;
        size_t n=random_next()%(ORB_GAME_MAX_SPRITES+1);
        prepare_reference(&expected,n);prepare_bead_batch(&actual,n);
        CHECK(actual.bead_batch_eligible==expected.bead_batch_eligible && actual.bead_batch_count==expected.bead_batch_count);
        CHECK(!memcmp(&actual.preview,&expected.preview,sizeof(actual.preview)));
        CHECK(actual.canary==expected.canary && actual.stats.bead_prepares==1);
        CHECK(actual.stats.bead_total_sprites==n && actual.stats.bead_batch_sprites==actual.bead_batch_count);
        CHECK(actual.stats.bead_rejected_prepares==!actual.bead_batch_eligible);
    }
}
static void rejection_receipts(void)
{
    for(unsigned flag=1;flag<=7;flag++) {
        orb_game_view_t v={.kind=ORB_GAME_BEADS};
        unsigned index=flag&1?256:0;
        for(unsigned i=0;i<=index;i++)v.next[i]=(orb_game_sprite_t){.radius=5,.shape=ORB_GAME_BEAD,.opacity=255};
        if(flag&2)v.next[index].shape=ORB_GAME_PARTICLE;
        if(flag&4)v.next[index].radius=3;
        prepare_bead_batch(&v,index+1);
        CHECK(!v.bead_batch_eligible && v.stats.bead_rejected_prepares==1 && v.stats.bead_reject_flags==flag);
        CHECK(v.stats.bead_reject_index==index && v.stats.bead_reject_shape==v.next[index].shape && v.stats.bead_reject_radius==v.next[index].radius);
        v.next[index].shape=ORB_GAME_DISC;prepare_bead_batch(&v,index+1);
        CHECK(v.stats.bead_rejected_prepares==2 && v.stats.bead_reject_flags==flag);
    }
}
static void draw_receipts(void)
{
    orb_game_view_t v={.kind=ORB_GAME_BEADS};lv_layer_t layer=0;
    CHECK(!draw_bead_batch(&v,&layer));
    CHECK(v.stats.bead_draw_calls==1 && v.stats.bead_masks_missing==1 && !dispatch_calls);
    s_bead_masks_ready=true;
    CHECK(!draw_bead_batch(&v,&layer));
    CHECK(v.stats.bead_ineligible==1 && !dispatch_calls);
    v.bead_batch_eligible=true;accept_dispatch=true;v.beads_draw=(void *)1;
    CHECK(draw_bead_batch(&v,&layer));
    CHECK(v.stats.bead_dispatched==1 && dispatch_calls==1 && !drain_calls && !native_calls);
    v.beads_draw=NULL;
    CHECK(draw_bead_batch(&v,&layer));
    CHECK(v.stats.bead_serial==1 && !v.stats.bead_source_ready && drain_calls==1 && native_calls==1);
    CHECK(v.stats.bead_drain_us==37 && v.stats.bead_native_us==91);
    v.beads_draw=(void *)1;accept_dispatch=false;
    CHECK(draw_bead_batch(&v,&layer));
    CHECK(v.stats.bead_serial==2 && v.stats.bead_source_ready && v.stats.bead_masks_ready);
    CHECK(v.stats.bead_drain_us==74 && v.stats.bead_native_us==182);
    CHECK(v.stats.bead_draw_calls==v.stats.bead_masks_missing+v.stats.bead_ineligible+v.stats.bead_serial+v.stats.bead_dispatched);
}
int main(void)
{
    preparation_oracle();rejection_receipts();draw_receipts();
    printf("Beads path diagnostics: %u actual-source checks; exact prepare oracle, reject flags, mask/ineligible/serial/dispatch receipts and timing pass\n",checks);
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="orb-beads-diag-") as directory:
    c = Path(directory) / "diag.c"
    binary = Path(directory) / "diag"
    c.write_text(prelude + stats + objects + draw + prepare + reference + tests)
    subprocess.run(
        [
            "cc",
            "-std=c11",
            "-O1",
            "-g",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all",
            "-I" + str(root / "firmware/components/orb_games/include"),
            str(c),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
