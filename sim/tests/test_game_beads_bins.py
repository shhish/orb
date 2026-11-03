"""Actual-source conservative coverage proof for the Beads32px damage bins."""

import re
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parents[2]
source = (repo / "firmware/components/orb_games/orb_game_view.c").read_text()
a = source.index("static lv_area_t bounds(")
b = source.index("\nstatic void finish_layer(", a)
bounds = source[a:b]
a = source.index("static void bead_damage_rows(")
b = source.index("\nstatic uint32_t damage_gate_sweep(", a)
binning = source[a:b]
definitions = "\n".join(re.findall(r"^#define BEAD_DAMAGE_(?:HEIGHT|ROWS).*$", source, re.M))
native = (repo / "firmware/components/orb_games/orb_game_native.h").read_text()
definitions += "\n" + "\n".join(re.findall(r"^#define ORB_GAME_NATIVE_PHASE.*$", native, re.M))
a = source.index("static bool same(")
b = source.index("\nstatic uint32_t damage_gate_ring(", a)
same = source[a:b]
prelude = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "orb_game_logic.h"
#define TAU 6.28318530718f
#define LV_MIN(a,b) ((a)<(b)?(a):(b))
#define LV_MAX(a,b) ((a)>(b)?(a):(b))
typedef struct { int32_t x1,y1,x2,y2; } lv_area_t;
static unsigned long long checks;
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr,"Beads bins:%d: %s\n",__LINE__,#c);exit(1); } } while(0)
"""
reference = r"""
/* Frozen16px reference. Rectangles may extend below the display until
 * LVGL clips invalidation; retain that existing behavior in both helpers. */
static void old_rows(lv_area_t *rows,const orb_game_sprite_t *sprite)
{
    if (!sprite->radius || !sprite->opacity) return;
    lv_area_t area=bounds(sprite);
    if (area.y2<0 || area.y1>465) return;
    int first=LV_MAX(0,area.y1)/16,last=LV_MIN(465,area.y2)/16;
    for(int i=first;i<=last;i++) {
        rows[i].x1=LV_MIN(rows[i].x1,area.x1);
        rows[i].x2=LV_MAX(rows[i].x2,area.x2);
        rows[i].y1=LV_MIN(rows[i].y1,LV_MAX(area.y1,i*16));
        rows[i].y2=LV_MAX(rows[i].y2,LV_MIN(area.y2,i*16+15));
    }
}
"""
tests = r"""
static unsigned random_value=17;
static unsigned random_next(void) {random_value^=random_value<<13;random_value^=random_value>>17;random_value^=random_value<<5;return random_value;}
static void empty_rows(lv_area_t *rows,unsigned count)
{for(unsigned i=0;i<count;i++)rows[i]=(lv_area_t){466,466,-1,-1};}
static void covers(const lv_area_t *actual,const lv_area_t *expected)
{
    CHECK(actual->x1<=expected->x1 && actual->x2>=expected->x2);
    CHECK(actual->y1<=expected->y1 && actual->y2>=expected->y2);
}
static void sample(const orb_game_sprite_t *sprites,unsigned count)
{
    struct {uint64_t before;lv_area_t rows[BEAD_DAMAGE_ROWS];uint64_t after;} candidate={.before=12345678,.after=87654321};
    lv_area_t reference[30];empty_rows(reference,30);empty_rows(candidate.rows,BEAD_DAMAGE_ROWS);
    for(unsigned i=0;i<count;i++) {
        old_rows(reference,&sprites[i]);bead_damage_rows(candidate.rows,&sprites[i]);
    }
    CHECK(candidate.before==12345678 && candidate.after==87654321);
    for(unsigned i=0;i<30;i++) if(reference[i].x1<=reference[i].x2)covers(&candidate.rows[i/2],&reference[i]);
    for(unsigned i=0;i<BEAD_DAMAGE_ROWS;i++) {
        const lv_area_t *row=&candidate.rows[i];
        if(row->x1>row->x2)continue;
        CHECK(row->y1>=(int)i*32 && row->y2<=(int)i*32+31);
        int first=LV_MAX(0,row->y1),last=LV_MIN(465,row->y2);
        CHECK(last-first+1<=32);
        /* Even QSPI expansion cannot escape an even-start/odd-end bucket. */
        CHECK((first&~1)>=(int)i*32 && (last|1)<=(int)i*32+31);
        CHECK((last-first+1)*466*2<=466*42*2);
    }
    /* Every visible old/new AA bound is covered, independently of old bins. */
    for(unsigned i=0;i<count;i++) {
        if(!sprites[i].radius || !sprites[i].opacity)continue;
        lv_area_t area=bounds(&sprites[i]);
        int first=LV_MAX(0,area.y1),last=LV_MIN(465,area.y2);
        for(int y=first;y<=last;y++) {
            const lv_area_t *row=&candidate.rows[y/32];
            CHECK(row->x1<=area.x1 && row->x2>=area.x2 && row->y1<=y && row->y2>=y);
        }
    }
}
static void boundaries(void)
{
    orb_game_sprite_t pair[2]={{.shape=ORB_GAME_BEAD,.radius=5,.opacity=255},{.shape=ORB_GAME_BEAD,.radius=5,.opacity=255}};
    for(int edge=-32;edge<=512;edge+=16)for(int offset=-8;offset<=8;offset++)for(unsigned radius=0;radius<9;radius++) {
        pair[0].x=pair[1].x=233;pair[0].radius=pair[1].radius=radius;
        pair[0].y=edge+offset-.5f;pair[1].y=edge+offset+.5f;sample(pair,2);
    }
    lv_area_t rows[BEAD_DAMAGE_ROWS];empty_rows(rows,BEAD_DAMAGE_ROWS);
    pair[0].radius=0;pair[1].opacity=0;bead_damage_rows(rows,&pair[0]);bead_damage_rows(rows,&pair[1]);
    for(unsigned i=0;i<BEAD_DAMAGE_ROWS;i++)CHECK(rows[i].x1>rows[i].x2);
    orb_game_sprite_t held={.shape=ORB_GAME_BEAD,.radius=5,.opacity=255,.x=233,.y=233},unchanged=held;
    CHECK(same(&held,&unchanged));unchanged.x+=.1f;CHECK(same(&held,&unchanged));
    unchanged.x+=.7f;CHECK(!same(&held,&unchanged));
}
static void random_batches(void)
{
    orb_game_sprite_t sprites[512];
    for(unsigned batch=0;batch<5000;batch++) {
        unsigned count=random_next()%513;
        for(unsigned i=0;i<count;i++) {
            sprites[i]=(orb_game_sprite_t){.x=(int)(random_next()%760)-150+.25f,
                .y=(int)(random_next()%760)-150+.5f,.shape=ORB_GAME_BEAD,
                .radius=(uint16_t)(random_next()%45),.opacity=(uint8_t)random_next()};
        }
        sample(sprites,count);
    }
}
int main(void)
{
    CHECK(BEAD_DAMAGE_HEIGHT==32 && BEAD_DAMAGE_ROWS==15);
    boundaries();random_batches();
    printf("Beads32px bins: %llu actual-source checks;16px-superset and every AA bound covered, clipping/even alignment/half-arena bounds, zero-active and unchanged semantics pass\n",checks);
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="orb-beads-bins-") as directory:
    c = Path(directory) / "bins.c"
    binary = Path(directory) / "bins"
    c.write_text(prelude + definitions + "\n" + bounds + binning + same + reference + tests)
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
            "-I" + str(repo / "firmware/components/orb_games/include"),
            str(c),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
