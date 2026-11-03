#include "orb_game_logic.h"

#include <math.h>
#include <string.h>

static uint32_t keepsake_checksum(const uint8_t *bytes)
{
    uint32_t checksum = 0x4f474b31u;
    for (size_t i = 0; i < ORB_KEEPSAKE_RECORD_SIZE - 4; ++i) {
        checksum = (checksum << 5) | (checksum >> 27);
        checksum ^= bytes[i];
    }
    return checksum;
}

static bool keepsake_valid(const orb_keepsake_record_t *rec)
{
    if (memcmp(rec->magic, ORB_KEEPSAKE_MAGIC, 4) || rec->version != 1 || rec->reserved ||
        rec->count > ORB_MARBLE_KEEPSAKES_MAX || rec->stages_cleared > ORB_MARBLE_STAGES) return false;
    for (unsigned i = 0; i < rec->count; ++i) {
        if (rec->items[i].kind > 2 || !rec->items[i].stage || rec->items[i].stage > ORB_MARBLE_STAGES ||
            rec->items[i].stage > rec->stages_cleared ||
            hypotf((float)rec->items[i].x, (float)rec->items[i].y) > 110) return false;
    }
    return true;
}

size_t orb_keepsake_encode(const orb_keepsake_record_t *rec, uint8_t *out, size_t capacity)
{
    if (!rec || !out || capacity < ORB_KEEPSAKE_RECORD_SIZE || !keepsake_valid(rec)) return 0;
    memset(out, 0, ORB_KEEPSAKE_RECORD_SIZE);
    memcpy(out, ORB_KEEPSAKE_MAGIC, 4);
    out[4] = 1; out[5] = rec->count; out[6] = rec->stages_cleared;
    for (unsigned i = 0; i < rec->count; ++i) {
        unsigned at = 8 + i * 6;
        uint16_t x = (uint16_t)rec->items[i].x, y = (uint16_t)rec->items[i].y;
        out[at] = rec->items[i].kind; out[at + 1] = rec->items[i].stage;
        out[at + 2] = (uint8_t)x; out[at + 3] = (uint8_t)(x >> 8);
        out[at + 4] = (uint8_t)y; out[at + 5] = (uint8_t)(y >> 8);
    }
    uint32_t checksum = keepsake_checksum(out);
    for (unsigned i = 0; i < 4; ++i) out[ORB_KEEPSAKE_RECORD_SIZE - 4 + i] = (uint8_t)(checksum >> (8 * i));
    return ORB_KEEPSAKE_RECORD_SIZE;
}

bool orb_keepsake_decode(const uint8_t *in, size_t size, orb_keepsake_record_t *out)
{
    if (!in || !out || size != ORB_KEEPSAKE_RECORD_SIZE) return false;
    uint32_t stored = 0;
    for (unsigned i = 0; i < 4; ++i) stored |= (uint32_t)in[ORB_KEEPSAKE_RECORD_SIZE - 4 + i] << (8 * i);
    if (stored != keepsake_checksum(in)) return false;
    orb_keepsake_record_t rec = {0};
    memcpy(rec.magic, in, 4);
    rec.version = in[4]; rec.count = in[5]; rec.stages_cleared = in[6]; rec.reserved = in[7];
    if (rec.count > ORB_MARBLE_KEEPSAKES_MAX) return false;
    for (unsigned i = 0; i < ORB_MARBLE_KEEPSAKES_MAX; ++i) {
        unsigned at = 8 + i * 6;
        if (i >= rec.count) {
            for (unsigned j = 0; j < 6; ++j) if (in[at + j]) return false;
            continue;
        }
        rec.items[i].kind = in[at]; rec.items[i].stage = in[at + 1];
        uint16_t x = (uint16_t)(in[at + 2] | ((uint16_t)in[at + 3] << 8));
        uint16_t y = (uint16_t)(in[at + 4] | ((uint16_t)in[at + 5] << 8));
        rec.items[i].x = (int16_t)(x < 32768u ? (int32_t)x : (int32_t)x - 65536);
        rec.items[i].y = (int16_t)(y < 32768u ? (int32_t)y : (int32_t)y - 65536);
    }
    if (!keepsake_valid(&rec)) return false;
    rec.checksum = stored;
    *out = rec;
    return true;
}
