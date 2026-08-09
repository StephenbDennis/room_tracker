/* Home Assistant reporting over WiFi + MQTT.
 *
 * Entirely optional and off unless the config carries credentials: with the
 * network disabled the radio stays BLE-only, which is what every install that
 * only drives GPIO wants.
 *
 * Zone state is published retained, so Home Assistant recovers the current
 * picture on restart without waiting for someone to walk through a room. An
 * MQTT last-will marks the node offline if it drops off, so a crashed board
 * shows as unavailable rather than as an empty room. */
#ifndef NET_HA_H
#define NET_HA_H

#include "config.h"
#include "zones.h"

/* Safe to call with the network disabled; it simply does nothing. node_id must
 * outlive the call — app_main's static id buffer does. */
void net_ha_init(const room_config_t *cfg, const char *node_id);

/* Re-read credentials and zone definitions after a config write. Reconnects
 * only if the connection parameters actually changed; republishes discovery
 * either way, since zones may have been added, renamed or removed. */
void net_ha_apply_config(const room_config_t *cfg);

/* Publish one zone's state. Called on change, and for every zone once the
 * broker connects. */
void net_ha_publish_zone(const zone_cfg_t *zone, bool active);

/* Publish the people count, their positions and the room size. Safe to call
 * every tick: it throttles to network.tracks_interval_ms internally, and a
 * change in the count goes out immediately regardless. Does nothing unless
 * network.publish_tracks is set. */
void net_ha_publish_tracks(const track_t *tracks, uint8_t count,
                           uint32_t now_ms);

/* True once WiFi has an IP and the broker session is up. */
bool net_ha_is_connected(void);

/* Receives `{"networks":[{"ssid","rssi","secure"},...]}` when a scan finishes.
 * Called from the WiFi event task, and the buffer is only valid for the call. */
typedef void (*net_ha_scan_cb_t)(const char *json, size_t len);

/* Start an access-point scan. Asynchronous: returns as soon as the scan is
 * queued, and `cb` fires later with the results. Works with reporting disabled
 * and no credentials stored -- that is the case it exists for. Returns false
 * if a scan is already running. */
bool net_ha_scan_start(net_ha_scan_cb_t cb);

#endif /* NET_HA_H */
