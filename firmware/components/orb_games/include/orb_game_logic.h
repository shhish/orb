#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* HIG §3: the field follows the panel, inset 18 px from its circular bezel.
 * Chrome is a separate layer; moving sprites never dirty the whole field. */
#define ORB_GAME_CX 233.0f
#define ORB_GAME_CY 233.0f
#define ORB_GAME_RADIUS 215.0f
#define ORB_MARBLE_RADIUS 223.0f /* The physical bezel, inset by HIG §3's 10 px rim. */
#define ORB_MARBLE_BALL_RADIUS 12u
#define ORB_MARBLE_FLOWER_RADIUS 21u
#define ORB_MARBLE_MAX_SPEED 210.0f
#define ORB_GAME_ORBIT_INNER (ORB_GAME_RADIUS - 47.0f)
#define ORB_GAME_ORBIT_OUTER (ORB_GAME_RADIUS - 13.0f)
/* Design 1.0 Orbit: three thin rings, a white sun, an indigo planet that taps
 * between rings, orange comets and yellow stars to collect. */
#define ORB_GAME_ORBIT_RINGS 3u
#define ORB_GAME_ORBIT_R0 80.0f
#define ORB_GAME_ORBIT_R1 136.0f
#define ORB_GAME_ORBIT_R2 192.0f
#define ORB_GAME_ORBIT_SUN 22u
#define ORB_GAME_ORBIT_PLANET 14u
#define ORB_GAME_ORBIT_COMET 8u
#define ORB_GAME_ORBIT_STAR 14u
#define ORB_GAME_ORBIT_STARS 3u
#define ORB_GAME_ORBIT_HIT_GRACE 48u /* 0.8 s to recover and choose another lane. */
#define ORB_GAME_SNAKE_HEAD_RADIUS 11u
#define ORB_GAME_SNAKE_BODY_RADIUS 9u
#define ORB_GAME_SNAKE_SPACING 12.0f
#define ORB_GAME_SNAKE_TURN_RATE 3.2f /* radians/s, independent of tilt strength */
#define ORB_GAME_SNAKE_GRACE 60u
/* The shared top score disc (Pop, Snake): centre offset and radius, panel px.
 * Snake pellets never spawn under it; the view places it here. */
#define ORB_GAME_HUD_X 0.0f
#define ORB_GAME_HUD_Y (-153.0f)
#define ORB_GAME_HUD_RADIUS 40.0f
#define ORB_GAME_STEP_US 16667u
#define ORB_GAME_MAX_SPRITES 288u /* 256 beads + 11 shared effect slots + headroom. */
/* A collision shell larger than the ink leaves separate beads even in a pile.
 * HIG §§1/5: flat individual tones, never a screen-wide gradient or glow. */
#define ORB_GAME_BEAD_COUNT 256u
#define ORB_GAME_BEAD_RADIUS 7u
#define ORB_GAME_BEAD_DRAW_RADIUS 5u
#define ORB_GAME_BEAD_SETTLE_STEPS 72u /* 1.2 s of sampled rest, then <=0.75 s travel. */
#define ORB_GAME_PARTICLES 10u
#define ORB_GAME_SNAKE_MAX 48u
#define ORB_GAME_BUBBLES 8u
#define ORB_GAME_OBSTACLES 4u
#define ORB_GAME_POP_ROUND_STEPS 2700u /* 45 s countdown on the rim timer. */
#define ORB_MARBLE_HISTORY_MAX 300u
#define ORB_MARBLE_GATES_MAX 3u
#define ORB_MARBLE_GATE_SEGMENTS 8u
#define ORB_MARBLE_GATE_WIDTH 12u
#define ORB_MARBLE_KEEPSAKES_MAX 12u
#define ORB_MARBLE_STAGES 3u
#define ORB_KEEPSAKE_MAGIC "OGK1"
#define ORB_KEEPSAKE_RECORD_SIZE 84u

typedef enum {
    ORB_GAME_MARBLE,
    ORB_GAME_BEADS,
    ORB_GAME_ORBIT,
    ORB_GAME_SNAKE,
    ORB_GAME_POP,
    ORB_GAME_COUNT,
} orb_game_kind_t;

