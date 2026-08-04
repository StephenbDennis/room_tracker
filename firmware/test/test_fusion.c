#include "../main/fusion.h"
#include "test.h"

#include <math.h>

/* -std=c11 hides M_PI; fusion.c and zones.c carry the same guard. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

TEST_STATE_DEFS

static node_pose_t pose(float x, float y, float theta)
{
    node_pose_t p;
    memset(&p, 0, sizeof p);
    strcpy(p.id, "n");
    p.x_mm = x; p.y_mm = y; p.theta_deg = theta; p.enabled = true;
    return p;
}

static ld2450_target_t target(int16_t x, int16_t y, int16_t speed)
{
    ld2450_target_t t = { .x_mm = x, .y_mm = y,
                          .speed_cms = speed, .resolution_mm = 320 };
    return t;
}

static detection_t det(float x, float y, float range, uint8_t node)
{
    detection_t d = { .x_mm = x, .y_mm = y, .speed_cms = 0,
                      .range_mm = range, .node_index = node };
    return d;
}

/* ---- coordinate transform ---- */

/* Sensor at the origin facing room +y: forward maps to +y, right maps to +x. */
static void test_transform_facing_up(void)
{
    node_pose_t p = pose(0, 0, 90);
    ld2450_target_t t = target(100, 1000, 0);
    detection_t d;

    fusion_transform(&p, &t, 0, &d);
    CHECK_NEAR(d.x_mm, 100, 0.1);
    CHECK_NEAR(d.y_mm, 1000, 0.1);
}

/* Facing room +x (east): forward is +x, and the sensor's right is -y (south). */
static void test_transform_facing_right(void)
{
    node_pose_t p = pose(0, 0, 0);
    ld2450_target_t t = target(100, 1000, 0);
    detection_t d;

    fusion_transform(&p, &t, 0, &d);
    CHECK_NEAR(d.x_mm, 1000, 0.1);
    CHECK_NEAR(d.y_mm, -100, 0.1);
}

static void test_transform_applies_translation(void)
{
    node_pose_t p = pose(2000, 3000, 90);
    ld2450_target_t t = target(0, 1500, 0);
    detection_t d;

    fusion_transform(&p, &t, 0, &d);
    CHECK_NEAR(d.x_mm, 2000, 0.1);
    CHECK_NEAR(d.y_mm, 4500, 0.1);
}

/* Two sensors in opposite corners looking at the same physical point must map
 * it to the same room coordinate. This is the property the whole multi-sensor
 * design rests on. */
static void test_two_sensors_agree_on_one_point(void)
{
    /* Room 5000 x 4000, target at the centre (2500, 2000). Sensor A sits in
     * the (0,0) corner and B in the (5000,4000) corner, each aimed directly at
     * the target, so it is straight ahead of both at equal range. */
    const float tx = 2500.0f, ty = 2000.0f;
    float range   = sqrtf(tx * tx + ty * ty);
    float theta_a = atan2f(ty, tx) * 180.0f / (float)M_PI;

    node_pose_t a = pose(0, 0, theta_a);
    node_pose_t b = pose(5000, 4000, theta_a + 180.0f);

    ld2450_target_t ta = target(0, (int16_t)range, 0);
    ld2450_target_t tb = target(0, (int16_t)range, 0);
    detection_t da, db;

    fusion_transform(&a, &ta, 0, &da);
    fusion_transform(&b, &tb, 1, &db);

    /* Each independently recovers the true room position ... */
    CHECK_NEAR(da.x_mm, tx, 5.0);
    CHECK_NEAR(da.y_mm, ty, 5.0);
    /* ... and therefore agree with each other, which is the property the whole
     * multi-sensor design rests on. */
    CHECK_NEAR(da.x_mm, db.x_mm, 5.0);
    CHECK_NEAR(da.y_mm, db.y_mm, 5.0);
}

