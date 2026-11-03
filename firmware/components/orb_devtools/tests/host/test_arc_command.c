#include <stdlib.h>
#include "orb_arc_command.h"

static unsigned s_checks;
#define CHECK(test) do { s_checks++; if (!(test)) { fprintf(stderr, "line %d: %s\n", __LINE__, #test); exit(1); } } while (0)

static void point(const orb_devtools_arc_t *arc, float fraction, int expected_x, int expected_y)
{
    int x, y;
    orb_devtools_arc_point(arc, fraction, &x, &y);
    CHECK(x == expected_x);
    CHECK(y == expected_y);
}

int main(void)
{
    orb_devtools_arc_t arc;
    CHECK(orb_devtools_arc_command("arc 200 0 360 600", &arc));
    CHECK(arc.duration_ms == 600);
    point(&arc, 0, 233, 33);
    point(&arc, 0.25f, 433, 233);
    point(&arc, 0.5f, 233, 433);
    point(&arc, 0.75f, 33, 233);
    point(&arc, 1, 233, 33);
    CHECK(orb_devtools_arc_command("arc 200 360 0 600", &arc));
    point(&arc, 0.25f, 33, 233);
    CHECK(orb_devtools_arc_command("arc 232 350 370 400", &arc));
    point(&arc, 0.5f, 233, 1);
    int x0, y0, x1, y1;
    orb_devtools_arc_point(&arc, 0, &x0, &y0);
    orb_devtools_arc_point(&arc, 1, &x1, &y1);
    CHECK(x0 < 233 && x1 > 233 && y0 == y1);
    CHECK(orb_devtools_arc_command("arc 232 10 -10 400", &arc));
    point(&arc, 0.5f, 233, 1);
    /* Dense samples ensure the arc stays on its requested radius throughout,
     * rather than cutting a chord like the straight swipe command. */
    CHECK(orb_devtools_arc_command("arc 232 -3600 3600 5000", &arc));
    for (unsigned i = 0; i <= 500; i++) {
        int x, y;
        orb_devtools_arc_point(&arc, (float)i / 500, &x, &y);
        CHECK(x >= 0 && x <= 465 && y >= 0 && y <= 465);
        CHECK(fabsf(hypotf(x - 233, y - 233) - 232) < 0.71f);
    }
    const char *const invalid[] = {
        "arc", "arc200 0 90 500", "arc 200 0 90", "arc 200 0 90 500 extra", "arc 0 0 90 500",
        "arc 233 0 90 500", "arc 200 -3601 90 500", "arc 200 0 3601 500",
        "arc 200 0 90 0", "arc 200 0 90 5001", "arc 200.5 0 90 500",
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        CHECK(!orb_devtools_arc_command(invalid[i], &arc));
    CHECK(!orb_devtools_arc_command(NULL, &arc));
    CHECK(!orb_devtools_arc_command("arc 200 0 90 500", NULL));
    printf("arc command: %u checks passed\n", s_checks);
}
