#include "net_ha.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"

static const char *TAG = "net";

#define TOPIC_MAX     192
#define PAYLOAD_MAX   768

static room_config_t       s_cfg;          /* private copy: zones are needed
                                            * for discovery long after the
                                            * caller's struct may have moved */
static char                s_node_id[ROOM_ID_LEN];
static esp_mqtt_client_handle_t s_client;
static bool                s_started;      /* WiFi + netif brought up once */
static bool                s_wifi_up;
static bool                s_mqtt_up;
static char                s_avail_topic[TOPIC_MAX];

/* ---------- topics ---------- */

/* Home Assistant object ids must be [a-zA-Z0-9_-]; zone ids and names are free
 * text from the webpage, so fold anything else to '_'. */
static void slugify(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < cap; i++) {
        char c = in[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-') {
            out[o++] = c;
        } else if (c >= 'A' && c <= 'Z') {
            out[o++] = (char)(c - 'A' + 'a');
        } else {
            out[o++] = '_';
        }
    }
    out[o] = '\0';
    if (o == 0 && cap > 1) {
        out[0] = 'z';
        out[1] = '\0';
    }
}

static void zone_state_topic(const zone_cfg_t *z, char *out, size_t cap)
{
    char slug[ROOM_ID_LEN * 2];
    slugify(z->id[0] ? z->id : z->name, slug, sizeof slug);
    snprintf(out, cap, "%s/%s/zone/%s/state",
             s_cfg.network.base_topic, s_node_id, slug);
}

/* ---------- Home Assistant discovery ---------- */

/* One retained discovery message per zone. Every zone carries the same
 * `device` block, so Home Assistant groups them under a single device rather
 * than scattering loose entities. */
static void publish_discovery(void)
{
    if (!s_client || !s_mqtt_up) {
        return;
    }

    char topic[TOPIC_MAX];
    char state[TOPIC_MAX];
    char slug[ROOM_ID_LEN * 2];
    char *payload = malloc(PAYLOAD_MAX);
    if (!payload) {
        return;
    }

    for (uint8_t i = 0; i < s_cfg.zone_count; i++) {
        const zone_cfg_t *z = &s_cfg.zones[i];
        slugify(z->id[0] ? z->id : z->name, slug, sizeof slug);
        zone_state_topic(z, state, sizeof state);

        snprintf(topic, sizeof topic, "%s/binary_sensor/roomtrack_%s_%s/config",
                 s_cfg.network.discovery_prefix, s_node_id, slug);

        int n = snprintf(payload, PAYLOAD_MAX,
            "{\"name\":\"%s\","
            "\"unique_id\":\"roomtrack_%s_%s\","
            "\"state_topic\":\"%s\","
            "\"availability_topic\":\"%s\","
            "\"device_class\":\"occupancy\","
            "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
            "\"device\":{\"identifiers\":[\"roomtrack_%s\"],"
            "\"name\":\"node-%s\",\"manufacturer\":\"room_tracker\","
            "\"model\":\"ESP32-S3 + LD2450\"}}",
            z->name[0] ? z->name : slug, s_node_id, slug,
            state, s_avail_topic, s_node_id, s_node_id);

        if (n > 0 && n < PAYLOAD_MAX) {
            esp_mqtt_client_publish(s_client, topic, payload, n, 1, true);
        }
    }

    if (s_cfg.network.publish_tracks) {
        char attrs[TOPIC_MAX];
        snprintf(topic, sizeof topic, "%s/sensor/roomtrack_%s_people/config",
                 s_cfg.network.discovery_prefix, s_node_id);
        snprintf(state, sizeof state, "%s/%s/people/state",
                 s_cfg.network.base_topic, s_node_id);
        snprintf(attrs, sizeof attrs, "%s/%s/people/attributes",
                 s_cfg.network.base_topic, s_node_id);

        /* Room size and per-target positions ride as attributes rather than
         * separate entities: they are context for the count, and templating
         * off attributes beats a dozen sensors nobody graphs. */
        int n = snprintf(payload, PAYLOAD_MAX,
            "{\"name\":\"people\","
            "\"unique_id\":\"roomtrack_%s_people\","
            "\"state_topic\":\"%s\","
            "\"json_attributes_topic\":\"%s\","
            "\"availability_topic\":\"%s\","
            "\"state_class\":\"measurement\","
            "\"unit_of_measurement\":\"people\","
            "\"icon\":\"mdi:account-multiple\","
            "\"device\":{\"identifiers\":[\"roomtrack_%s\"],"
            "\"name\":\"node-%s\",\"manufacturer\":\"room_tracker\","
            "\"model\":\"ESP32-S3 + LD2450\"}}",
            s_node_id, state, attrs, s_avail_topic, s_node_id, s_node_id);

        if (n > 0 && n < PAYLOAD_MAX) {
            esp_mqtt_client_publish(s_client, topic, payload, n, 1, true);
        }
    }

    ESP_LOGI(TAG, "published discovery for %u zone(s)%s", s_cfg.zone_count,
             s_cfg.network.publish_tracks ? " + people" : "");
    free(payload);
}

