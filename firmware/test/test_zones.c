#include "../main/zones.h"
#include "test.h"

TEST_STATE_DEFS

/* A 1000 x 1000 mm box centred at (2000, 2000), triggering on one or more
 * moving targets, with no delays unless a test overrides them. */
static zone_cfg_t make_zone(void)
{
    zone_cfg_t z;
    memset(&z, 0, sizeof z);
    strcpy(z.id, "z1");
    strcpy(z.name, "desk");
    z.enabled = true;
    z.rect = (zone_rect_t){ .cx_mm = 2000, .cy_mm = 2000,
                            .w_mm = 1000, .h_mm = 1000, .rot_deg = 0 };
    z.state_mask = MOTION_MASK_MOVING;
    z.count_op = COUNT_OP_GE;
    z.count_n = 1;
    z.on_delay_ms = 0;
    z.untrigger_mode = UNTRIGGER_CONDITIONS_UNMET;
    z.off_delay_ms = 0;
    z.max_on_ms = 0;
    return z;
}

static track_t make_track(uint8_t id, float x, float y, motion_state_t m)
{
    track_t t;
    memset(&t, 0, sizeof t);
    t.id = id;
    t.x_mm = x;
    t.y_mm = y;
    t.motion = m;
    t.active = true;
    return t;
}

/* ---- geometry ---- */

static void test_point_inside_axis_aligned_rect(void)
{
    zone_rect_t r = { 2000, 2000, 1000, 1000, 0 };
    CHECK(zone_contains_point(&r, 2000, 2000));
    CHECK(zone_contains_point(&r, 1500, 1500));   /* corner, inclusive */
    CHECK(zone_contains_point(&r, 2500, 2500));
    CHECK(!zone_contains_point(&r, 1499, 2000));
    CHECK(!zone_contains_point(&r, 2000, 2501));
}

static void test_point_inside_rotated_rect(void)
{
    /* A 45-degree rotated 1000x200 slot: points along the diagonal are inside,
     * points off the diagonal at the same radius are not. */
    zone_rect_t r = { 0, 0, 1000, 200, 45 };
    CHECK(zone_contains_point(&r, 0, 0));
    CHECK(zone_contains_point(&r, 300, 300));      /* along the long axis */
    CHECK(!zone_contains_point(&r, 300, -300));    /* across the short axis */
}

/* ---- counting and predicates ---- */

static void test_counts_only_matching_motion_state(void)
{
    zone_cfg_t z = make_zone();
    track_t tracks[] = {
        make_track(1, 2000, 2000, MOTION_MOVING),
        make_track(2, 2100, 2100, MOTION_STOPPED),
        make_track(3, 2200, 2200, MOTION_MOVING),
    };
    CHECK_INT(zone_count_matching(&z, tracks, 3), 2);

    z.state_mask = MOTION_MASK_STOPPED;
    CHECK_INT(zone_count_matching(&z, tracks, 3), 1);

    z.state_mask = MOTION_MASK_ANY;
    CHECK_INT(zone_count_matching(&z, tracks, 3), 3);
}

static void test_ignores_tracks_outside_and_inactive(void)
{
    zone_cfg_t z = make_zone();
    z.state_mask = MOTION_MASK_ANY;

    track_t tracks[] = {
        make_track(1, 2000, 2000, MOTION_MOVING),
        make_track(2, 9000, 9000, MOTION_MOVING),   /* outside */
        make_track(3, 2100, 2100, MOTION_MOVING),   /* inactive below */
    };
    tracks[2].active = false;

    CHECK_INT(zone_count_matching(&z, tracks, 3), 1);
}

static void test_count_operators(void)
{
    zone_cfg_t z = make_zone();
    z.state_mask = MOTION_MASK_ANY;
    track_t tracks[] = {
        make_track(1, 1900, 1900, MOTION_MOVING),
        make_track(2, 2100, 2100, MOTION_MOVING),
    };
    uint8_t n = 0;

    z.count_op = COUNT_OP_GE; z.count_n = 2;
    CHECK(zone_conditions_met(&z, tracks, 2, &n));
    CHECK_INT(n, 2);

    z.count_op = COUNT_OP_GE; z.count_n = 3;
    CHECK(!zone_conditions_met(&z, tracks, 2, &n));

    z.count_op = COUNT_OP_EQ; z.count_n = 2;
    CHECK(zone_conditions_met(&z, tracks, 2, &n));

    z.count_op = COUNT_OP_EQ; z.count_n = 1;
    CHECK(!zone_conditions_met(&z, tracks, 2, &n));

    z.count_op = COUNT_OP_LE; z.count_n = 2;
    CHECK(zone_conditions_met(&z, tracks, 2, &n));

    z.count_op = COUNT_OP_LE; z.count_n = 1;
    CHECK(!zone_conditions_met(&z, tracks, 2, &n));
}

/* ---- state machine ---- */

