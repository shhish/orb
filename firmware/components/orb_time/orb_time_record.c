#include "orb_time_internal.h"

#include <limits.h>
#include <string.h>

/* Fixed wire fields avoid persisting ABI padding, time_t width or enum size.
 * Atomic LittleFS replacement yields either the prior record or the new one;
 * a checksum and field validation reject damaged/truncated storage safely. */

#if defined(ESP_PLATFORM) || defined(ORB_TIME_TEST_STORAGE)
void orb_time_put32(unsigned char *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) out[i] = (unsigned char)(value >> (i * 8));
}
uint32_t orb_time_get32(const unsigned char *in)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= (uint32_t)in[i] << (i * 8);
    return value;
}
void orb_time_put64(unsigned char *out, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) out[i] = (unsigned char)(value >> (i * 8));
}
uint64_t orb_time_get64(const unsigned char *in)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)in[i] << (i * 8);
    return value;
}
uint32_t orb_time_checksum(const unsigned char *record)
{
    uint32_t value = UINT32_C(2166136261);
    for (unsigned i = 0; i < RECORD_SIZE - 4; ++i) value = (value ^ record[i]) * UINT32_C(16777619);
    return value;
}
bool orb_time_record_valid(const unsigned char *record)
{
    if (memcmp(record, "ORBT", 4) || record[4] != 1 || record[5] > 23 || record[6] > 59 ||
        record[7] > ORB_TIME_REPEAT_CUSTOM || record[8] != orb_time_repeat_days((orb_time_repeat_t)record[7], record[8]) ||
        (record[7] == ORB_TIME_REPEAT_CUSTOM && (!record[8] || (record[8] & ~ORB_TIME_DAYS_ALL))) ||
        (record[9] & ~31u) || record[10] > ORB_TIME_REMINDERS_MAX || record[11] ||
        orb_time_get32(record + 12) < 60 || orb_time_get32(record + 12) > 3600 ||
        orb_time_get32(record + 16) > ORB_TIME_TIMER_MAX_SECONDS || !orb_time_get32(record + 20) ||
        orb_time_get64(record + 24) > INT64_MAX || orb_time_get32(record + RECORD_SIZE - 4) != orb_time_checksum(record)) return false;
    for (unsigned i = 0; i < record[10]; ++i) {
        const unsigned char *r = record + RECORD_HEADER + i * RECORD_REMINDER;
        if (!orb_time_get32(r) || orb_time_get64(r + 4) > INT64_MAX || !orb_time_clock_valid((time_t)orb_time_get64(r + 4)) ||
            !r[12] || !memchr(r + 12, 0, 64) || (r[76] & ~3u)) return false;
        for (unsigned j = 0; j < i; ++j)
            if (orb_time_get32(record + RECORD_HEADER + j * RECORD_REMINDER) == orb_time_get32(r)) return false;
    }
    return true;
}
#endif

