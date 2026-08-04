#include "espnow_link.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"

static const char *TAG = "espnow";

#define ESPNOW_MAGIC    0x4C44   /* 'LD' */
#define ESPNOW_VERSION  1

enum { PKT_HEARTBEAT = 1, PKT_DETECTIONS = 2 };

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  version;
    uint8_t  type;
    char     node_id[ROOM_ID_LEN];
} espnow_hdr_t;

typedef struct __attribute__((packed)) {
    espnow_hdr_t hdr;
    uint32_t     config_version;
    uint32_t     uptime_s;
    char         name[24];
} espnow_heartbeat_t;

typedef struct __attribute__((packed)) {
    int16_t x_mm;
    int16_t y_mm;
    int16_t speed_cms;
    uint16_t range_mm;
} espnow_det_t;

typedef struct __attribute__((packed)) {
    espnow_hdr_t hdr;
    uint32_t     seq;
    uint8_t      count;
    espnow_det_t dets[LD2450_MAX_TARGETS];
} espnow_dets_t;

static const uint8_t BROADCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

typedef struct {
    peer_info_t  info;
    detection_t  dets[LD2450_MAX_TARGETS];
    uint8_t      det_count;
    uint32_t     det_at_ms;
    uint8_t      node_index;
} peer_slot_t;

static peer_slot_t       s_peers[ROOM_MAX_NODES];
static char              s_self_id[ROOM_ID_LEN];
static char              s_self_name[24];
static SemaphoreHandle_t s_lock;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static peer_slot_t *peer_find_or_add(const char *id)
{
    for (uint8_t i = 0; i < ROOM_MAX_NODES; i++) {
        if (s_peers[i].info.in_use &&
            strncmp(s_peers[i].info.id, id, ROOM_ID_LEN) == 0) {
            return &s_peers[i];
        }
    }
    for (uint8_t i = 0; i < ROOM_MAX_NODES; i++) {
        if (!s_peers[i].info.in_use) {
            memset(&s_peers[i], 0, sizeof s_peers[i]);
            s_peers[i].info.in_use = true;
            s_peers[i].node_index = i;
            strncpy(s_peers[i].info.id, id, ROOM_ID_LEN - 1);
            ESP_LOGI(TAG, "peer joined: %s", s_peers[i].info.id);
            return &s_peers[i];
        }
    }
    return NULL;
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < (int)sizeof(espnow_hdr_t)) return;

    const espnow_hdr_t *h = (const espnow_hdr_t *)data;
    if (h->magic != ESPNOW_MAGIC || h->version != ESPNOW_VERSION) return;
    /* Ignore our own broadcast echo. */
    if (strncmp(h->node_id, s_self_id, ROOM_ID_LEN) == 0) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    peer_slot_t *p = peer_find_or_add(h->node_id);
    if (!p) {
        xSemaphoreGive(s_lock);
        return;
    }
    p->info.last_seen_ms = now_ms();
    if (info && info->rx_ctrl) {
        p->info.rssi = info->rx_ctrl->rssi;
    }

    if (h->type == PKT_HEARTBEAT && len >= (int)sizeof(espnow_heartbeat_t)) {
        const espnow_heartbeat_t *hb = (const espnow_heartbeat_t *)data;
        p->info.config_version = hb->config_version;
        memcpy(p->info.name, hb->name, sizeof p->info.name);
        p->info.name[sizeof p->info.name - 1] = '\0';
    } else if (h->type == PKT_DETECTIONS && len >= (int)sizeof(espnow_dets_t)) {
        const espnow_dets_t *d = (const espnow_dets_t *)data;
        uint8_t n = d->count > LD2450_MAX_TARGETS ? LD2450_MAX_TARGETS : d->count;
        for (uint8_t i = 0; i < n; i++) {
            p->dets[i].x_mm       = (float)d->dets[i].x_mm;
            p->dets[i].y_mm       = (float)d->dets[i].y_mm;
            p->dets[i].speed_cms  = (float)d->dets[i].speed_cms;
            p->dets[i].range_mm   = (float)d->dets[i].range_mm;
            p->dets[i].node_index = p->node_index;
        }
        p->det_count = n;
        p->det_at_ms = now_ms();
    }
    xSemaphoreGive(s_lock);
}

