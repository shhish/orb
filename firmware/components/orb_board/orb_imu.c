#include "orb_imu.h"

void orb_imu_decode(const uint8_t raw[6], float lsb_per_g, float g[3])
{
    for (int i = 0; i < 3; i++) {
        g[i] = (int16_t)(raw[2 * i + 1] << 8 | raw[2 * i]) / lsb_per_g;
    }
}

void orb_imu_to_screen(const float sensor[3], float screen[3])
{
    /* Half-turn mounting rotation, matching the panel/touch orientation.
     * These remain specific-force readings: face up is +z, upright is +y.
     * Glass uses this up vector for light from above. Downhill game gravity
     * must oppose x/y at the consumer, never by flipping this shared map. */
    screen[0] = -sensor[0];
    screen[1] = -sensor[1];
    screen[2] = sensor[2];
}
