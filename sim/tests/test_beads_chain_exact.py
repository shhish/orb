"""Check the current Games 4 spatial hash against independent traversal/mapping."""

import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GAMES = ROOT / "firmware/components/orb_games"


def main():
    source = (GAMES / "orb_game_logic.c").read_text()
    cell_mode = "--cell" in sys.argv[1:]
    if cell_mode:
        # Games 4 uses a 16px power-of-two cell again. The old integer-division
        # optimization no longer exists; check the actual clamped index below.
        reference = source
    else:
        # Cell membership is frozen when a constraint pass begins, while the
        # particles move during that pass. An independent exhaustive scan must
        # retain those original bins and descending pair order to be byte exact.
        current = "for (unsigned j = s_bead_cells[cell]; j != 0xFFFF; j = s_bead_next[j]) {"
        assert source.count(current) == 1
        reference = (
            source.replace(
                "static void beads_collide(orb_game_state_t *state)\n{",
                "static void beads_collide(orb_game_state_t *state)\n{\n"
                "    unsigned reference_cells[ORB_GAME_BEAD_COUNT];",
            )
            .replace(
                "s_bead_next[i] = s_bead_cells[cell];",
                "reference_cells[i] = cell;\n        s_bead_next[i] = s_bead_cells[cell];",
            )
            .replace(
                current,
                "for (unsigned j = state->game.beads.count; j-- > i + 1;) {\n"
                "                    if (reference_cells[j] != cell) continue;",
            )
        )
    modules = [GAMES / "orb_game_motion.c", GAMES / "orb_game_record.c"]
    reference += "\n" + "\n".join(path.read_text() for path in modules)
    declarations = (GAMES / "include/orb_game_logic.h").read_text()
    names = sorted(set(re.findall(r"\b(orb_\w+)\s*\(", declarations)))
    fixture = r"""
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "orb_game_logic.h"
void reference_orb_game_init(orb_game_state_t *, orb_game_kind_t, uint32_t);
void reference_orb_game_step(orb_game_state_t *, float, float, bool, bool);
int main(void) {
    static orb_game_state_t candidate, reference;
    static const unsigned counts[] = {32, 128, ORB_GAME_BEAD_COUNT};
    unsigned checks = 0;
    for (unsigned mode = 0; mode < 3; mode++) {
        orb_game_init(&candidate, ORB_GAME_BEADS, 17u + mode * 7109u);
        reference_orb_game_init(&reference, ORB_GAME_BEADS, 17u + mode * 7109u);
        assert(!memcmp(&candidate, &reference, sizeof(candidate)));
        candidate.game.beads.count = (uint16_t)counts[mode];
        candidate.motion_moving = true;
        if (mode == 2) {
            /* Deliberate overlapping pile, then gravity reversals and rim
             * contact. Both implementations rebuild their independent hash
             * scratch on every production constraint pass. */
            for (unsigned i = 0; i < counts[mode]; i++) {
                candidate.game.beads.beads[i] = (orb_game_bead_t){
                    ((float)(i % 16) - 7.5f) * 12.0f,
                    15.0f + (float)(i / 16) * 12.0f,
                    (float)((int)(i % 7) - 3) * 9.0f,
                    (float)((int)(i % 11) - 5) * 4.0f};
            }
        }
        reference = candidate;
        for (unsigned step = 0; step < 900; step++) {
            float x = step < 300 ? 0.5f : step < 600 ? -0.5f : sinf((float)step * 0.03f) * 0.25f;
            float y = step < 600 ? 0.1f : cosf((float)step * 0.07f) * 0.25f;
            orb_game_step(&candidate, x, y, true, false);
            reference_orb_game_step(&reference, x, y, true, false);
            assert(!memcmp(&candidate, &reference, sizeof(candidate)));
            checks++;
        }
    }
    printf("Beads hash: %u complete-state byte comparisons against exhaustive descending bin traversal; 32/128/256 particles, pours/reversals/packed rim\n", checks);
}
"""
    if cell_mode:
        begin = source.index("static unsigned bead_cell_index(float x, float y)")
        end = source.index("\n}", begin) + 2
        constants = "\n".join(
            re.findall(r"^#define BEAD_(?:CELL|GRID)\s+[^\n]+", source, re.MULTILINE)
        )
        assert len(constants.splitlines()) == 2
        mapping = (
            constants
            + "\n"
            + source[begin:end]
            + r"""
static unsigned reference_cell(float x, float y) {
    int cx = (int)floorf((x + 233.0f) / (float)BEAD_CELL);
    int cy = (int)floorf((y + 233.0f) / (float)BEAD_CELL);
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx >= (int)BEAD_GRID) cx = (int)BEAD_GRID - 1;
    if (cy >= (int)BEAD_GRID) cy = (int)BEAD_GRID - 1;
    return (unsigned)(cy * BEAD_GRID + cx);
}
static void check_cells(void) {
    unsigned checks = 0;
    uint32_t rng = 19;
    for (unsigned i = 0; i < 1000000; i++) {
        float position[2];
        for (unsigned axis = 0; axis < 2; axis++) {
            rng = rng * 1664525u + 1013904223u;
            /* Include positions outside the bowl to exercise both clamps. */
            position[axis] = ((float)(rng & 0xffffffu) / 16777215.0f) * 1000.0f - 500.0f;
        }
        assert(bead_cell_index(position[0], position[1]) == reference_cell(position[0], position[1]));
        checks++;
    }
    for (int cell = -1; cell <= (int)BEAD_GRID + 1; cell++) {
        float boundary = (float)(cell * (int)BEAD_CELL) - 233.0f;
        for (unsigned direction = 0; direction < 2; direction++) {
            float position = boundary;
            for (unsigned i = 0; i < 64; i++) {
                assert(bead_cell_index(position, 0) == reference_cell(position, 0));
                assert(bead_cell_index(0, position) == reference_cell(0, position));
                assert(bead_cell_index(position, position) == reference_cell(position, position));
                checks += 3;
                position = nextafterf(position, direction ? INFINITY : -INFINITY);
            }
        }
    }
    /* Every pair of cell centres has a unique row-major bin. */
    for (unsigned y = 0; y < BEAD_GRID; y++) {
        for (unsigned x = 0; x < BEAD_GRID; x++) {
            float px = (float)(x * BEAD_CELL) - 233.0f + BEAD_CELL * .5f;
            float py = (float)(y * BEAD_CELL) - 233.0f + BEAD_CELL * .5f;
            assert(bead_cell_index(px, py) == y * BEAD_GRID + x);
            checks++;
        }
    }
    printf("Beads cells: %u random/clamp/nextafter-boundary/unique-centre checks\n", checks);
}
"""
        )
        # Cell mapping has its own independent oracle. Do not waste another
        # full solver run comparing two identical copies of production code.
        fixture = fixture[: fixture.index("void reference_orb_game_init")]
        fixture += mapping + "\nint main(void) { check_cells(); }\n"
    with tempfile.TemporaryDirectory(prefix="orb-beads-chain-") as temporary:
        directory = Path(temporary)
        original = directory / "reference.c"
        original.write_text(reference)
        harness = directory / "chain.c"
        harness.write_text(fixture)
        common = [
            *shlex.split(os.environ.get("CC", "cc")),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-O2",
            "-fsanitize=address,undefined",
            f"-I{GAMES / 'include'}",
        ]
        if cell_mode:
            objects = []
        else:
            reference_object = directory / "reference.o"
            subprocess.run(
                [
                    *common,
                    *(f"-D{name}=reference_{name}" for name in names),
                    "-c",
                    str(original),
                    "-o",
                    str(reference_object),
                ],
                check=True,
            )
            objects = [
                str(GAMES / "orb_game_logic.c"),
                *(str(path) for path in modules),
                str(reference_object),
            ]
        binary = directory / "chain"
        subprocess.run(
            [*common, str(harness), *objects, "-lm", "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
