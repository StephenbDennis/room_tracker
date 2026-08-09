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

#define NET_SSID_LEN    33
#define NET_PASS_LEN    65
#define NET_URI_LEN     128
#define NET_USER_LEN    33
#define NET_TOPIC_LEN   32

/* WiFi and MQTT, for reporting zone state to Home Assistant. Off by default:
 * the radio is BLE-only until someone supplies credentials. */
typedef struct {
    bool enabled;
    char wifi_ssid[NET_SSID_LEN];
    char wifi_pass[NET_PASS_LEN];
    char mqtt_uri[NET_URI_LEN];        /* mqtt://host:1883 */
    char mqtt_user[NET_USER_LEN];
    char mqtt_pass[NET_PASS_LEN];
    char base_topic[NET_TOPIC_LEN];        /* default "roomtrack" */
    char discovery_prefix[NET_TOPIC_LEN];  /* default "homeassistant" */
} network_cfg_t;

/* Whether a serialised config carries the secrets. NVS needs them; the copy
 * served over BLE must not, or anyone who can reach CONFIG_READ in config mode
 * can read the WiFi password straight back out. */
typedef enum {
    CONFIG_JSON_REDACTED = 0,
    CONFIG_JSON_WITH_SECRETS = 1,
} config_json_mode_t;

typedef struct {
    uint32_t     version;        /* monotonic; bumped on every write */
    float        room_w_mm;
    float        room_h_mm;

    sensor_pose_t sensor;

    zone_cfg_t   zones[ROOM_MAX_ZONES];
    uint8_t      zone_count;

    fusion_cfg_t fusion;
    network_cfg_t network;
} room_config_t;

/* An empty 5 x 4 m room with tuning defaults and no zones. */
void config_defaults(room_config_t *cfg);

/* JSON codec. Returns false / 0 on malformed input or insufficient capacity.
 * The schema is documented in docs/protocol.md. */
bool   config_from_json(const char *json, size_t len, room_config_t *out);
size_t config_to_json(const room_config_t *cfg, char *out, size_t cap,
                      config_json_mode_t mode);

/* Copy secrets from `current` into `incoming` wherever `incoming` left them
 * blank. The webpage never receives the stored passwords, so it cannot send
 * them back; without this every config push would wipe them. */
void config_carry_secrets(room_config_t *incoming, const room_config_t *current);

/* NVS persistence. Both return true on success; config_load() falls back to
 * defaults (and still returns true) when nothing is stored yet. */
bool config_load(room_config_t *cfg);
bool config_save(const room_config_t *cfg);

#endif /* CONFIG_H */
