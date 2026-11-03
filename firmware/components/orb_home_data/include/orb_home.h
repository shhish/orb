#pragma once

/* Bounded, LVGL-free Home state. The pinned hub owns credentials and aliases;
 * snapshots, staging and pending writes live in PSRAM, never the DMA heap. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ORB_HOME_MAX_ACCESSORIES 64
#define ORB_HOME_MAX_ROOMS 16
#define ORB_HOME_MAX_SCENES 32
#define ORB_HOME_MAX_PENDING 4
#define ORB_HOME_NAME_MAX 32
#define ORB_HOME_ID_MAX 16

typedef enum {
    ORB_HOME_LIGHT, ORB_HOME_SWITCH, ORB_HOME_OUTLET, ORB_HOME_THERMOSTAT,
    ORB_HOME_LOCK, ORB_HOME_FAN, ORB_HOME_TEMPERATURE, ORB_HOME_HUMIDITY,
    ORB_HOME_MOTION, ORB_HOME_CONTACT, ORB_HOME_SENSOR, ORB_HOME_DOORBELL,
    ORB_HOME_BUTTON, ORB_HOME_COVER, ORB_HOME_OTHER, ORB_HOME_UNKNOWN,
} orb_home_kind_t;
typedef enum { ORB_HOME_MODE_OFF, ORB_HOME_MODE_HEAT, ORB_HOME_MODE_COOL, ORB_HOME_MODE_AUTO } orb_home_mode_t;
enum {
    ORB_HOME_ON = 1u << 0, ORB_HOME_BRIGHTNESS = 1u << 1, ORB_HOME_HUE = 1u << 2,
    ORB_HOME_SAT = 1u << 3, ORB_HOME_CT = 1u << 4, ORB_HOME_TEMP = 1u << 5,
    ORB_HOME_TARGET = 1u << 6, ORB_HOME_MODE = 1u << 7, ORB_HOME_HUMIDITY_VALUE = 1u << 8,
    ORB_HOME_MOTION_VALUE = 1u << 9, ORB_HOME_CONTACT_VALUE = 1u << 10,
    ORB_HOME_LOCKED = 1u << 11, ORB_HOME_SPEED = 1u << 12, ORB_HOME_POSITION = 1u << 13,
    ORB_HOME_BATTERY = 1u << 14, ORB_HOME_LOW_BATTERY = 1u << 15,
};
#define ORB_HOME_WRITABLE (ORB_HOME_ON | ORB_HOME_BRIGHTNESS | ORB_HOME_HUE | \
    ORB_HOME_SAT | ORB_HOME_CT | ORB_HOME_TARGET | ORB_HOME_MODE | \
    ORB_HOME_LOCKED | ORB_HOME_SPEED | ORB_HOME_POSITION)
typedef struct {
    uint32_t present;
    bool on, motion, contact, locked, low_battery;
    float brightness, hue, sat, ct, temp, target, humidity, speed, position, battery;
    orb_home_mode_t mode;
} orb_home_state_t;
typedef struct { char id[33], name[33]; } orb_home_room_t;
typedef struct {
    char id[33], name[33], room[33];
    orb_home_kind_t kind;
    bool reachable, favourite, pending;
    uint32_t writable;
    uint64_t updated;
    orb_home_state_t state;
} orb_home_accessory_t;
typedef struct {
    char id[33], name[33];
    bool favourite, contains_unlock; /* Unsafe scenes never offered/executed. */
} orb_home_scene_t;
typedef struct {
    uint64_t revision;
    /* Local intent epoch, never on the wire. A replacement/reconnect can be
     * completed between UI polls; held gestures must still be invalidated. */
    uint64_t generation;
    bool online, synced, available, demo;
    size_t accessory_count, room_count, scene_count;
    orb_home_accessory_t accessories[ORB_HOME_MAX_ACCESSORIES];
    orb_home_room_t rooms[ORB_HOME_MAX_ROOMS];
    orb_home_scene_t scenes[ORB_HOME_MAX_SCENES];
    char error[97], error_id[33], last_scene[33];
    bool scene_pending;
    char room_off_id[33];
    bool room_off_pending;
    uint8_t room_off_total, room_off_completed, room_off_failed;
} orb_home_snapshot_t;
typedef enum { ORB_HOME_REQUEST_SET, ORB_HOME_REQUEST_TOGGLE, ORB_HOME_REQUEST_SCENE } orb_home_request_kind_t;
typedef struct {
    uint32_t sequence;
    orb_home_request_kind_t kind;
    char id[33];
    orb_home_state_t values;
} orb_home_request_t;

bool orb_home_init(void);
void orb_home_reset(void);
uint64_t orb_home_revision(void);
bool orb_home_snapshot(orb_home_snapshot_t *out);
bool orb_home_snapshot_if_changed(uint64_t revision, orb_home_snapshot_t *out);
void orb_home_set_connection(bool online, bool synced);
/* Atomic chunk staging: unknown additive fields are handled by the link parser.
 * A reset starts a snapshot; its last chunk publishes all three arrays at once. */
bool orb_home_snapshot_chunk(const orb_home_room_t *rooms, size_t room_count,
    const orb_home_accessory_t *accessories, size_t accessory_count,
    const orb_home_scene_t *scenes, size_t scene_count, bool reset, bool more);
/* Wire snapshots carry availability on every chunk. A replacement remains
 * read-only until its final chunk, and mixed availability rejects the stage. */
bool orb_home_snapshot_chunk_available(const orb_home_room_t *rooms, size_t room_count,
    const orb_home_accessory_t *accessories, size_t accessory_count,
    const orb_home_scene_t *scenes, size_t scene_count, bool available, bool reset, bool more);
void orb_home_snapshot_discard(void);
bool orb_home_update(const char *id, const orb_home_state_t *patch,
                     bool has_reachable, bool reachable, uint64_t updated);
/* Single pending write per accessory; queue bounded to four. Unlock is allowed
 * only via set(..., deliberate_unlock=true), after the UI's one-second hold. */
bool orb_home_set(const char *id, const orb_home_state_t *values, bool deliberate_unlock);
bool orb_home_toggle(const char *id);
bool orb_home_scene(const char *id);
/* Explicitly request off for reported-on lights, switches and outlets in a
 * room. Four existing request slots drain the batch without retries; progress
 * includes inaccessible targets as failures, never as successful off writes. */
bool orb_home_room_off(const char *room_id);
bool orb_home_take_request(orb_home_request_t *out);
void orb_home_complete(const orb_home_request_t *request,
                       const orb_home_state_t *accepted, const char *error);
/* Home's shared lighting/power summary includes bulbs, switches and outlets.
 * Climate (fan/thermostat) and security state have separate readouts. */
bool orb_home_summary_accessory(const orb_home_accessory_t *accessory);
size_t orb_home_on_count(const orb_home_snapshot_t *snapshot);
/* Allocation-free public glance summary. False while unavailable/stale; never
 * copies accessory names, values or the complete Home snapshot to the shell. */
bool orb_home_glance_count(size_t *on_count);
const orb_home_accessory_t *orb_home_find(const orb_home_snapshot_t *snapshot, const char *id);
void orb_home_load_demo(void); /* Explicit simulator/demo fixture, never live. */
size_t orb_home_memory_bytes(void); /* Exact live retained-store payload. */