static void test_triggers_immediately_with_no_delays(void)
{
    zone_cfg_t z = make_zone();
    zone_state_t st = {0};
    track_t t = make_track(1, 2000, 2000, MOTION_MOVING);
    bool active = false;

    CHECK(zone_update(&z, &st, &t, 1, 1000, &active));   /* changed */
    CHECK(active);
    CHECK_INT(st.phase, ZONE_TRIGGERED);
}

static void test_on_delay_debounces_entry(void)
{
    zone_cfg_t z = make_zone();
    z.on_delay_ms = 300;
    zone_state_t st = {0};
    track_t t = make_track(1, 2000, 2000, MOTION_MOVING);
    bool active = false;

    zone_update(&z, &st, &t, 1, 1000, &active);
    CHECK(!active);
    CHECK_INT(st.phase, ZONE_ARMING);

    zone_update(&z, &st, &t, 1, 1200, &active);
    CHECK(!active);

    zone_update(&z, &st, &t, 1, 1300, &active);
    CHECK(active);
    CHECK_INT(st.phase, ZONE_TRIGGERED);
}

/* A single jittery frame at the boundary must not fire the output. This is the
 * whole reason on_delay_ms exists. */
static void test_on_delay_rejects_single_frame_blip(void)
{
    zone_cfg_t z = make_zone();
    z.on_delay_ms = 300;
    zone_state_t st = {0};
    track_t in  = make_track(1, 2000, 2000, MOTION_MOVING);
    track_t out = make_track(1, 9000, 9000, MOTION_MOVING);
    bool active = false;

    zone_update(&z, &st, &in,  1, 1000, &active);   /* blip in */
    CHECK_INT(st.phase, ZONE_ARMING);
    zone_update(&z, &st, &out, 1, 1100, &active);   /* gone again */
    CHECK_INT(st.phase, ZONE_IDLE);
    CHECK(!active);

    /* Still idle well past the original delay window. */
    zone_update(&z, &st, &out, 1, 2000, &active);
    CHECK(!active);
}

static void test_off_delay_holds_then_releases(void)
{
    zone_cfg_t z = make_zone();
    z.off_delay_ms = 5000;
    zone_state_t st = {0};
    track_t in  = make_track(1, 2000, 2000, MOTION_MOVING);
    track_t out = make_track(1, 9000, 9000, MOTION_MOVING);
    bool active = false;

    zone_update(&z, &st, &in, 1, 1000, &active);
    CHECK(active);

    zone_update(&z, &st, &out, 1, 2000, &active);
    CHECK(active);                       /* holding */
    CHECK_INT(st.phase, ZONE_RELEASING);

    zone_update(&z, &st, &out, 1, 6999, &active);
    CHECK(active);

    CHECK(zone_update(&z, &st, &out, 1, 7000, &active));   /* changed */
    CHECK(!active);
    CHECK_INT(st.phase, ZONE_IDLE);
}

/* Someone pacing across a boundary should keep the output steady, not chatter
 * it. Re-entry during RELEASING cancels the release. */
static void test_reentry_during_releasing_cancels_release(void)
{
    zone_cfg_t z = make_zone();
    z.off_delay_ms = 5000;
    zone_state_t st = {0};
    track_t in  = make_track(1, 2000, 2000, MOTION_MOVING);
    track_t out = make_track(1, 9000, 9000, MOTION_MOVING);
    bool active = false;

    zone_update(&z, &st, &in,  1, 1000, &active);
    CHECK_INT(st.phase, ZONE_TRIGGERED);

    zone_update(&z, &st, &out, 1, 2000, &active);
    CHECK_INT(st.phase, ZONE_RELEASING);

    /* Back inside before the hold expires. */
    CHECK(!zone_update(&z, &st, &in, 1, 3000, &active));   /* no output change */
    CHECK(active);
    CHECK_INT(st.phase, ZONE_TRIGGERED);

    /* Leaving again restarts the full hold from this moment, not the first. */
    zone_update(&z, &st, &out, 1, 4000, &active);
    CHECK_INT(st.phase, ZONE_RELEASING);
    zone_update(&z, &st, &out, 1, 8500, &active);
    CHECK(active);
    zone_update(&z, &st, &out, 1, 9000, &active);
    CHECK(!active);
}

static void test_max_on_forces_release_while_still_occupied(void)
{
    zone_cfg_t z = make_zone();
    z.max_on_ms = 10000;
    zone_state_t st = {0};
    track_t in = make_track(1, 2000, 2000, MOTION_MOVING);
    bool active = false;

    zone_update(&z, &st, &in, 1, 1000, &active);
    CHECK(active);

    zone_update(&z, &st, &in, 1, 9000, &active);
    CHECK(active);

    CHECK(zone_update(&z, &st, &in, 1, 11000, &active));
    CHECK(!active);
    CHECK_INT(st.phase, ZONE_IDLE);
}

/* After a max_on cutoff the zone must stay parked while the target is still
 * present, otherwise it would immediately re-trigger and oscillate forever. */
