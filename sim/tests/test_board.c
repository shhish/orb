/* Host tests: the fake board's tilt read the way orb_ui reads the real sensor, the round mask, the pixel format. */
#include "check.h"
#include "orb_motion_math.h"
#include "sim_board.h"

#define D 466

static orb_vec3_t gravity(float right, float down)
{
    float g[3];
    sim_board_gravity(right, down, g);
    return (orb_vec3_t){ g[0], g[1], g[2] };
}

static void test_gravity(void)
{
    orb_vec3_t flat = gravity(0, 0);
    CHECK(NEAR(flat.x, 0, 1e-6f) && NEAR(flat.y, 0, 1e-6f) && NEAR(flat.z, 1, 1e-6f));
    for (float r = -45; r <= 45; r += 15) {
        for (float d = -45; d <= 45; d += 15) {
            orb_vec3_t g = gravity(r, d);
            CHECK(NEAR(g.x * g.x + g.y * g.y + g.z * g.z, 1, 1e-4f));
        }
    }
}

/* Each dipped edge as orb_ui sees it: the light comes from the raised side and the content slides downhill. */
static void test_tilt_reads_right(void)
{
    orb_vec3_t flat = gravity(0, 0);
    struct {
        float right, down, light;
        int dx, dy;   /* sign of the content's shift */
    } cases[] = {
        { 25, 0, 240, 1, 0 },    /* right edge down: highlight swings left, content right */
        { -25, 0, 300, -1, 0 },  /* left edge down */
        { 0, 25, 270, 0, 1 },    /* bottom edge down, as when held to read: light at 12, content down */
        { 0, -25, 270, 0, -1 },  /* top edge down: up points at the bottom, the light stays clamped near 12 */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        orb_vec3_t g = gravity(cases[i].right, cases[i].down);
        float deg, amount;
        orb_motion_light(g, 30, &deg, &amount);
        if (cases[i].down >= 0) {
            CHECK(NEAR(deg, cases[i].light, 0.5f));
        } else {
            CHECK(NEAR(deg, 240, 0.5f) || NEAR(deg, 300, 0.5f));
        }
        CHECK(NEAR(amount, sinf(25 * (float)M_PI / 180), 1e-3f));
        int x = 0, y = 0;
        orb_motion_parallax((orb_vec3_t){ g.x - flat.x, g.y - flat.y, g.z - flat.z }, 1, 8, &x, &y);
        CHECK((x > 0) - (x < 0) == cases[i].dx && (y > 0) - (y < 0) == cases[i].dy);
    }
}

static void test_cover(void)
{
    CHECK(sim_board_cover(D / 2, D / 2, D) == 255);
    CHECK(sim_board_cover(0, 0, D) == 0 && sim_board_cover(D - 1, D - 1, D) == 0);
    /* The top row's middle pixel touches the rim; the one above the circle's widest row is fully in. */
    CHECK(sim_board_cover(D / 2, 0, D) > 0);
    CHECK(sim_board_cover(0, D / 2, D) > 0 && sim_board_cover(1, D / 2, D) == 255);
    double area = 0;
    int partial = 0;
    for (int y = 0; y < D; y++) {
        for (int x = 0; x < D; x++) {
            uint8_t c = sim_board_cover(x, y, D);
            CHECK(c == sim_board_cover(D - 1 - x, y, D) && c == sim_board_cover(x, D - 1 - y, D));
            float dx = x + .5f - D / 2.f, dy = y + .5f - D / 2.f;
            if (c) CHECK(dx * dx + dy * dy <= D * D / 4.f);
            area += c / 255.0;
            partial += c > 0 && c < 255;
        }
    }
    /* Anti-aliased: the shown area is the circle's to within a tenth of a percent, the edge one pixel wide. */
    CHECK(fabs(area - M_PI * D * D / 4) < M_PI * D * D / 4 * 1e-3);
    CHECK(partial > D && partial < D * 2); /* Only the interior half of the AA rim is visible. */
}

static void test_rgb(void)
{
    /* RGB565 with the high byte first. */
    const uint8_t px[] = { 0xF8, 0x00, 0x07, 0xE0, 0x00, 0x1F, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF };
    const uint8_t cover[] = { 255, 255, 255, 255, 255, 128 };
    uint8_t rgb[18];
    sim_board_to_rgb(px, cover, rgb, 6);
    const uint8_t want[] = { 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 128, 128, 128 };
    for (int i = 0; i < 18; i++) {
        CHECK(rgb[i] == want[i]);
    }
    sim_board_to_rgb(px + 2, NULL, rgb, 1);
    CHECK(rgb[0] == 0 && rgb[1] == 255 && rgb[2] == 0);
}

int main(void)
{
    test_gravity();
    test_tilt_reads_right();
    test_cover();
    test_rgb();
    return finish("test_board");
}
