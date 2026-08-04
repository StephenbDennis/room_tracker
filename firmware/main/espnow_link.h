/* Peer-to-peer link between sensor nodes over ESP-NOW.
 *
 * Carries two things:
 *   - detections, so every node can fuse the whole room
 *   - heartbeats, so the connected node can report which peers exist and what
 *     config version each holds
 *
 * It deliberately does NOT distribute configuration. The user configures one
 * node at a time over BLE; heartbeats surface any resulting version drift as a
 * visible badge in the UI rather than a silent inconsistency. */
#ifndef ESPNOW_LINK_H
#define ESPNOW_LINK_H

#include "fusion.h"
#include "room_types.h"

#define ESPNOW_CHANNEL_DEFAULT  6
#define ESPNOW_PEER_TIMEOUT_MS  15000
#define ESPNOW_DET_MAX_AGE_MS   300

typedef struct {
    char     id[ROOM_ID_LEN];
    char     name[24];
    uint32_t config_version;
    uint32_t last_seen_ms;
    int8_t   rssi;
    bool     in_use;
} peer_info_t;

/* All nodes must sit on the same fixed channel; there is no AP to follow. */
void espnow_link_init(const char *self_node_id, const char *self_name,
                      uint8_t channel);

/* Broadcast this node's own detections, already in room coordinates. */
void espnow_link_broadcast_detections(const detection_t *dets, uint8_t count,
                                      uint32_t seq);

void espnow_link_send_heartbeat(uint32_t config_version, uint32_t uptime_s);

/* Includes peers that have gone quiet; compare last_seen_ms against
 * ESPNOW_PEER_TIMEOUT_MS to distinguish them. */
uint8_t espnow_link_get_peers(peer_info_t *out, uint8_t cap);

/* Gather every peer detection newer than max_age_ms. Stale peers are simply
 * omitted, which is what keeps a dead node from freezing phantom targets in
 * the room. */
uint8_t espnow_link_collect_detections(detection_t *out, uint8_t cap,
                                       uint32_t now_ms, uint32_t max_age_ms);

#endif /* ESPNOW_LINK_H */
