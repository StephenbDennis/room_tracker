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

/* True once WiFi has an IP and the broker session is up. */
bool net_ha_is_connected(void);

#endif /* NET_HA_H */
