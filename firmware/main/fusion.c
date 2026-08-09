#include "fusion.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DT_MIN_S  0.01f
#define DT_MAX_S  0.50f

static inline float dist2(float ax, float ay, float bx, float by)
{
    float dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy;
}

void fusion_default_cfg(fusion_cfg_t *cfg)
{
    cfg->assoc_gate_mm      = 800.0f;
    cfg->coast_ms           = 1000;
    cfg->confirm_frames     = 2;
    cfg->alpha              = 0.45f;
    cfg->beta               = 0.10f;
    cfg->moving_thresh_mms  = 100.0f;   /* 10 cm/s */
    cfg->stopped_thresh_mms = 50.0f;    /*  5 cm/s */
    cfg->stopped_hold_ms    = 1000;
}

void fusion_init(fusion_t *f, const fusion_cfg_t *cfg)
{
    memset(f, 0, sizeof *f);
    if (cfg) {
        f->cfg = *cfg;
    } else {
        fusion_default_cfg(&f->cfg);
    }
    f->next_id = 1;   /* 0 is reserved as "no track" */
}

void fusion_transform(const sensor_pose_t *pose, const ld2450_target_t *t,
                      detection_t *out)
{
    float rad = (float)(pose->theta_deg * M_PI / 180.0);
    float c = cosf(rad), s = sinf(rad);

    float lx = (float)t->x_mm;   /* right of boresight */
    float ly = (float)t->y_mm;   /* forward along boresight */

    /* forward = (c, s); right = (s, -c) */
    out->x_mm = pose->x_mm + ly * c + lx * s;
    out->y_mm = pose->y_mm + ly * s - lx * c;

    out->speed_cms = (float)t->speed_cms;
    out->range_mm  = sqrtf(lx * lx + ly * ly);
}

static fusion_track_t *alloc_track(fusion_t *f)
{
    for (uint8_t i = 0; i < ROOM_MAX_TRACKS; i++) {
        if (!f->tracks[i].in_use) {
            fusion_track_t *tr = &f->tracks[i];
            memset(tr, 0, sizeof *tr);
            tr->in_use = true;
            tr->pub.id = f->next_id++;
            if (f->next_id == 0) {
                f->next_id = 1;
            }
            return tr;
        }
    }
    return NULL;
}

static void classify_motion(const fusion_cfg_t *cfg, fusion_track_t *tr,
                            float radial_speed_mms, uint32_t now_ms)
{
    float vmag = sqrtf(tr->pub.vx_mms * tr->pub.vx_mms +
                       tr->pub.vy_mms * tr->pub.vy_mms);

    /* The radar's reported speed is radial only, so it misses purely
     * tangential motion; the position-derived velocity misses slow radial
     * creep. Taking the larger of the two catches both. */
    float rs = fabsf(radial_speed_mms);
    float effective = vmag > rs ? vmag : rs;

    if (effective > cfg->moving_thresh_mms) {
        tr->pub.motion = MOTION_MOVING;
        tr->last_moving_ms = now_ms;
    } else if (effective < cfg->stopped_thresh_mms) {
        if ((uint32_t)(now_ms - tr->last_moving_ms) >= cfg->stopped_hold_ms) {
            tr->pub.motion = MOTION_STOPPED;
        }
        /* Between the thresholds, hold the previous classification. */
    }
}

