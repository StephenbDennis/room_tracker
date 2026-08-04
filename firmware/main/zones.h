/* Event-box (zone) evaluation and trigger/untrigger state machine.
 *
 * Pure C: no ESP-IDF dependencies, so this compiles and tests on the host.
 * Zone rules are data, never compiled-in logic — the webpage writes them over
 * BLE and they take effect without a reflash. */
#ifndef ZONES_H
#define ZONES_H

#include "room_types.h"

#define ZONE_MAX_ACTIONS  4

typedef enum {
    COUNT_OP_GE = 0,   /* >= n */
    COUNT_OP_EQ = 1,   /* == n */
    COUNT_OP_LE = 2,   /* <= n */
} count_op_t;

typedef enum {
    /* Release begins when the trigger conditions stop being satisfied;
     * off_delay_ms is the hold applied after that. */
    UNTRIGGER_CONDITIONS_UNMET = 0,
    /* Release happens off_delay_ms after triggering, whether or not the
     * conditions still hold. */
    UNTRIGGER_TIMER = 1,
} untrigger_mode_t;

typedef enum {
    ZONE_IDLE      = 0,
    ZONE_ARMING    = 1,   /* conditions met, waiting out on_delay_ms */
    ZONE_TRIGGERED = 2,
    ZONE_RELEASING = 3,   /* waiting out off_delay_ms, cancellable */
} zone_phase_t;

typedef enum {
    ACTION_GPIO = 0,
} action_type_t;

typedef struct {
    action_type_t type;
    char          node_id[ROOM_ID_LEN];  /* which board owns the pin */
    uint8_t       pin;
    uint8_t       active_level;          /* 0 or 1 */
    bool          pulse;                 /* true = pulse, false = latch */
    uint32_t      pulse_ms;
} zone_action_t;

/* Axis-aligned rectangle with an optional rotation about its centre. */
typedef struct {
    float cx_mm;
    float cy_mm;
    float w_mm;
    float h_mm;
    float rot_deg;
} zone_rect_t;

typedef struct {
    char        id[ROOM_ID_LEN];
    char        name[32];
    bool        enabled;
    zone_rect_t rect;

    /* Trigger conditions */
    uint8_t     state_mask;      /* MOTION_MASK_* */
    count_op_t  count_op;
    uint8_t     count_n;
    uint32_t    on_delay_ms;     /* entry debounce */

    /* Untrigger conditions */
    untrigger_mode_t untrigger_mode;
    uint32_t         off_delay_ms;
    uint32_t         max_on_ms;  /* 0 = no cap */

    zone_action_t actions[ZONE_MAX_ACTIONS];
    uint8_t       action_count;
} zone_cfg_t;

/* Mutable per-zone state. Zero-initialise before first use. */
typedef struct {
    zone_phase_t phase;
    uint32_t     phase_since_ms;   /* when the current phase began */
    uint32_t     triggered_at_ms;
    uint8_t      match_count;      /* tracks matching the filter, last eval */
    bool         suppressed;       /* max_on_ms fired; wait for conditions to clear */
} zone_state_t;

/* Emitted when a zone's output changes, so callers can drive GPIO. */
typedef struct {
    uint8_t zone_index;
    bool    active;
} zone_event_t;

/* True if (x, y) lies inside the rectangle, accounting for rotation. */
bool zone_contains_point(const zone_rect_t *r, float x_mm, float y_mm);

/* Count tracks inside the zone whose motion state passes the filter. */
uint8_t zone_count_matching(const zone_cfg_t *z, const track_t *tracks,
                            uint8_t track_count);

/* Evaluate the raw trigger predicate (count comparison), ignoring timing. */
bool zone_conditions_met(const zone_cfg_t *z, const track_t *tracks,
                         uint8_t track_count, uint8_t *count_out);

/* Advance one zone's state machine. now_ms must be monotonic; wraparound is
 * handled. Returns true if the zone's active output changed this tick. */
bool zone_update(const zone_cfg_t *z, zone_state_t *st, const track_t *tracks,
                 uint8_t track_count, uint32_t now_ms, bool *active_out);

/* True when the zone should be driving its actions. */
static inline bool zone_is_active(const zone_state_t *st)
{
    return st->phase == ZONE_TRIGGERED || st->phase == ZONE_RELEASING;
}

/* Advance every zone. Fills events[] with changes, returns the event count. */
uint8_t zones_update_all(const zone_cfg_t *zones, zone_state_t *states,
                         uint8_t zone_count, const track_t *tracks,
                         uint8_t track_count, uint32_t now_ms,
                         zone_event_t *events, uint8_t events_cap);

#endif /* ZONES_H */
