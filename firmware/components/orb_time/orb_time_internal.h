#pragma once

#include "orb_time.h"

void orb_time_copy_utf8(char *out, size_t capacity, const char *text);

/* Pure calendar and record helpers shared only by the service implementation. */
bool orb_time_clock_valid(time_t now);
uint8_t orb_time_repeat_days(orb_time_repeat_t repeat, uint8_t custom);
time_t orb_time_civil_alarm(int day, unsigned hour, unsigned minute);
time_t orb_time_next_alarm(time_t wall_now, unsigned hour, unsigned minute,
                           uint8_t days, int last_day, int *out_day);

#define RECORD_HEADER 40u
#define RECORD_REMINDER 80u
#define RECORD_SIZE (RECORD_HEADER + ORB_TIME_REMINDERS_MAX * RECORD_REMINDER + 4u)

#if defined(ESP_PLATFORM) || defined(ORB_TIME_TEST_STORAGE)
void orb_time_put32(unsigned char *out, uint32_t value);
uint32_t orb_time_get32(const unsigned char *in);
void orb_time_put64(unsigned char *out, uint64_t value);
uint64_t orb_time_get64(const unsigned char *in);
uint32_t orb_time_checksum(const unsigned char *record);
bool orb_time_record_valid(const unsigned char *record);
#endif
