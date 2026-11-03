"""Check the Games 4 rim response against its formula and physical invariants."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GAMES = ROOT / "firmware/components/orb_games"


def main():
    source = (GAMES / "orb_game_logic.c").read_text()
    start = source.index("static void beads_wall(orb_game_state_t *state)")
    end = source.index("\n}", start) + 2
    fixture = (
        r"""
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "orb_game_logic.h"
static unsigned s_checks, s_contacts;
static void original_wall(orb_game_state_t *state) {
    const float edge = ORB_GAME_RADIUS - ORB_GAME_BEAD_RADIUS;
    orb_game_bead_t *b = &state->game.beads.beads[0];
    float length = hypotf(b->x, b->y);
    if (length > edge) {
        float nx = b->x / length, ny = b->y / length;
        b->x = nx * edge; b->y = ny * edge;
        float into = b->vx * nx + b->vy * ny;
        if (into > 0) {
            b->vx -= into * nx; b->vy -= into * ny;
            b->vx *= 0.998f; b->vy *= 0.998f;
        }
    }
}
"""
        + source[start:end]
        + r"""
static void sample(float x, float y, float vx, float vy) {
    static orb_game_state_t candidate, reference;
    candidate.game.beads.count = 1;
    candidate.game.beads.beads[0] = (orb_game_bead_t){x, y, vx, vy};
    reference = candidate;
    original_wall(&reference); beads_wall(&candidate);
    assert(!memcmp(&candidate.game.beads.beads[0], &reference.game.beads.beads[0], sizeof(orb_game_bead_t)));
    if (isfinite(x) && isfinite(y)) {
        const orb_game_bead_t *b = &candidate.game.beads.beads[0];
        float edge = ORB_GAME_RADIUS - ORB_GAME_BEAD_RADIUS;
        assert(hypotf(b->x, b->y) <= edge + 0.00005f);
        assert(b->vx * b->vx + b->vy * b->vy <= vx * vx + vy * vy + 0.05f);
        if (hypotf(x, y) > edge) {
            /* An outward incoming velocity becomes tangential; an inward
             * velocity is retained. Neither may leave an outward component. */
            assert(b->vx * b->x + b->vy * b->y <= 0.05f);
            s_contacts++;
        }
    }
    s_checks++;
}
int main(void) {
    uint32_t rng = 17;
    for (unsigned i = 0; i < 262144; i++) {
        float v[4];
        for (unsigned j = 0; j < 4; j++) {
            rng = rng * 1664525u + 1013904223u;
            v[j] = ((float)(rng & 0xffffffu) / 16777215.0f - 0.5f) * 600.0f;
        }
        sample(v[0], v[1], v[2], v[3]);
    }
    for (unsigned i = 0; i < 4096; i++) {
        float angle = (float)i * 6.28318530718f / 4096.0f;
        for (int d = -40; d <= 40; d++) {
            float radius = ORB_GAME_RADIUS - ORB_GAME_BEAD_RADIUS + (float)d * 0.0005f;
            sample(cosf(angle) * radius, sinf(angle) * radius, 170.0f, -130.0f);
        }
    }
    sample(0.0f, -0.0f, 0.0f, 0.0f);
    sample(INFINITY, 0.0f, 1.0f, 1.0f);
    sample(NAN, 1.0f, 1.0f, 1.0f);
    assert(s_contacts > 100000);
    printf("Beads wall: %u byte-exact random/boundary checks; %u contained, non-energising rim contacts\n", s_checks, s_contacts);
}
"""
    )
    with tempfile.TemporaryDirectory(prefix="orb-beads-wall-") as temporary:
        directory = Path(temporary)
        path = directory / "wall.c"
        path.write_text(fixture)
        binary = directory / "wall"
        subprocess.run(
            [
                *shlex.split(os.environ.get("CC", "cc")),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-O2",
                "-fsanitize=address,undefined",
                f"-I{GAMES / 'include'}",
                str(path),
                "-lm",
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