void espnow_link_init(const char *self_node_id, const char *self_name,
                      uint8_t channel)
{
    memset(s_peers, 0, sizeof s_peers);
    strncpy(s_self_id, self_node_id, ROOM_ID_LEN - 1);
    s_self_id[ROOM_ID_LEN - 1] = '\0';
    strncpy(s_self_name, self_name, sizeof s_self_name - 1);
    s_self_name[sizeof s_self_name - 1] = '\0';

    s_lock = xSemaphoreCreateMutex();

    /* ESP-NOW needs WiFi initialised even though we never associate. Staying
     * in STA mode without connecting keeps the radio free for BLE coexistence
     * to schedule around. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));

    esp_now_peer_info_t peer = {
        .channel = channel,
        .ifidx   = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, BROADCAST_MAC, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    ESP_LOGI(TAG, "ESP-NOW up as '%s' on channel %u", s_self_id, channel);
}

static void fill_hdr(espnow_hdr_t *h, uint8_t type)
{
    h->magic   = ESPNOW_MAGIC;
    h->version = ESPNOW_VERSION;
    h->type    = type;
    memset(h->node_id, 0, ROOM_ID_LEN);
    strncpy(h->node_id, s_self_id, ROOM_ID_LEN - 1);
}

void espnow_link_broadcast_detections(const detection_t *dets, uint8_t count,
                                      uint32_t seq)
{
    espnow_dets_t pkt;
    memset(&pkt, 0, sizeof pkt);
    fill_hdr(&pkt.hdr, PKT_DETECTIONS);
    pkt.seq = seq;

    if (count > LD2450_MAX_TARGETS) count = LD2450_MAX_TARGETS;
    pkt.count = count;
    for (uint8_t i = 0; i < count; i++) {
        pkt.dets[i].x_mm      = (int16_t)dets[i].x_mm;
        pkt.dets[i].y_mm      = (int16_t)dets[i].y_mm;
        pkt.dets[i].speed_cms = (int16_t)dets[i].speed_cms;
        pkt.dets[i].range_mm  = (uint16_t)dets[i].range_mm;
    }

    esp_now_send(BROADCAST_MAC, (const uint8_t *)&pkt, sizeof pkt);
}

void espnow_link_send_heartbeat(uint32_t config_version, uint32_t uptime_s)
{
    espnow_heartbeat_t hb;
    memset(&hb, 0, sizeof hb);
    fill_hdr(&hb.hdr, PKT_HEARTBEAT);
    hb.config_version = config_version;
    hb.uptime_s = uptime_s;
    strncpy(hb.name, s_self_name, sizeof hb.name - 1);

    esp_now_send(BROADCAST_MAC, (const uint8_t *)&hb, sizeof hb);
}

uint8_t espnow_link_get_peers(peer_info_t *out, uint8_t cap)
{
    uint8_t n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint8_t i = 0; i < ROOM_MAX_NODES && n < cap; i++) {
        if (!s_peers[i].info.in_use) continue;
        /* Stale peers are reported too, so the UI can show "not seen for 40s"
         * rather than silently dropping a node still hanging on the wall. The
         * caller compares last_seen_ms against ESPNOW_PEER_TIMEOUT_MS. */
        out[n++] = s_peers[i].info;
    }
    xSemaphoreGive(s_lock);
    return n;
}

uint8_t espnow_link_collect_detections(detection_t *out, uint8_t cap,
                                       uint32_t t_ms, uint32_t max_age_ms)
{
    uint8_t n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint8_t i = 0; i < ROOM_MAX_NODES; i++) {
        peer_slot_t *p = &s_peers[i];
        if (!p->info.in_use || p->det_count == 0) continue;
        if ((uint32_t)(t_ms - p->det_at_ms) > max_age_ms) continue;

        for (uint8_t j = 0; j < p->det_count && n < cap; j++) {
            out[n++] = p->dets[j];
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}