typedef enum {
    ORB_GAME_PRIMARY,
    ORB_GAME_LEFT,
    ORB_GAME_RIGHT,
    ORB_GAME_UP,
    ORB_GAME_DOWN,
    ORB_GAME_TAP,
    ORB_GAME_RESTART,
    ORB_GAME_START,
    ORB_GAME_PAUSE,
    ORB_GAME_CONTROLS,
    ORB_GAME_FINISH, /* Explicit End Round, accepted only from user pause. */
    ORB_GAME_REWIND, /* Begin rewind; START/STOP delimit continuous contact. */
    ORB_GAME_REWIND_START, /* Marble Conservatory: begin continuous rewind */
    ORB_GAME_REWIND_STOP,  /* Marble Conservatory: end continuous rewind */
    ORB_GAME_GARDEN,       /* Start/result: view the persistent keepsakes. */
    ORB_GAME_STEER_START,  /* Snake: current held finger position, absolute panel px. */
    ORB_GAME_STEER_STOP,   /* Snake: release/cancel, including paused contacts. */
} orb_game_action_t;

typedef enum {
    ORB_GAME_DISC,
    ORB_GAME_RING,
    ORB_GAME_SQUARE,
    ORB_GAME_DIAMOND,
    ORB_GAME_MARBLE_SPRITE,
    ORB_GAME_CAPSULE,
    ORB_GAME_HEAD,
    ORB_GAME_BUBBLE,
    ORB_GAME_PARTICLE,
    ORB_GAME_GATE,
    ORB_GAME_FLOWER,
    ORB_GAME_STONE,
    ORB_GAME_STAR,
    ORB_GAME_SPARK,
    ORB_GAME_BEAD,
} orb_game_shape_t;

typedef struct {
    float x, y;
    uint16_t radius;
    uint32_t colour;
    uint8_t shape;
    uint8_t opacity;
    int8_t light_x, light_y;
    /* Gates are continuous round-capped arcs, in radians. Their sprite radius
     * is half the stroke width; damage uses arc extrema rather than a disc. */
    float arc_radius, arc_start, arc_end;
} orb_game_sprite_t;

typedef struct { float x, y; } orb_game_point_t;
/* Game-only neutral frame. The shared IMU/parallax convention stays physical:
 * measured specific force is +z face up, +y upright, opposite downhill. */
typedef struct {
    float sum[3], right[3], up[3];
    float neutral[3], pose_mean[3], pose_rate[3], coherent_rate[3], gyro_mean[3];
    float pose_variance, gyro_variance;
    float filtered_x, filtered_y;
    float previous_x, previous_y, derivative_x, derivative_y;
    uint32_t average_us, still_us;
    bool ready, observed, moving, gyro_observed;
} orb_game_tilt_t;
typedef struct { float angle; uint8_t ring; bool passed; } orb_game_obstacle_t;
typedef struct { float angle; uint8_t ring; bool active; } orb_game_star_t;
typedef struct { float x, y, vx, vy; uint8_t radius; bool active; } orb_game_bubble_t;
typedef struct {
    float x, y;
    uint32_t colour;
    uint8_t age, duration, count, ring_radius;
} orb_game_effect_t;

/* A single Beads particle. Positions are screen-space offsets from the panel
 * centre in pixels, exactly like every other sprite; velocities are px/s. */
typedef struct {
    float x, y, vx, vy;
} orb_game_bead_t;

/* Compact 8-byte path sample: ten seconds at 30 Hz, independent of rendering. */
typedef struct {
    int16_t x, y;
    int16_t vx, vy;
} orb_marble_sample_t;

typedef struct {
    float radius;
    float angle;
    float speed;
    float gap_angle;
    bool active;
} orb_marble_gate_t;

typedef struct {
    uint8_t kind;       /* 0: flat stone, 1/2: original flat blossoms */
    uint8_t stage;      /* 1, 2, 3 */
    int16_t x, y;       /* Position in circular garden */
} orb_marble_keepsake_t;

typedef struct {
    char magic[4];          /* "OGK1" */
    uint8_t version;        /* 1 */
    uint8_t count;          /* 0..ORB_MARBLE_KEEPSAKES_MAX */
    uint8_t stages_cleared; /* 0..3 */
    uint8_t reserved;
    struct {
        uint8_t kind;
        uint8_t stage;
        int16_t x, y;
    } items[ORB_MARBLE_KEEPSAKES_MAX];
    uint32_t checksum;
} orb_keepsake_record_t;

