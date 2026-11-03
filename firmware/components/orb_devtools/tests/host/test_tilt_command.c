#include <stdlib.h>
#include "orb_tilt_command.h"

static unsigned s_checks;
#define CHECK(test) do { s_checks++; if (!(test)) { fprintf(stderr, "line %d: %s\n", __LINE__, #test); exit(1); } } while (0)

int main(void)
{
    orb_devtools_tilt_t tilt;
    orb_devtools_tilt_override_t state = {0};
    float x = 9, y = 9, z = 9;
    CHECK(!orb_devtools_tilt_sample(&state, 100, &x, &y, &z));
    CHECK(x == 9 && y == 9 && z == 9); /* Inactive overrides leave real data alone. */
    CHECK(orb_devtools_tilt_command("tilt -0.25 0 5000", &tilt));
    CHECK(tilt.x == -0.25f && tilt.y == 0 && tilt.duration_ms == 5000);
    orb_devtools_tilt_begin(&state, &tilt, 1000000);
    CHECK(orb_devtools_tilt_sample(&state, 1000000, &x, &y, &z));
    CHECK(x == -0.25f && y == 0 && fabsf(x*x + y*y + z*z - 1) < .00001f && z > 0);
    CHECK(orb_devtools_tilt_sample(&state, 5999999, &x, &y, &z));
    CHECK(!orb_devtools_tilt_sample(&state, 6000000, &x, &y, &z));
    CHECK(!orb_devtools_tilt_sample(&state, 7000000, &x, &y, &z));
    CHECK(orb_devtools_tilt_command("tilt 1 0 1", &tilt));
    orb_devtools_tilt_begin(&state, &tilt, 8000000);
    CHECK(orb_devtools_tilt_sample(&state, 8000000, &x, &y, &z) && x == 1 && z == 0);
    CHECK(orb_devtools_tilt_command("tilt 0 0.5 200", &tilt));
    orb_devtools_tilt_begin(&state, &tilt, 8000001); /* Last command replaces, never stacks. */
    CHECK(orb_devtools_tilt_sample(&state, 8001001, &x, &y, &z) && x == 0 && y == .5f);
    CHECK(!orb_devtools_tilt_sample(&state, 8200001, &x, &y, &z));
    CHECK(!orb_devtools_tilt_sample(&state, 8000001, &x, &y, &z)); /* Expiry cannot revive. */
    const char *const invalid[] = {
        "tilt", "tilt0 0 100", "tilt 0 0", "tilt 0 0 100 extra", "tilt 0 0 100\nheap",
        "tilt nan 0 100", "tilt 0 inf 100", "tilt 1e999 0 100", "tilt 1.01 0 100",
        "tilt 0 -1.01 100", "tilt 0.8 0.8 100", "tilt 0 0 0", "tilt 0 0 5001",
        "tilt 0 0 -4294967295", "tilt 0 0 1.5", "tilt 0 0 nan", "tilt 0 0 1e999",
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        CHECK(!orb_devtools_tilt_command(invalid[i], &tilt));
    CHECK(!orb_devtools_tilt_command(NULL, &tilt));
    CHECK(!orb_devtools_tilt_command("tilt 0 0 100", NULL));
    printf("tilt command: %u checks passed\n", s_checks);
}