static void test_transform_reports_range(void)
{
    node_pose_t p = pose(1000, 1000, 90);
    ld2450_target_t t = target(300, 400, 0);
    detection_t d;

    fusion_transform(&p, &t, 0, &d);
    CHECK_NEAR(d.range_mm, 500, 0.1);   /* 3-4-5 */
}

/* ---- merge ---- */

static void test_merge_collapses_nearby_detections(void)
{
    detection_t in[] = {
        det(2000, 2000, 1000, 0),
        det(2400, 2000, 1000, 1),   /* 400 mm away: same person */
    };
    detection_t out[4];

    uint8_t n = fusion_merge(in, 2, 500.0f, out, 4);
    CHECK_INT(n, 1);
    CHECK_NEAR(out[0].x_mm, 2200, 1.0);
    CHECK_NEAR(out[0].y_mm, 2000, 1.0);
}

static void test_merge_keeps_distinct_detections_apart(void)
{
    detection_t in[] = {
        det(2000, 2000, 1000, 0),
        det(3500, 2000, 1000, 1),   /* 1500 mm away: two people */
    };
    detection_t out[4];

    uint8_t n = fusion_merge(in, 2, 500.0f, out, 4);
    CHECK_INT(n, 2);
}

/* The closer sensor should dominate the merged position, because LD2450
 * accuracy degrades with range. */
static void test_merge_weights_closer_sensor_more(void)
{
    detection_t in[] = {
        det(2000, 2000, 1000, 0),   /* 1 m away */
        det(2400, 2000, 5000, 1),   /* 5 m away */
    };
    detection_t out[4];

    uint8_t n = fusion_merge(in, 2, 500.0f, out, 4);
    CHECK_INT(n, 1);
    /* Unweighted centroid would be 2200; weighting pulls it toward 2000. */
    CHECK(out[0].x_mm < 2100.0f);
    CHECK(out[0].x_mm > 2000.0f);
}

static void test_merge_result_independent_of_input_order(void)
{
    detection_t a[] = { det(2000, 2000, 1000, 0), det(2300, 2000, 1000, 1) };
    detection_t b[] = { det(2300, 2000, 1000, 1), det(2000, 2000, 1000, 0) };
    detection_t oa[4], ob[4];

    uint8_t na = fusion_merge(a, 2, 500.0f, oa, 4);
    uint8_t nb = fusion_merge(b, 2, 500.0f, ob, 4);
    CHECK_INT(na, 1);
    CHECK_INT(nb, 1);
    CHECK_NEAR(oa[0].x_mm, ob[0].x_mm, 0.1);
}

/* ---- tracking ---- */

static void test_track_confirms_after_required_hits(void)
{
    fusion_t f;
    fusion_init(&f, NULL);   /* confirm_frames = 2 */
    detection_t d = det(2000, 2000, 1000, 0);
    track_t out[ROOM_MAX_TRACKS];

    fusion_update(&f, &d, 1, 1000);
    CHECK_INT(fusion_get_tracks(&f, out, ROOM_MAX_TRACKS), 0);   /* tentative */

    fusion_update(&f, &d, 1, 1100);
    CHECK_INT(fusion_get_tracks(&f, out, ROOM_MAX_TRACKS), 1);
}

static void test_track_id_is_stable_while_moving(void)
{
    fusion_t f;
    fusion_init(&f, NULL);
    track_t out[ROOM_MAX_TRACKS];
    uint8_t id = 0;

    for (int i = 0; i < 10; i++) {
        detection_t d = det(2000.0f + i * 120.0f, 2000, 1000, 0);
        fusion_update(&f, &d, 1, 1000 + i * 100);
        uint8_t n = fusion_get_tracks(&f, out, ROOM_MAX_TRACKS);
        if (n == 1) {
            if (id == 0) id = out[0].id;
            CHECK_INT(out[0].id, id);
        }
    }
    CHECK(id != 0);
}

