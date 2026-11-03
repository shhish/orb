/* Host tests: gravity to light and parallax. */
#include "check.h"
#include "orb_motion_math.h"

static void test_lowpass(void)
{
    orb_vec3_t s = { 0, 0, 0 };
    for (int i = 0; i < 100; i++) {
        orb_motion_lowpass(&s, (orb_vec3_t){ 1, -1, 0.5f }, 0.02f, 0.15f);
    }
    CHECK(NEAR(s.x, 1, 1e-3f) && NEAR(s.y, -1, 1e-3f) && NEAR(s.z, 0.5f, 1e-3f));
    /* One step of a tau-long filter moves about dt / tau of the way. */
    orb_vec3_t t = { 0, 0, 0 };
    orb_motion_lowpass(&t, (orb_vec3_t){ 1, 0, 0 }, 0.015f, 0.15f);
    CHECK(t.x > 0.08f && t.x < 0.1f);
}

static void test_light(void)
{
    float deg, amt;
    /* Upright: up is +y, so 12 o'clock (270), full. */
    orb_motion_light((orb_vec3_t){ 0, 1, 0 }, 30, &deg, &amt);
    CHECK(NEAR(deg, 270, 1e-3f) && NEAR(amt, 1, 1e-4f));
    /* Flat on its back: no lean. */
    orb_motion_light((orb_vec3_t){ 0, 0, 1 }, 30, &deg, &amt);
    CHECK(NEAR(amt, 0, 1e-4f));
    /* Tipped 20 degrees to the right (its right edge down): up drifts to the left, anticlockwise. */
    orb_motion_light((orb_vec3_t){ -0.342f, 0.940f, 0 }, 30, &deg, &amt);
    CHECK(NEAR(deg, 250, 0.1f));
    /* On its side: kept within 30 degrees of the top. */
    orb_motion_light((orb_vec3_t){ -1, 0, 0 }, 30, &deg, &amt);
    CHECK(NEAR(deg, 240, 1e-3f));
    orb_motion_light((orb_vec3_t){ 1, 0, 0 }, 30, &deg, &amt);
    CHECK(NEAR(deg, 300, 1e-3f));
}

static void test_parallax(void)
{
    int x = 0, y = 0;
    /* At rest: nothing moves. */
    CHECK(!orb_motion_parallax((orb_vec3_t){ 0, 0, 0 }, 1, 8, &x, &y));
    /* Right edge dips 20 degrees: the far layer shifts its full 8 px right, downhill. */
    CHECK(orb_motion_parallax((orb_vec3_t){ -0.35f, 0, 0 }, 1, 8, &x, &y));
    CHECK(x == 8 && y == 0);
    /* Noise under the hysteresis doesn't redraw. */
    CHECK(!orb_motion_parallax((orb_vec3_t){ -0.33f, 0.01f, 0 }, 1, 8, &x, &y));
    /* Nearer layers move less, and never past the limit. */
    int nx = 0, ny = 0;
    orb_motion_parallax((orb_vec3_t){ 0, -2, 0 }, 0.25f, 8, &nx, &ny);
    CHECK(nx == 0 && ny == -2);
}

int main(void)
{
    test_lowpass();
    test_light();
    test_parallax();
    return finish("orb_ui motion");
}
