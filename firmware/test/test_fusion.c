#include "../main/fusion.h"
#include "test.h"

#include <math.h>

/* -std=c11 hides M_PI; fusion.c and zones.c carry the same guard. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

TEST_STATE_DEFS

static sensor_pose_t pose(float x, float y, float theta)
{
    sensor_pose_t p;
    memset(&p, 0, sizeof p);
    p.x_mm = x; p.y_mm = y; p.theta_deg = theta;
    return p;
}

static ld2450_target_t target(int16_t x, int16_t y, int16_t speed)
{
    ld2450_target_t t = { .x_mm = x, .y_mm = y,
                          .speed_cms = speed, .resolution_mm = 320 };
    return t;
}

static detection_t det(float x, float y, float range)
{
    detection_t d = { .x_mm = x, .y_mm = y, .speed_cms = 0,
                      .range_mm = range };
    return d;
}

/* ---- coordinate transform ---- */

/* Sensor at the origin facing room +y: forward maps to +y, right maps to +x. */
static void test_transform_facing_up(void)
{
    sensor_pose_t p = pose(0, 0, 90);
    ld2450_target_t t = target(100, 1000, 0);
    detection_t d;

    fusion_transform(&p, &t, &d);
    CHECK_NEAR(d.x_mm, 100, 0.1);
    CHECK_NEAR(d.y_mm, 1000, 0.1);
}

/* Facing room +x (east): forward is +x, and the sensor's right is -y (south). */
static void test_transform_facing_right(void)
{
    sensor_pose_t p = pose(0, 0, 0);
    ld2450_target_t t = target(100, 1000, 0);
    detection_t d;

    fusion_transform(&p, &t, &d);
    CHECK_NEAR(d.x_mm, 1000, 0.1);
    CHECK_NEAR(d.y_mm, -100, 0.1);
}

static void test_transform_applies_translation(void)
{
    sensor_pose_t p = pose(2000, 3000, 90);
    ld2450_target_t t = target(0, 1500, 0);
    detection_t d;

    fusion_transform(&p, &t, &d);
    CHECK_NEAR(d.x_mm, 2000, 0.1);
    CHECK_NEAR(d.y_mm, 4500, 0.1);
}

/* A sensor placed off-origin and rotated must still recover the true room
 * position of a target it sees straight ahead. */
static void test_transform_recovers_room_position(void)
{
    const float tx = 2500.0f, ty = 2000.0f;
    float range = sqrtf(tx * tx + ty * ty);
    float theta = atan2f(ty, tx) * 180.0f / (float)M_PI;

    sensor_pose_t p = pose(0, 0, theta);
    ld2450_target_t t = target(0, (int16_t)range, 0);
    detection_t d;

    fusion_transform(&p, &t, &d);
    CHECK_NEAR(d.x_mm, tx, 5.0);
    CHECK_NEAR(d.y_mm, ty, 5.0);
}

static void test_transform_reports_range(void)
{
    sensor_pose_t p = pose(1000, 1000, 90);
    ld2450_target_t t = target(300, 400, 0);
    detection_t d;

    fusion_transform(&p, &t, &d);
    CHECK_NEAR(d.range_mm, 500, 0.1);   /* 3-4-5 */
}

/* ---- tracking ---- */

/* With one sensor there is nothing to fuse, so two returns close together are
 * two people standing close together and must stay two tracks. The old
 * multi-sensor build deliberately collapsed these, which would now lose a
 * person from the count any zone rule depends on. */
static void test_nearby_detections_stay_separate(void)
{
    fusion_t f;
    fusion_init(&f, NULL);
    detection_t d[2] = { det(2000, 2000, 1000), det(2400, 2000, 1000) };
    track_t out[ROOM_MAX_TRACKS];

    fusion_update(&f, d, 2, 1000);
    fusion_update(&f, d, 2, 1100);
    CHECK_INT(fusion_get_tracks(&f, out, ROOM_MAX_TRACKS), 2);
}

static void test_track_confirms_after_required_hits(void)
{
    fusion_t f;
    fusion_init(&f, NULL);   /* confirm_frames = 2 */
    detection_t d = det(2000, 2000, 1000);
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
        detection_t d = det(2000.0f + i * 120.0f, 2000, 1000);
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
    detection_t d[2] = { det(1000, 1000, 1000), det(4000, 3000, 1000) };
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
    detection_t d = det(2000, 2000, 1000);
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
    detection_t d = det(2000, 2000, 1000);
    track_t out[ROOM_MAX_TRACKS];

    fusion_update(&f, &d, 1, 1000);
    fusion_update(&f, &d, 1, 1100);
    CHECK_INT(fusion_get_tracks(&f, out, ROOM_MAX_TRACKS), 1);

    fusion_update(&f, NULL, 0, 2200);   /* > coast_ms since last seen */
    CHECK_INT(fusion_get_tracks(&f, out, ROOM_MAX_TRACKS), 0);
}

static void test_motion_classified_moving_then_stopped(void)
{
    fusion_t f;
    fusion_init(&f, NULL);
    track_t out[ROOM_MAX_TRACKS];
    uint32_t t = 1000;

    /* Walking: 300 mm per 100 ms = 3000 mm/s. */
    for (int i = 0; i < 6; i++) {
        detection_t d = det(1000.0f + i * 300.0f, 2000, 1000);
        d.speed_cms = 300;
        fusion_update(&f, &d, 1, t);
        t += 100;
    }
    CHECK_INT(fusion_get_tracks(&f, out, ROOM_MAX_TRACKS), 1);
    CHECK_INT(out[0].motion, MOTION_MOVING);

    /* Now standing still for well over stopped_hold_ms. */
    for (int i = 0; i < 30; i++) {
        detection_t d = det(2500, 2000, 1000);
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
        detection_t d = det(1000.0f + i * 250.0f, 2000, 1000);
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
        detection_t d = det(2000.0f + jitter[i], 2000, 1000);
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
        d[i] = det(1000.0f + i * 1500.0f, 1000, 1000);
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
    RUN_TEST(test_transform_recovers_room_position);
    RUN_TEST(test_transform_reports_range);
    RUN_TEST(test_nearby_detections_stay_separate);
    RUN_TEST(test_track_confirms_after_required_hits);
    RUN_TEST(test_track_id_is_stable_while_moving);
    RUN_TEST(test_two_people_get_two_tracks);
    RUN_TEST(test_track_coasts_through_dropped_frames);
    RUN_TEST(test_track_expires_after_coast_window);
    RUN_TEST(test_motion_classified_moving_then_stopped);
    RUN_TEST(test_tangential_motion_still_reads_as_moving);
    RUN_TEST(test_smoothing_reduces_jitter);
    RUN_TEST(test_track_table_does_not_overflow);
    return TEST_SUMMARY("fusion");
}
