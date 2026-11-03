/* Host tests: QMI8658 samples to g, and sensor axes to screen axes. */
#include <math.h>
#include <stdio.h>

#include "orb_imu.h"

static int s_failed;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_failed++;                                                 \
        }                                                               \
    } while (0)

#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)

static void test_decode(void)
{
    /* x = +8192 (1 g at +-4 g), y = -8192, z = -32768 (-4 g): little-endian int16. */
    const uint8_t raw[6] = { 0x00, 0x20, 0x00, 0xE0, 0x00, 0x80 };
    float g[3];
    orb_imu_decode(raw, ORB_IMU_LSB_PER_G_4G, g);
    CHECK(NEAR(g[0], 1.0f));
    CHECK(NEAR(g[1], -1.0f));
    CHECK(NEAR(g[2], -4.0f));

    /* 0.46 g, as the sensor read lying flat with stale state left in it, is 3768 LSB at +-4 g. */
    const uint8_t flat[6] = { 0, 0, 0, 0, 0xB8, 0x0E };
    orb_imu_decode(flat, ORB_IMU_LSB_PER_G_4G, g);
    CHECK(NEAR(g[2], 3768 / 8192.0f));
}

static void test_axes(void)
{
    /* Half a turn about z: x and y flip, z (out of the screen) stays. */
    float s[3] = { 0.3f, -0.8f, 0.5f }, out[3];
    orb_imu_to_screen(s, out);
    CHECK(NEAR(out[0], -0.3f));
    CHECK(NEAR(out[1], 0.8f));
    CHECK(NEAR(out[2], 0.5f));
}

static void test_physical_specific_force(void)
{
    /* Synthetic QMI8658 little-endian samples, not downhill gravity. Each
     * pose goes through both production decode and the mounting rotation. */
    static const struct {
        uint8_t raw[6];
        float x, y, z;
    } poses[] = {
        /* Face up: the supporting force is out of the screen. */
        {{0, 0, 0, 0, 0, 0x20}, 0, 0, 1},
        /* Upright: screen up points away from Earth. */
        {{0, 0, 0, 0xe0, 0, 0}, 0, 1, 0},
        /* Right side down by 30 degrees: screen ax < 0, downhill right. */
        {{0, 0x10, 0, 0, 0xb6, 0x1b}, -0.5f, 0, 7094 / 8192.0f},
        /* Bottom side down by 30 degrees: ay > 0, downhill screen-down. */
        {{0, 0, 0, 0xf0, 0xb6, 0x1b}, 0, 0.5f, 7094 / 8192.0f},
    };
    for (unsigned i = 0; i < sizeof(poses) / sizeof(poses[0]); ++i) {
        float sensor[3], screen[3];
        orb_imu_decode(poses[i].raw, ORB_IMU_LSB_PER_G_4G, sensor);
        orb_imu_to_screen(sensor, screen);
        CHECK(NEAR(screen[0], poses[i].x));
        CHECK(NEAR(screen[1], poses[i].y));
        CHECK(NEAR(screen[2], poses[i].z));
    }
}

int main(void)
{
    test_decode();
    test_axes();
    test_physical_specific_force();
    if (s_failed) {
        fprintf(stderr, "%d check(s) failed\n", s_failed);
        return 1;
    }
    printf("orb_board IMU: all tests passed\n");
    return 0;
}
