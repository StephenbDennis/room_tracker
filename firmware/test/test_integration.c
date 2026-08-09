/* The fusion -> zones seam.
 *
 * test_fusion.c and test_zones.c each pass while the two disagree: the zone
 * tests build track_t by hand and never produce MOTION_UNKNOWN, which is the
 * state a real tracker emits for over a second on every new track. That gap
 * hid a bug where a person standing in a box triggered nothing at all. These
 * tests drive zones with tracks the tracker actually produced. */
#include "../main/fusion.h"
#include "../main/zones.h"
#include "test.h"

TEST_STATE_DEFS

/* A 1500 mm box at (2000, 2000) matching any motion state, no debounce --
 * the defaults the web app writes for a new event box. */
static zone_cfg_t any_zone(void)
{
    zone_cfg_t z;
    memset(&z, 0, sizeof z);
    strcpy(z.id, "z1");
    strcpy(z.name, "box");
    z.enabled = true;
    z.rect = (zone_rect_t){ .cx_mm = 2000, .cy_mm = 2000,
                            .w_mm = 1500, .h_mm = 1500, .rot_deg = 0 };
    z.state_mask = MOTION_MASK_ANY;
    z.count_op = COUNT_OP_GE;
    z.count_n = 1;
    z.untrigger_mode = UNTRIGGER_CONDITIONS_UNMET;
    return z;
}

static detection_t at(float x, float y)
{
    detection_t d;
    memset(&d, 0, sizeof d);
    d.x_mm = x;
    d.y_mm = y;
    d.range_mm = 1000;
    return d;
}

/* The reported bug: someone stands in the box and nothing fires. The track is
 * confirmed and inside, but the classifier has not resolved its motion yet. */
static void test_standing_person_triggers_an_any_zone(void)
{
    fusion_t f;
    fusion_init(&f, NULL);
    zone_cfg_t z = any_zone();
    zone_state_t st;
    memset(&st, 0, sizeof st);
    track_t tracks[ROOM_MAX_TRACKS];

    bool ever_active = false;
    for (int i = 0; i < 8; i++) {
        uint32_t t = 1000 + i * 100;
        detection_t d = at(2000, 2000);
        fusion_update(&f, &d, 1, t);
        uint8_t n = fusion_get_tracks(&f, tracks, ROOM_MAX_TRACKS);

        bool active = false;
        zone_update(&z, &st, tracks, n, t, &active);
        if (zone_is_active(&st)) ever_active = true;
    }

    /* 800 ms is well inside stopped_hold_ms, so the track is still UNKNOWN
     * here. "Any" has to count it regardless. */
    CHECK(ever_active);
}

/* A target whose speed sits between stopped_thresh and moving_thresh never
 * crosses either, so it has nothing to resolve against and must not be left
 * unclassified for the rest of its life. */
static void test_dead_band_target_does_not_stay_unknown(void)
{
    fusion_t f;
    fusion_init(&f, NULL);
    track_t tracks[ROOM_MAX_TRACKS];

    /* 7 mm per 100 ms frame = 70 mm/s, between the 50 and 100 defaults. */
    for (int i = 0; i < 40; i++) {
        detection_t d = at(2000.0f + i * 7.0f, 2000);
        d.speed_cms = 7;
        fusion_update(&f, &d, 1, 1000 + i * 100);
    }

    CHECK_INT(fusion_get_tracks(&f, tracks, ROOM_MAX_TRACKS), 1);
    CHECK(tracks[0].motion != MOTION_UNKNOWN);
}

/* A motion-filtered zone must still discriminate: "any" admitting UNKNOWN
 * must not have turned every filter into a pass. */
static void test_stopped_only_zone_ignores_a_walking_target(void)
{
    fusion_t f;
    fusion_init(&f, NULL);
    zone_cfg_t z = any_zone();
    z.state_mask = MOTION_MASK_STOPPED;
    zone_state_t st;
    memset(&st, 0, sizeof st);
    track_t tracks[ROOM_MAX_TRACKS];

    /* 300 mm per frame = 3 m/s, unambiguously moving, and inside the box. */
    for (int i = 0; i < 5; i++) {
        uint32_t t = 1000 + i * 100;
        detection_t d = at(1400.0f + i * 300.0f, 2000);
        d.speed_cms = 300;
        fusion_update(&f, &d, 1, t);
        uint8_t n = fusion_get_tracks(&f, tracks, ROOM_MAX_TRACKS);
        bool active = false;
        zone_update(&z, &st, tracks, n, t, &active);
    }

    CHECK_INT(tracks[0].motion, MOTION_MOVING);
    CHECK(!zone_is_active(&st));
}

/* Walking in and stopping must hold the zone, not drop it on the transition
 * from MOVING through the band into STOPPED. */
static void test_zone_holds_across_moving_to_stopped(void)
{
    fusion_t f;
    fusion_init(&f, NULL);
    zone_cfg_t z = any_zone();
    zone_state_t st;
    memset(&st, 0, sizeof st);
    track_t tracks[ROOM_MAX_TRACKS];
    uint32_t t = 1000;

    for (int i = 0; i < 4; i++, t += 100) {
        detection_t d = at(1500.0f + i * 200.0f, 2000);
        d.speed_cms = 200;
        fusion_update(&f, &d, 1, t);
        uint8_t n = fusion_get_tracks(&f, tracks, ROOM_MAX_TRACKS);
        bool active = false;
        zone_update(&z, &st, tracks, n, t, &active);
    }
    CHECK(zone_is_active(&st));

    for (int i = 0; i < 30; i++, t += 100) {
        detection_t d = at(2100, 2000);
        fusion_update(&f, &d, 1, t);
        uint8_t n = fusion_get_tracks(&f, tracks, ROOM_MAX_TRACKS);
        bool active = false;
        zone_update(&z, &st, tracks, n, t, &active);
        CHECK(zone_is_active(&st));
    }

    CHECK_INT(tracks[0].motion, MOTION_STOPPED);
}

int main(void)
{
    printf("\nintegration\n");
    RUN_TEST(test_standing_person_triggers_an_any_zone);
    RUN_TEST(test_dead_band_target_does_not_stay_unknown);
    RUN_TEST(test_stopped_only_zone_ignores_a_walking_target);
    RUN_TEST(test_zone_holds_across_moving_to_stopped);
    return TEST_SUMMARY("integration");
}
