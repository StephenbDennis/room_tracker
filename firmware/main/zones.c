#include "zones.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Monotonic elapsed time. Unsigned subtraction gives the correct answer across
 * the 49-day uint32 millisecond wrap, so no special-casing is needed. */
static inline uint32_t elapsed_since(uint32_t now_ms, uint32_t then_ms)
{
    return now_ms - then_ms;
}

bool zone_contains_point(const zone_rect_t *r, float x_mm, float y_mm)
{
    float dx = x_mm - r->cx_mm;
    float dy = y_mm - r->cy_mm;

    if (r->rot_deg != 0.0f) {
        /* Rotate the point into the rectangle's own frame (by -rot). */
        float rad = (float)(r->rot_deg * M_PI / 180.0);
        float c = cosf(rad), s = sinf(rad);
        float lx =  dx * c + dy * s;
        float ly = -dx * s + dy * c;
        dx = lx;
        dy = ly;
    }

    return fabsf(dx) <= (r->w_mm * 0.5f) && fabsf(dy) <= (r->h_mm * 0.5f);
}

uint8_t zone_count_matching(const zone_cfg_t *z, const track_t *tracks,
                            uint8_t track_count)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < track_count; i++) {
        const track_t *t = &tracks[i];
        if (!t->active) {
            continue;
        }
        /* MOTION_MASK_ANY means the user does not care about motion state, so
         * it has to admit a track the classifier has not resolved yet.
         * motion_to_mask(MOTION_UNKNOWN) is 0, which fails every filter
         * including this one, so without the first clause a person standing
         * in a box is counted by nothing at all. */
        if (z->state_mask != MOTION_MASK_ANY &&
            (motion_to_mask(t->motion) & z->state_mask) == 0) {
            continue;
        }
        if (zone_contains_point(&z->rect, t->x_mm, t->y_mm)) {
            n++;
        }
    }
    return n;
}

bool zone_conditions_met(const zone_cfg_t *z, const track_t *tracks,
                         uint8_t track_count, uint8_t *count_out)
{
    uint8_t n = zone_count_matching(z, tracks, track_count);
    if (count_out) {
        *count_out = n;
    }

    switch (z->count_op) {
        case COUNT_OP_GE: return n >= z->count_n;
        case COUNT_OP_EQ: return n == z->count_n;
        case COUNT_OP_LE: return n <= z->count_n;
        default:          return false;
    }
}

static void enter_phase(zone_state_t *st, zone_phase_t phase, uint32_t now_ms)
{
    st->phase = phase;
    st->phase_since_ms = now_ms;
    if (phase == ZONE_TRIGGERED && st->triggered_at_ms == 0) {
        st->triggered_at_ms = now_ms;
    }
}

bool zone_update(const zone_cfg_t *z, zone_state_t *st, const track_t *tracks,
                 uint8_t track_count, uint32_t now_ms, bool *active_out)
{
    bool was_active = zone_is_active(st);

    if (!z->enabled) {
        st->phase = ZONE_IDLE;
        st->match_count = 0;
        st->suppressed = false;
        st->triggered_at_ms = 0;
        if (active_out) *active_out = false;
        return was_active;
    }

    bool met = zone_conditions_met(z, tracks, track_count, &st->match_count);

    /* Transitions can cascade within a single tick when a delay is zero
     * (IDLE -> ARMING -> TRIGGERED). Loop until the phase settles rather than
     * making the caller wait an extra tick per hop. */
    for (int guard = 0; guard < 4; guard++) {
        zone_phase_t before = st->phase;

        switch (st->phase) {
        case ZONE_IDLE:
            /* After a max_on cutoff or a timer expiry the zone stays parked
             * until the conditions clear at least once. Without this a
             * still-occupied zone would immediately re-fire and oscillate. */
            if (st->suppressed) {
                if (!met) {
                    st->suppressed = false;
                }
                break;
            }
            if (met) {
                st->triggered_at_ms = 0;
                enter_phase(st, ZONE_ARMING, now_ms);
            }
            break;

        case ZONE_ARMING:
            if (!met) {
                enter_phase(st, ZONE_IDLE, now_ms);
            } else if (elapsed_since(now_ms, st->phase_since_ms) >= z->on_delay_ms) {
                enter_phase(st, ZONE_TRIGGERED, now_ms);
            }
            break;

        case ZONE_TRIGGERED:
            if (z->max_on_ms != 0 &&
                elapsed_since(now_ms, st->triggered_at_ms) >= z->max_on_ms) {
                st->suppressed = true;
                enter_phase(st, ZONE_IDLE, now_ms);
                break;
            }
            if (z->untrigger_mode == UNTRIGGER_TIMER) {
                /* Hold for off_delay_ms measured from the trigger, regardless
                 * of whether anyone is still present. */
                if (elapsed_since(now_ms, st->triggered_at_ms) >= z->off_delay_ms) {
                    st->suppressed = true;
                    enter_phase(st, ZONE_IDLE, now_ms);
                }
            } else if (!met) {
                enter_phase(st, ZONE_RELEASING, now_ms);
            }
            break;

        case ZONE_RELEASING:
            if (z->max_on_ms != 0 &&
                elapsed_since(now_ms, st->triggered_at_ms) >= z->max_on_ms) {
                st->suppressed = true;
                enter_phase(st, ZONE_IDLE, now_ms);
                break;
            }
            if (met) {
                /* Conditions came back before the hold expired. Cancel the
                 * release; this is what stops someone pacing a boundary from
                 * chattering the output. */
                st->phase = ZONE_TRIGGERED;
            } else if (elapsed_since(now_ms, st->phase_since_ms) >= z->off_delay_ms) {
                st->triggered_at_ms = 0;
                enter_phase(st, ZONE_IDLE, now_ms);
            }
            break;
        }

        if (st->phase == before) {
            break;
        }
    }

    bool now_active = zone_is_active(st);
    if (active_out) {
        *active_out = now_active;
    }
    return now_active != was_active;
}

uint8_t zones_update_all(const zone_cfg_t *zones, zone_state_t *states,
                         uint8_t zone_count, const track_t *tracks,
                         uint8_t track_count, uint32_t now_ms,
                         zone_event_t *events, uint8_t events_cap)
{
    uint8_t n = 0;

    for (uint8_t i = 0; i < zone_count; i++) {
        bool active = false;
        if (zone_update(&zones[i], &states[i], tracks, track_count,
                        now_ms, &active)) {
            if (n < events_cap) {
                events[n].zone_index = i;
                events[n].active = active;
                n++;
            }
        }
    }

    return n;
}
