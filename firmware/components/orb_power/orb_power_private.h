#pragma once
#include "orb_power.h"
void orb_power_changed(orb_power_controller_t *power);
void orb_power_enter_sleep(orb_power_controller_t *power, bool immediate);
unsigned orb_power_motion_sample(orb_power_motion_t *motion, float x, float y, float z, uint64_t now_ms);
enum { ORB_POWER_MOTION_INVALID = 1, ORB_POWER_MOTION_PICKUP = 2, ORB_POWER_MOTION_DOWN = 4, ORB_POWER_MOTION_UP = 8 };