void fusion_update(fusion_t *f, const detection_t *dets, uint8_t det_count,
                   uint32_t now_ms)
{
    uint8_t n = det_count > FUSION_MAX_DETECTIONS ? FUSION_MAX_DETECTIONS
                                                  : det_count;

    float dt = 0.1f;
    if (f->started) {
        dt = (float)(uint32_t)(now_ms - f->last_update_ms) / 1000.0f;
        if (dt < DT_MIN_S) dt = DT_MIN_S;
        if (dt > DT_MAX_S) dt = DT_MAX_S;
    }
    f->started = true;
    f->last_update_ms = now_ms;

    /* --- predict --- */
    for (uint8_t i = 0; i < ROOM_MAX_TRACKS; i++) {
        fusion_track_t *tr = &f->tracks[i];
        if (!tr->in_use) continue;
        tr->pub.x_mm += tr->pub.vx_mms * dt;
        tr->pub.y_mm += tr->pub.vy_mms * dt;
    }

    /* --- associate: greedy nearest pair under the gate --- */
    bool det_used[FUSION_MAX_DETECTIONS] = { false };
    bool trk_used[ROOM_MAX_TRACKS] = { false };
    float gate2 = f->cfg.assoc_gate_mm * f->cfg.assoc_gate_mm;

    for (;;) {
        float best = gate2;
        int bt = -1, bd = -1;

        for (uint8_t ti = 0; ti < ROOM_MAX_TRACKS; ti++) {
            if (!f->tracks[ti].in_use || trk_used[ti]) continue;
            for (uint8_t di = 0; di < n; di++) {
                if (det_used[di]) continue;
                float d2 = dist2(f->tracks[ti].pub.x_mm, f->tracks[ti].pub.y_mm,
                                 dets[di].x_mm, dets[di].y_mm);
                if (d2 < best) {
                    best = d2;
                    bt = ti;
                    bd = di;
                }
            }
        }
        if (bt < 0) {
            break;
        }

        trk_used[bt] = true;
        det_used[bd] = true;

        fusion_track_t *tr = &f->tracks[bt];
        const detection_t *z = &dets[bd];

        /* alpha-beta correction against the prediction */
        float rx = z->x_mm - tr->pub.x_mm;
        float ry = z->y_mm - tr->pub.y_mm;
        tr->pub.x_mm   += f->cfg.alpha * rx;
        tr->pub.y_mm   += f->cfg.alpha * ry;
        tr->pub.vx_mms += (f->cfg.beta * rx) / dt;
        tr->pub.vy_mms += (f->cfg.beta * ry) / dt;

        tr->last_seen_ms = now_ms;
        if (tr->hits < 255) tr->hits++;
        if (!tr->confirmed && tr->hits >= f->cfg.confirm_frames) {
            tr->confirmed = true;
        }
        tr->pub.active = tr->confirmed;

        classify_motion(&f->cfg, tr, z->speed_cms * 10.0f, now_ms);
    }

    /* --- unmatched detections become tentative tracks --- */
    for (uint8_t di = 0; di < n; di++) {
        if (det_used[di]) continue;
        fusion_track_t *tr = alloc_track(f);
        if (!tr) break;   /* track table full; drop the extra detection */

        tr->pub.x_mm   = dets[di].x_mm;
        tr->pub.y_mm   = dets[di].y_mm;
        tr->pub.vx_mms = 0.0f;
        tr->pub.vy_mms = 0.0f;
        tr->pub.motion = MOTION_UNKNOWN;
        tr->last_seen_ms   = now_ms;
        tr->last_moving_ms = now_ms;
        tr->hits = 1;
        tr->confirmed = (f->cfg.confirm_frames <= 1);
        tr->pub.active = tr->confirmed;
    }

    /* --- coast then expire unmatched tracks --- */
    for (uint8_t i = 0; i < ROOM_MAX_TRACKS; i++) {
        fusion_track_t *tr = &f->tracks[i];
        if (!tr->in_use || trk_used[i]) continue;

        if ((uint32_t)(now_ms - tr->last_seen_ms) >= f->cfg.coast_ms) {
            memset(tr, 0, sizeof *tr);   /* frees the slot */
        }
        /* Otherwise it keeps coasting on the predicted position above, which
         * is what carries a track through a dropped frame or two. */
    }
}

uint8_t fusion_get_tracks(const fusion_t *f, track_t *out, uint8_t out_cap)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < ROOM_MAX_TRACKS && n < out_cap; i++) {
        if (f->tracks[i].in_use && f->tracks[i].confirmed) {
            out[n++] = f->tracks[i].pub;
        }
    }
    return n;
}
