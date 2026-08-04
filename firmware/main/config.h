/* Room configuration: the single source of truth shared by firmware and web.
 *
 * This is DATA, not code. The webpage writes it over BLE and it takes effect
 * without a reflash — reflashing is only ever needed to add new capability
 * (a new action type, say), never to change a room's setup. */
#ifndef CONFIG_H
#define CONFIG_H

#include "fusion.h"
#include "room_types.h"
#include "zones.h"

#define CONFIG_NVS_NAMESPACE  "roomtrack"
#define CONFIG_NVS_KEY        "room"
#define CONFIG_JSON_MAX       6144

typedef struct {
    uint32_t     version;        /* monotonic; bumped on every write */
    float        room_w_mm;
    float        room_h_mm;

    node_pose_t  nodes[ROOM_MAX_NODES];
    uint8_t      node_count;

    zone_cfg_t   zones[ROOM_MAX_ZONES];
    uint8_t      zone_count;

    fusion_cfg_t fusion;
} room_config_t;

/* An empty 5 x 4 m room with tuning defaults and no zones. */
void config_defaults(room_config_t *cfg);

/* Index of a node by id string, or -1. */
int config_find_node(const room_config_t *cfg, const char *node_id);

/* JSON codec. Returns false / 0 on malformed input or insufficient capacity.
 * The schema is documented in docs/protocol.md. */
bool   config_from_json(const char *json, size_t len, room_config_t *out);
size_t config_to_json(const room_config_t *cfg, char *out, size_t cap);

/* NVS persistence. Both return true on success; config_load() falls back to
 * defaults (and still returns true) when nothing is stored yet. */
bool config_load(room_config_t *cfg);
bool config_save(const room_config_t *cfg);

#endif /* CONFIG_H */