/* A zone removed from the config must have its retained discovery message
 * cleared, or Home Assistant keeps the entity forever. An empty retained
 * payload is the documented way to delete one. */
static void clear_discovery(const zone_cfg_t *zones, uint8_t count)
{
    if (!s_client || !s_mqtt_up) {
        return;
    }
    char topic[TOPIC_MAX];
    char slug[ROOM_ID_LEN * 2];

    for (uint8_t i = 0; i < count; i++) {
        slugify(zones[i].id[0] ? zones[i].id : zones[i].name, slug, sizeof slug);
        snprintf(topic, sizeof topic, "%s/binary_sensor/roomtrack_%s_%s/config",
                 s_cfg.network.discovery_prefix, s_node_id, slug);
        esp_mqtt_client_publish(s_client, topic, "", 0, 1, true);
    }
}

void net_ha_publish_zone(const zone_cfg_t *zone, bool active)
{
    if (!s_client || !s_mqtt_up) {
        return;
    }
    char topic[TOPIC_MAX];
    zone_state_topic(zone, topic, sizeof topic);
    /* Retained: Home Assistant restarts should not have to wait for someone to
     * walk through the room before the entity has a value again. */
    esp_mqtt_client_publish(s_client, topic, active ? "ON" : "OFF", 0, 1, true);
}

/* ---------- people / room ---------- */

static uint32_t s_last_tracks_ms;
static uint8_t  s_last_count = 0xFF;   /* 0xFF = nothing published yet */

static const char *motion_name(motion_state_t m)
{
    switch (m) {
        case MOTION_MOVING:  return "moving";
        case MOTION_STOPPED: return "stopped";
        default:             return "unknown";
    }
}

void net_ha_publish_tracks(const track_t *tracks, uint8_t count,
                           uint32_t now_ms)
{
    if (!s_client || !s_mqtt_up || !s_cfg.network.publish_tracks) {
        return;
    }

    /* A change in how many people are in the room is the interesting event and
     * goes out at once; their positions shifting is not worth 10 Hz of broker
     * traffic and recorder writes. */
    bool changed = (count != s_last_count);
    if (!changed &&
        (uint32_t)(now_ms - s_last_tracks_ms) < s_cfg.network.tracks_interval_ms) {
        return;
    }
    s_last_tracks_ms = now_ms;
    s_last_count = count;

    char topic[TOPIC_MAX];
    char num[8];
    snprintf(num, sizeof num, "%u", count);
    snprintf(topic, sizeof topic, "%s/%s/people/state",
             s_cfg.network.base_topic, s_node_id);
    esp_mqtt_client_publish(s_client, topic, num, 0, 0, true);

    char *payload = malloc(PAYLOAD_MAX);
    if (!payload) {
        return;
    }
    int n = snprintf(payload, PAYLOAD_MAX,
                     "{\"room_w_mm\":%d,\"room_h_mm\":%d,\"targets\":[",
                     (int)s_cfg.room_w_mm, (int)s_cfg.room_h_mm);

    for (uint8_t i = 0; i < count && n > 0 && n < PAYLOAD_MAX; i++) {
        n += snprintf(payload + n, (size_t)(PAYLOAD_MAX - n),
                      "%s{\"id\":%u,\"x_mm\":%d,\"y_mm\":%d,\"motion\":\"%s\"}",
                      i ? "," : "", tracks[i].id,
                      (int)tracks[i].x_mm, (int)tracks[i].y_mm,
                      motion_name(tracks[i].motion));
    }
    if (n > 0 && n < PAYLOAD_MAX) {
        n += snprintf(payload + n, (size_t)(PAYLOAD_MAX - n), "]}");
    }

    if (n > 0 && n < PAYLOAD_MAX) {
        snprintf(topic, sizeof topic, "%s/%s/people/attributes",
                 s_cfg.network.base_topic, s_node_id);
        esp_mqtt_client_publish(s_client, topic, payload, n, 0, true);
    }
    free(payload);
}