static void test_max_on_suppresses_until_conditions_clear(void)
{
    zone_cfg_t z = make_zone();
    z.max_on_ms = 10000;
    zone_state_t st = {0};
    track_t in  = make_track(1, 2000, 2000, MOTION_MOVING);
    track_t out = make_track(1, 9000, 9000, MOTION_MOVING);
    bool active = false;

    zone_update(&z, &st, &in, 1, 1000, &active);
    zone_update(&z, &st, &in, 1, 11000, &active);
    CHECK(!active);

    /* Still there, long after: must stay off. */
    zone_update(&z, &st, &in, 1, 20000, &active);
    CHECK(!active);
    zone_update(&z, &st, &in, 1, 60000, &active);
    CHECK(!active);

    /* Leaves -> suppression clears. */
    zone_update(&z, &st, &out, 1, 61000, &active);
    CHECK(!active);

    /* Returns -> fires again. */
    CHECK(zone_update(&z, &st, &in, 1, 62000, &active));
    CHECK(active);
}

static void test_timer_mode_releases_regardless_of_occupancy(void)
{
    zone_cfg_t z = make_zone();
    z.untrigger_mode = UNTRIGGER_TIMER;
    z.off_delay_ms = 30000;
    zone_state_t st = {0};
    track_t in = make_track(1, 2000, 2000, MOTION_MOVING);
    bool active = false;

    zone_update(&z, &st, &in, 1, 1000, &active);
    CHECK(active);

    zone_update(&z, &st, &in, 1, 20000, &active);
    CHECK(active);

    /* Target never left, but the timer expires anyway. */
    CHECK(zone_update(&z, &st, &in, 1, 31000, &active));
    CHECK(!active);

    /* And stays off until they leave and return. */
    zone_update(&z, &st, &in, 1, 90000, &active);
    CHECK(!active);
}

static void test_disabled_zone_never_triggers(void)
{
    zone_cfg_t z = make_zone();
    z.enabled = false;
    zone_state_t st = {0};
    track_t in = make_track(1, 2000, 2000, MOTION_MOVING);
    bool active = false;

    zone_update(&z, &st, &in, 1, 1000, &active);
    CHECK(!active);
    CHECK_INT(st.phase, ZONE_IDLE);
}

/* uint32 millisecond counters wrap after ~49 days; the delays must still be
 * measured correctly across the boundary. */
static void test_timing_survives_millis_wraparound(void)
{
    zone_cfg_t z = make_zone();
    z.off_delay_ms = 5000;
    zone_state_t st = {0};
    track_t in  = make_track(1, 2000, 2000, MOTION_MOVING);
    track_t out = make_track(1, 9000, 9000, MOTION_MOVING);
    bool active = false;

    uint32_t near_wrap = 0xFFFFF000u;

    zone_update(&z, &st, &in, 1, near_wrap, &active);
    CHECK(active);

    zone_update(&z, &st, &out, 1, near_wrap + 1000, &active);
    CHECK(active);
    CHECK_INT(st.phase, ZONE_RELEASING);

    /* Crosses zero mid-hold. */
    zone_update(&z, &st, &out, 1, (uint32_t)(near_wrap + 4000), &active);
    CHECK(active);

    zone_update(&z, &st, &out, 1, (uint32_t)(near_wrap + 6000), &active);
    CHECK(!active);
}

static void test_zones_update_all_reports_events(void)
{
    zone_cfg_t zones[2] = { make_zone(), make_zone() };
    zones[1].rect.cx_mm = 4000;
    zones[1].rect.cy_mm = 4000;

    zone_state_t states[2] = {0};
    zone_event_t events[4];

    track_t tracks[] = {
        make_track(1, 2000, 2000, MOTION_MOVING),   /* in zone 0 */
        make_track(2, 4000, 4000, MOTION_MOVING),   /* in zone 1 */
    };

    uint8_t n = zones_update_all(zones, states, 2, tracks, 2, 1000, events, 4);
    CHECK_INT(n, 2);
    CHECK(events[0].active);
    CHECK(events[1].active);

    /* Steady state produces no further events. */
    n = zones_update_all(zones, states, 2, tracks, 2, 1100, events, 4);
    CHECK_INT(n, 0);
}

int main(void)
{
    printf("\nzones\n");
    RUN_TEST(test_point_inside_axis_aligned_rect);
    RUN_TEST(test_point_inside_rotated_rect);
    RUN_TEST(test_counts_only_matching_motion_state);
    RUN_TEST(test_ignores_tracks_outside_and_inactive);
    RUN_TEST(test_count_operators);
    RUN_TEST(test_triggers_immediately_with_no_delays);
    RUN_TEST(test_on_delay_debounces_entry);
    RUN_TEST(test_on_delay_rejects_single_frame_blip);
    RUN_TEST(test_off_delay_holds_then_releases);
    RUN_TEST(test_reentry_during_releasing_cancels_release);
    RUN_TEST(test_max_on_forces_release_while_still_occupied);
    RUN_TEST(test_max_on_suppresses_until_conditions_clear);
    RUN_TEST(test_timer_mode_releases_regardless_of_occupancy);
    RUN_TEST(test_disabled_zone_never_triggers);
    RUN_TEST(test_timing_survives_millis_wraparound);
    RUN_TEST(test_zones_update_all_reports_events);
    return TEST_SUMMARY("zones");
}