static void test_two_people_get_two_tracks(void)
{
    fusion_t f;
    fusion_init(&f, NULL);
    detection_t d[2] = { det(1000, 1000, 1000, 0), det(4000, 3000, 1000, 0) };
    track_t out[ROOM_MAX_TRACKS];

    fusion_update(&f, d, 2, 1000);
    fusion_update(&f, d, 2, 1100);
    CHECK_INT(fusion_get_tracks(&f, out, ROOM_MAX_TRACKS), 2);
}

/* A dropped frame or two must not destroy the track; the LD2450 loses targets
 * regularly and a flapping track would flap every zone it sits in. */
static void test_track_coasts_through_dropped_frames(void)
{
    fusion_t f;
    fusion_init(&f, NULL);
    detection_t d = det(2000, 2000, 1000, 0);
    track_t out[ROOM_MAX_TRACKS];

    fusion_update(&f, &d, 1, 1000);
    fusion_update(&f, &d, 1, 1100);
    CHECK_INT(fusion_get_tracks(&f, out, ROOM_MAX_TRACKS), 1);

    /* Three empty frames inside the 1000 ms coast window. */
    fusion_update(&f, NULL, 0, 1200);
    fusion_update(&f, NULL, 0, 1300);
    fusion_update(&f, NULL, 0, 1400);
    CHECK_INT(fusion_get_tracks(&f, out, ROOM_MAX_TRACKS), 1);
}

static void test_track_expires_after_coast_window(void)
{
    fusion_t f;
    fusion_init(&f, NULL);
    detection_t d = det(2000, 2000, 1000, 0);
    track_t out[ROOM_MAX_TRACKS];

    fusion_update(&f, &d, 1, 1000);
    fusion_update(&f, &d, 1, 1100);
    CHECK_INT(fusion_get_tracks(&f, out, ROOM_MAX_TRACKS), 1);

    fusion_update(&f, NULL, 0, 2200);   /* > coast_ms since last seen */
    CHECK_INT(fusion_get_tracks(&f, out, ROOM_MAX_TRACKS), 0);
}

/* A person walking through overlapping coverage must be one track, not two.
 * This is bring-up checklist item 3, executed in software. */
static void test_two_sensors_one_person_yields_one_track(void)
{
    fusion_t f;
    fusion_init(&f, NULL);
    track_t out[ROOM_MAX_TRACKS];

    node_pose_t a = pose(0, 0, 90);
    node_pose_t b = pose(4000, 0, 90);

    for (int i = 0; i < 5; i++) {
        float y = 2000.0f + i * 50.0f;
        /* Both sensors see the same person at room (2000, y), with the small
         * disagreement real hardware would produce. */
        ld2450_target_t ta = target(2000, (int16_t)y, 0);
        ld2450_target_t tb = target(-1850, (int16_t)(y + 120), 0);

        detection_t d[2];
        fusion_transform(&a, &ta, 0, &d[0]);
        fusion_transform(&b, &tb, 1, &d[1]);

        fusion_update(&f, d, 2, 1000 + i * 100);
    }

    CHECK_INT(fusion_get_tracks(&f, out, ROOM_MAX_TRACKS), 1);
}

static void test_motion_classified_moving_then_stopped(void)
{
    fusion_t f;
    fusion_init(&f, NULL);
    track_t out[ROOM_MAX_TRACKS];
    uint32_t t = 1000;

    /* Walking: 300 mm per 100 ms = 3000 mm/s. */
    for (int i = 0; i < 6; i++) {
        detection_t d = det(1000.0f + i * 300.0f, 2000, 1000, 0);
        d.speed_cms = 300;
        fusion_update(&f, &d, 1, t);
        t += 100;
    }
    CHECK_INT(fusion_get_tracks(&f, out, ROOM_MAX_TRACKS), 1);
    CHECK_INT(out[0].motion, MOTION_MOVING);

    /* Now standing still for well over stopped_hold_ms. */
    for (int i = 0; i < 30; i++) {
        detection_t d = det(2500, 2000, 1000, 0);
        d.speed_cms = 0;
        fusion_update(&f, &d, 1, t);
        t += 100;
    }
    CHECK_INT(fusion_get_tracks(&f, out, ROOM_MAX_TRACKS), 1);
    CHECK_INT(out[0].motion, MOTION_STOPPED);
}