/* ---------- MQTT ---------- */

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_up = true;
        ESP_LOGI(TAG, "MQTT connected");
        esp_mqtt_client_publish(s_client, s_avail_topic, "online", 0, 1, true);
        publish_discovery();
        /* Seed every zone so Home Assistant has a value immediately; zone
         * events only fire on change, which could be hours away. */
        for (uint8_t i = 0; i < s_cfg.zone_count; i++) {
            net_ha_publish_zone(&s_cfg.zones[i], false);
        }
        s_last_count = 0xFF;   /* republish people on the next tick */
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_up = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT error");
        break;

    default:
        break;
    }
}

static void mqtt_start(void)
{
    if (s_client || s_cfg.network.mqtt_uri[0] == '\0') {
        return;
    }

    snprintf(s_avail_topic, sizeof s_avail_topic, "%s/%s/availability",
             s_cfg.network.base_topic, s_node_id);

    esp_mqtt_client_config_t mc = {
        .broker.address.uri = s_cfg.network.mqtt_uri,
        .credentials.username = s_cfg.network.mqtt_user[0]
                                    ? s_cfg.network.mqtt_user : NULL,
        .credentials.authentication.password = s_cfg.network.mqtt_pass[0]
                                    ? s_cfg.network.mqtt_pass : NULL,
        .session.last_will.topic = s_avail_topic,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
        .session.keepalive = 30,
    };

    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) {
        ESP_LOGE(TAG, "MQTT client init failed");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    esp_mqtt_client_start(s_client);
}

/* ---------- WiFi ---------- */

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_up = false;
        /* The zone engine and GPIO do not care that the network is gone, so
         * just keep retrying quietly rather than escalating. */
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        s_wifi_up = true;
        ESP_LOGI(TAG, "wifi up, ip " IPSTR, IP2STR(&e->ip_info.ip));
        mqtt_start();
    }
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, s_cfg.network.wifi_ssid,
            sizeof wc.sta.ssid - 1);
    strncpy((char *)wc.sta.password, s_cfg.network.wifi_pass,
            sizeof wc.sta.password - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    /* BLE stays the configuration path and must keep working while WiFi is
     * associating, so let the coexistence scheduler share the radio. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_started = true;
}

/* ---------- public ---------- */

void net_ha_init(const room_config_t *cfg, const char *node_id)
{
    s_cfg = *cfg;
    strncpy(s_node_id, node_id, sizeof s_node_id - 1);
    s_node_id[sizeof s_node_id - 1] = '\0';

    if (!cfg->network.enabled || cfg->network.wifi_ssid[0] == '\0') {
        ESP_LOGI(TAG, "network reporting disabled");
        return;
    }
    wifi_start();
}

void net_ha_apply_config(const room_config_t *cfg)
{
    bool was_enabled = s_cfg.network.enabled;
    bool creds_changed =
        strcmp(s_cfg.network.wifi_ssid, cfg->network.wifi_ssid) != 0 ||
        strcmp(s_cfg.network.wifi_pass, cfg->network.wifi_pass) != 0 ||
        strcmp(s_cfg.network.mqtt_uri,  cfg->network.mqtt_uri)  != 0 ||
        strcmp(s_cfg.network.mqtt_user, cfg->network.mqtt_user) != 0 ||
        strcmp(s_cfg.network.mqtt_pass, cfg->network.mqtt_pass) != 0;

    /* Zones that no longer exist have to have their retained discovery
     * messages deleted before the new set is published, or Home Assistant
     * keeps entities for boxes the user removed. */
    zone_cfg_t old[ROOM_MAX_ZONES];
    uint8_t old_count = s_cfg.zone_count;
    memcpy(old, s_cfg.zones, sizeof old);

    if (creds_changed || (cfg->network.enabled != was_enabled)) {
        ESP_LOGW(TAG, "network settings changed; reboot to apply");
    }

    clear_discovery(old, old_count);

    /* Turning people reporting off has to delete its entity too, or Home
     * Assistant keeps a sensor that will never update again. */
    if (s_cfg.network.publish_tracks && !cfg->network.publish_tracks &&
        s_client && s_mqtt_up) {
        char topic[TOPIC_MAX];
        snprintf(topic, sizeof topic, "%s/sensor/roomtrack_%s_people/config",
                 s_cfg.network.discovery_prefix, s_node_id);
        esp_mqtt_client_publish(s_client, topic, "", 0, 1, true);
    }

    s_cfg = *cfg;
    s_last_count = 0xFF;   /* force the next publish, whatever the count */
    publish_discovery();
    for (uint8_t i = 0; i < s_cfg.zone_count; i++) {
        net_ha_publish_zone(&s_cfg.zones[i], false);
    }
}

bool net_ha_is_connected(void)
{
    return s_wifi_up && s_mqtt_up;
}
