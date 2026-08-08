/* Detection tracking: transform, associate, smooth.
 *
 * Pure C: no ESP-IDF dependencies, so this compiles and tests on the host.
 *
 * One sensor per room, so there is nothing to fuse across sources: the radar
 * already separates its targets, and two returns close together are two people
 * standing close together, not one person seen twice. */
#ifndef FUSION_H
#define FUSION_H

#include "ld2450.h"
#include "room_types.h"

#define FUSION_MAX_DETECTIONS  LD2450_MAX_TARGETS

/* One radar return, already transformed into room coordinates. */
typedef struct {
    float x_mm;
    float y_mm;
    float speed_cms;    /* radial speed reported by the sensor, signed */
    float range_mm;     /* distance from the sensor */
} detection_t;

typedef struct {
    float    assoc_gate_mm;      /* max track-to-detection association distance */
    uint32_t coast_ms;           /* keep a track alive this long unseen */
    uint8_t  confirm_frames;     /* hits before a track is published */
    float    alpha;              /* alpha-beta position gain */
    float    beta;               /* alpha-beta velocity gain */
    float    moving_thresh_mms;  /* above this => moving */
    float    stopped_thresh_mms; /* below this, held, => stopped */
    uint32_t stopped_hold_ms;    /* dwell before declaring stopped */
} fusion_cfg_t;

typedef struct {
    track_t  pub;
    uint32_t last_seen_ms;
    uint32_t last_moving_ms;
    uint8_t  hits;
    bool     confirmed;
    bool     in_use;
} fusion_track_t;

typedef struct {
    fusion_cfg_t   cfg;
    fusion_track_t tracks[ROOM_MAX_TRACKS];
    uint8_t        next_id;
    uint32_t       last_update_ms;
    bool           started;
} fusion_t;

/* Sensible defaults for a room-scale install; see docs/protocol.md. */
void fusion_default_cfg(fusion_cfg_t *cfg);

/* cfg may be NULL to accept defaults. */
void fusion_init(fusion_t *f, const fusion_cfg_t *cfg);

/* Transform one sensor-frame target into room coordinates.
 *
 * The sensor's local +y runs forward along boresight and +x to its right;
 * theta_deg is the boresight bearing counter-clockwise from room +x. */
void fusion_transform(const sensor_pose_t *pose, const ld2450_target_t *t,
                      detection_t *out);

/* Advance the tracker by one frame of room-frame detections. */
void fusion_update(fusion_t *f, const detection_t *dets, uint8_t det_count,
                   uint32_t now_ms);

/* Copy confirmed, live tracks out. Returns the count written. */
uint8_t fusion_get_tracks(const fusion_t *f, track_t *out, uint8_t out_cap);

#endif /* FUSION_H */