/* Purely tangential motion produces near-zero radial speed, so classification
 * must fall back on position-derived velocity or it would read as stopped. */
static void test_tangential_motion_still_reads_as_moving(void)
{
    fusion_t f;
    fusion_init(&f, NULL);
    track_t out[ROOM_MAX_TRACKS];
    uint32_t t = 1000;

    for (int i = 0; i < 8; i++) {
        detection_t d = det(1000.0f + i * 250.0f, 2000, 1000, 0);
        d.speed_cms = 0;              /* radar reports no radial component */
        fusion_update(&f, &d, 1, t);
        t += 100;
    }

    CHECK_INT(fusion_get_tracks(&f, out, ROOM_MAX_TRACKS), 1);
    CHECK_INT(out[0].motion, MOTION_MOVING);
}

static void test_smoothing_reduces_jitter(void)
{
    fusion_t f;
    fusion_init(&f, NULL);
    track_t out[ROOM_MAX_TRACKS];
    uint32_t t = 1000;

    /* A stationary target reported with +/-200 mm of jitter. */
    const float jitter[] = { 0, 200, -200, 150, -180, 190, -160, 170 };
    for (int i = 0; i < 8; i++) {
        detection_t d = det(2000.0f + jitter[i], 2000, 1000, 0);
        fusion_update(&f, &d, 1, t);
        t += 100;
    }

    CHECK_INT(fusion_get_tracks(&f, out, ROOM_MAX_TRACKS), 1);
    /* The filtered output should sit far closer to the true 2000 than the
     * raw +/-200 swing it was fed. */
    CHECK_NEAR(out[0].x_mm, 2000, 150.0);
}

static void test_track_table_does_not_overflow(void)
{
    fusion_t f;
    fusion_init(&f, NULL);
    detection_t d[FUSION_MAX_DETECTIONS];
    track_t out[ROOM_MAX_TRACKS];

    for (int i = 0; i < FUSION_MAX_DETECTIONS; i++) {
        d[i] = det(1000.0f + i * 1500.0f, 1000, 1000, 0);
    }

    fusion_update(&f, d, FUSION_MAX_DETECTIONS, 1000);
    fusion_update(&f, d, FUSION_MAX_DETECTIONS, 1100);

    uint8_t n = fusion_get_tracks(&f, out, ROOM_MAX_TRACKS);
    CHECK(n <= ROOM_MAX_TRACKS);
}

int main(void)
{
    printf("\nfusion\n");
    RUN_TEST(test_transform_facing_up);
    RUN_TEST(test_transform_facing_right);
    RUN_TEST(test_transform_applies_translation);
    RUN_TEST(test_two_sensors_agree_on_one_point);
    RUN_TEST(test_transform_reports_range);
    RUN_TEST(test_merge_collapses_nearby_detections);
    RUN_TEST(test_merge_keeps_distinct_detections_apart);
    RUN_TEST(test_merge_weights_closer_sensor_more);
    RUN_TEST(test_merge_result_independent_of_input_order);
    RUN_TEST(test_track_confirms_after_required_hits);
    RUN_TEST(test_track_id_is_stable_while_moving);
    RUN_TEST(test_two_people_get_two_tracks);
    RUN_TEST(test_track_coasts_through_dropped_frames);
    RUN_TEST(test_track_expires_after_coast_window);
    RUN_TEST(test_two_sensors_one_person_yields_one_track);
    RUN_TEST(test_motion_classified_moving_then_stopped);
    RUN_TEST(test_tangential_motion_still_reads_as_moving);
    RUN_TEST(test_smoothing_reduces_jitter);
    RUN_TEST(test_track_table_does_not_overflow);
    return TEST_SUMMARY("fusion");
}