typedef struct {
    orb_game_kind_t kind;
    uint32_t score, combo, ticks, random;
    uint8_t lives;
    bool game_over, started, user_paused, controls, reduce_motion;
    bool touch_fallback_hint;
    bool tilt_calibrating, motion_unavailable, motion_moving;
    int8_t light_x, light_y;
    orb_game_effect_t effect;
    union {
        struct {
            float x, y, vx, vy;
            uint8_t stage;
            uint8_t stage_count;
            bool stage_cleared;
            bool garden_open;
            uint8_t stages_cleared;
            bool rewind_active;
            uint16_t rewind_ticks;
            uint16_t history_head;
            uint16_t history_count;
            uint8_t sample_tick;
            uint8_t keepsake_count;
            orb_marble_gate_t gates[ORB_MARBLE_GATES_MAX];
            orb_marble_keepsake_t keepsakes[ORB_MARBLE_KEEPSAKES_MAX];
            orb_game_point_t trail[4];
            uint8_t trail_count;
            /* Last member: runtime publishes the small prefix, never path data. */
            orb_marble_sample_t history[ORB_MARBLE_HISTORY_MAX];
        } marble;
        struct {
            orb_game_bead_t beads[ORB_GAME_BEAD_COUNT];
            /* Fluid integration and clock placement never run together.
             * Share their PSRAM storage instead of adding internal BSS or a
             * frame allocation; entering clock mode rebuilds every target. */
            union {
                struct {
                    int16_t target_x[ORB_GAME_BEAD_COUNT];
                    int16_t target_y[ORB_GAME_BEAD_COUNT];
                    uint8_t target_opacity[ORB_GAME_BEAD_COUNT];
                };
                orb_game_point_t previous[ORB_GAME_BEAD_COUNT];
            };
            uint8_t hour, minute;   /* Current time the beads settle into. */
            uint16_t settle_ticks;  /* Real sampled still time, in step units (capped). */
            uint16_t count, digit_count; /* Live beads and bright digit sites. */
            uint8_t travel_ticks;    /* Snap the hold after finite spring travel. */
            bool settled;           /* Beads currently hold the time digits. */
            bool time_valid;         /* No made-up clock before wall time is available. */
            float flow_speed;        /* RMS free-flow px/s; zero for the held clock. */
            float touch_x, touch_y;  /* Screen offsets; centre is level. */
            uint16_t touch_quiet_ticks;
            bool touch_held;
        } beads;
        struct {
            uint8_t ring, trail_age, hit_grace;
            float previous_radius;
            float angle; /* Planet's fixed polar angle (radians). */
            orb_game_obstacle_t obstacles[ORB_GAME_OBSTACLES];
            /* Zero keeps the normal lane/speed cycle. Passing comets may use
             * an intermediate radius and signed angular speed; ring >= the
             * lane count disables a slot without changing sprite ordering. */
            float comet_radius[ORB_GAME_OBSTACLES], comet_speed[ORB_GAME_OBSTACLES];
            orb_game_star_t stars[ORB_GAME_ORBIT_STARS];
        } orbit;
        struct {
            orb_game_point_t cells[ORB_GAME_SNAKE_MAX], food;
            orb_game_point_t finger;
            float heading;
            uint8_t length, food_age, grace;
            bool touch_held, won;
        } snake;
        struct {
            orb_game_bubble_t bubbles[ORB_GAME_BUBBLES];
            uint16_t spawn_in;
            uint16_t time_left; /* Rim-timer countdown, in 1/60 s steps. */
            uint16_t reward; /* Points shown by the current pop feedback. */
        } pop;
    } game;
} orb_game_state_t;

/* Compact immutable presentation publication. This mirrors only fields read
 * by Games 4 interpolation; rules and solver scratch remain main-owned. */
typedef struct {
    orb_game_kind_t kind;
    uint32_t ticks;
    bool playing, controls;
    uint8_t lives;
    union {
        struct { float x, y, angle[ORB_MARBLE_GATES_MAX]; uint8_t stage; bool rewinding; } marble;
        struct { orb_game_point_t points[ORB_GAME_BEAD_COUNT]; uint16_t count; bool settled; } beads;
        struct { orb_game_point_t points[ORB_GAME_SNAKE_MAX]; uint16_t length; } snake;
        struct {
            float angle, comets[ORB_GAME_OBSTACLES], stars[ORB_GAME_ORBIT_STARS];
            uint8_t ring, comet_ring[ORB_GAME_OBSTACLES], star_ring[ORB_GAME_ORBIT_STARS];
            bool comet_passed[ORB_GAME_OBSTACLES], star_active[ORB_GAME_ORBIT_STARS];
        } orbit;
        struct { orb_game_point_t points[ORB_GAME_BUBBLES]; uint16_t radius[ORB_GAME_BUBBLES]; bool active[ORB_GAME_BUBBLES]; } pop;
    } game;
} orb_game_pose_t;
void orb_game_pose_capture(const orb_game_state_t *state, orb_game_pose_t *pose);
void orb_game_pose_interpolate(orb_game_state_t *state, const orb_game_pose_t *pose, float alpha);
size_t orb_game_snapshot_bytes(orb_game_kind_t kind);

