/* Types shared between fusion and the zone engine.
 *
 * Room frame: origin at a chosen corner, millimetres, +x right, +y up in plan
 * view. Every coordinate outside of ld2450.h is in this frame. */
#ifndef ROOM_TYPES_H
#define ROOM_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#define ROOM_MAX_NODES    8
#define ROOM_MAX_TRACKS   12
#define ROOM_MAX_ZONES    16
#define ROOM_ID_LEN       16

typedef enum {
    MOTION_UNKNOWN = 0,
    MOTION_MOVING  = 1,
    MOTION_STOPPED = 2,
} motion_state_t;

/* Bitmask form, used by zone trigger filters. MOTION_MASK_ANY matches both. */
#define MOTION_MASK_MOVING   (1u << 0)
#define MOTION_MASK_STOPPED  (1u << 1)
#define MOTION_MASK_ANY      (MOTION_MASK_MOVING | MOTION_MASK_STOPPED)

static inline uint8_t motion_to_mask(motion_state_t s)
{
    switch (s) {
        case MOTION_MOVING:  return MOTION_MASK_MOVING;
        case MOTION_STOPPED: return MOTION_MASK_STOPPED;
        default:             return 0;
    }
}

/* A fused person-sized object in room coordinates. */
typedef struct {
    uint8_t        id;         /* stable while the track lives; reused after expiry */
    float          x_mm;
    float          y_mm;
    float          vx_mms;     /* velocity, mm/s */
    float          vy_mms;
    motion_state_t motion;
    bool           active;
} track_t;

/* Where a sensor sits in the room. theta_deg is the boresight bearing measured
 * counter-clockwise from room +x. */
typedef struct {
    char  id[ROOM_ID_LEN];
    float x_mm;
    float y_mm;
    float theta_deg;
    bool  enabled;
} node_pose_t;

#endif /* ROOM_TYPES_H */