const char *orb_game_id(orb_game_kind_t kind);
const char *orb_game_name(orb_game_kind_t kind);
bool orb_game_kind_from_id(const char *id, orb_game_kind_t *out);
/* Each game's single accent colour from Main.dc.html (hig.md §5). Content
 * hazards/collectibles may add their own colours; this is the screen accent. */
uint32_t orb_game_accent(orb_game_kind_t kind);
/* Set the wall-clock digits Beads settles into (24-hour hour, minute). The
 * logic layer is otherwise clock-independent; the runtime injects real time. */
void orb_game_beads_set_time(orb_game_state_t *state, uint8_t hour, uint8_t minute);
/* Screen-frame specific force (g) to effective downhill force (px/s²). */
orb_game_point_t orb_game_beads_force(float ax, float ay);
/* Coordinates are offsets from the panel centre, in the same pixel units as
 * physics and sprite extraction. Spawn, touch and chrome share this geometry. */
bool orb_game_position_clear(float x, float y, float radius,
                              orb_game_kind_t kind, bool controls);
/* Init and app entry start an active round. The shell's Play picker supplies
 * the attract screen and explicit launch action. */
void orb_game_init(orb_game_state_t *state, orb_game_kind_t kind, uint32_t seed);
/* Capture a fresh 300 ms neutral on round start/restart using distinct real samples,
 * then rotate their full 3D specific force into that frame. sample_us is the
 * duration of this sample, not fixed-step catchup time. False means calibration
 * is incomplete or this sample is invalid; outputs are zero in either case. */
void orb_game_tilt_reset(orb_game_tilt_t *tilt);
bool orb_game_tilt_update(orb_game_tilt_t *tilt, float ax, float ay, float az,
                          uint32_t sample_us, float *out_ax, float *out_ay);
/* Optional bias-corrected gyro in degrees/s. Motion is measured independently
 * of neutral. A held play tilt never changes the captured steering frame. */
bool orb_game_tilt_update_motion(orb_game_tilt_t *tilt, float ax, float ay, float az,
                                 const float gyro[3], uint32_t sample_us,
                                 float *out_ax, float *out_ay);
/* Physical Beads gravity: normalized acceleration in the existing calibrated
 * board axes, with no captured neutral, response curve or input dead zone.
 * Shares sensor-time quiet detection; valid gyro is required to form the time. */
bool orb_game_gravity_update_motion(orb_game_tilt_t *tilt, float ax, float ay, float az,
                                    const float gyro[3], uint32_t sample_us,
                                    float *out_ax, float *out_ay);
/* Publish sensor-time stillness before stepping. Catch-up physics must never
 * invent quiet samples. Invalid samples clear the observation as well. */
void orb_game_set_motion(orb_game_state_t *state, const orb_game_tilt_t *tilt);
/* Exactly one 1/60 s simulation step. No allocation, clock reads or rendering.
 * Input is screen-frame accelerometer specific force (+x right, +y up), in g;
 * downhill is -ax/+ay in LVGL's +y-down pixels. Runtime supplies Marble's
 * captured-neutral inputs for Marble/Snake and true gravity for Beads. Reduce Motion removes only
 * decorative response; it never slows essential game input or physics. */
void orb_game_step(orb_game_state_t *state, float ax, float ay,
                   bool motion_valid, bool reduce_motion);
void orb_game_action(orb_game_state_t *state, orb_game_action_t action, float x, float y);
/* The same slots retain their identity between frames, including inactive Pop
 * slots (radius 0). Renderer invalidates only changed old/new bounds. */
size_t orb_game_sprites(const orb_game_state_t *state, orb_game_sprite_t *out, size_t capacity);

/* Keepsake record LittleFS serialization. Tiny versioned record (OGK1). */
size_t orb_keepsake_encode(const orb_keepsake_record_t *rec, uint8_t *out, size_t capacity);
bool orb_keepsake_decode(const uint8_t *in, size_t size, orb_keepsake_record_t *out);
