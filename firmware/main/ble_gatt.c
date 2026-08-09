#include "ble_gatt.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";

/* Base f0d2a450-1e4b-4c3a-9d1f-0000000000NN, matching docs/src/ble/uuids.ts.
 * NimBLE stores 128-bit UUIDs little-endian, so the array below is the UUID
 * byte-reversed: `last` comes first and the f0-d2 prefix comes last. */
#define UUID128_BASE(last)                                        \
    BLE_UUID128_INIT((last), 0x00, 0x00, 0x00, 0x00, 0x00,        \
                     0x1f, 0x9d, 0x3a, 0x4c, 0x4b, 0x1e,          \
                     0x50, 0xa4, 0xd2, 0xf0)

static const ble_uuid128_t SVC_UUID          = UUID128_BASE(0x00);
static const ble_uuid128_t CHR_CONFIG_WRITE  = UUID128_BASE(0x01);
static const ble_uuid128_t CHR_CONFIG_READ   = UUID128_BASE(0x02);
static const ble_uuid128_t CHR_TRACKS        = UUID128_BASE(0x03);
static const ble_uuid128_t CHR_ZONE_STATE    = UUID128_BASE(0x04);
static const ble_uuid128_t CHR_STATUS        = UUID128_BASE(0x05);
static const ble_uuid128_t CHR_COMMAND       = UUID128_BASE(0x06);

static ble_gatt_cbs_t s_cbs;
static uint8_t        s_addr_type;
static uint16_t       s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool           s_config_mode = true;

static uint16_t s_h_tracks, s_h_zone, s_h_status;

/* Reassembly buffer for inbound config writes. */
static char     s_cfg_in[CONFIG_JSON_MAX];
static size_t   s_cfg_in_len;
static uint16_t s_cfg_in_next_seq;

/* Outbound copies, served on read. */
static char   s_cfg_out[CONFIG_JSON_MAX];
static size_t s_cfg_out_len;
static char   s_status[1024];
static size_t s_status_len;

static void advertise(void);

/* ---------- characteristic handlers ---------- */

static int chr_config_write(uint16_t conn, uint16_t attr,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (!s_config_mode) {
        ESP_LOGW(TAG, "config write refused: not in config mode");
        return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < BLE_CHUNK_HDR_LEN) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t buf[512];
    if (len > sizeof buf) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint16_t got = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &got) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t seq   = (uint16_t)(buf[0] | (buf[1] << 8));
    uint16_t total = (uint16_t)(buf[2] | (buf[3] << 8));
    const uint8_t *payload = buf + BLE_CHUNK_HDR_LEN;
    size_t plen = got - BLE_CHUNK_HDR_LEN;

    if (seq == 0) {
        s_cfg_in_len = 0;
        s_cfg_in_next_seq = 0;
    } else if (seq != s_cfg_in_next_seq) {
        /* A dropped chunk would otherwise splice two configs together. */
        ESP_LOGW(TAG, "config chunk %u out of order (expected %u); resetting",
                 seq, s_cfg_in_next_seq);
        s_cfg_in_len = 0;
        s_cfg_in_next_seq = 0;
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (s_cfg_in_len + plen >= sizeof s_cfg_in) {
        ESP_LOGW(TAG, "config exceeds %u bytes", (unsigned)sizeof s_cfg_in);
        s_cfg_in_len = 0;
        s_cfg_in_next_seq = 0;
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    memcpy(s_cfg_in + s_cfg_in_len, payload, plen);
    s_cfg_in_len += plen;
    s_cfg_in_next_seq = seq + 1;

    if (s_cfg_in_next_seq >= total) {
        s_cfg_in[s_cfg_in_len] = '\0';
        ESP_LOGI(TAG, "config received: %u bytes", (unsigned)s_cfg_in_len);
        bool ok = s_cbs.on_config ? s_cbs.on_config(s_cfg_in, s_cfg_in_len) : true;
        s_cfg_in_len = 0;
        s_cfg_in_next_seq = 0;
        if (!ok) {
            return BLE_ATT_ERR_UNLIKELY;
        }
    }
    return 0;
}

static int chr_config_read(uint16_t conn, uint16_t attr,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, s_cfg_out, s_cfg_out_len) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int chr_status_read(uint16_t conn, uint16_t attr,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, s_status, s_status_len) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int chr_command_write(uint16_t conn, uint16_t attr,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (!s_config_mode) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    }

    uint8_t buf[32];
    uint16_t got = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &got) != 0 || got < 1) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (s_cbs.on_command) {
        s_cbs.on_command((ble_cmd_t)buf[0], buf + 1, got - 1);
    }
    return 0;
}

static int chr_notify_only(uint16_t conn, uint16_t attr,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return 0;   /* value delivered exclusively via notifications */
}

static const struct ble_gatt_svc_def SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &CHR_CONFIG_WRITE.u,
              .access_cb = chr_config_write,
              .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &CHR_CONFIG_READ.u,
              .access_cb = chr_config_read,
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = &CHR_TRACKS.u,
              .access_cb = chr_notify_only,
              .val_handle = &s_h_tracks,
              .flags = BLE_GATT_CHR_F_NOTIFY },
            { .uuid = &CHR_ZONE_STATE.u,
              .access_cb = chr_notify_only,
              .val_handle = &s_h_zone,
              .flags = BLE_GATT_CHR_F_NOTIFY },
            { .uuid = &CHR_STATUS.u,
              .access_cb = chr_status_read,
              .val_handle = &s_h_status,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { .uuid = &CHR_COMMAND.u,
              .access_cb = chr_command_write,
              .flags = BLE_GATT_CHR_F_WRITE },
            { 0 },
        },
    },
    { 0 },
};

/* ---------- GAP ---------- */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "client connected");
        } else {
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "client disconnected (reason %d)",
                 event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        /* Zone logic and GPIO keep running; the browser is a client, never
         * the control loop. */
        advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU now %d", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void advertise(void)
{
    /* A legacy advertisement carries 31 bytes. Flags (3) plus the full 128-bit
     * service UUID (18) leaves 10, which "node-xxxxxx" does not fit into. The
     * UUID has to stay here because the web client scans with a service filter,
     * so the name rides in the scan response instead. */
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&SVC_UUID;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set advertisement fields (rc %d)", rc);
        return;
    }

    const char *name = ble_svc_gap_device_name();
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (uint8_t *)name;
    rsp.name_len = strlen(name);
    rsp.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set scan response fields (rc %d)", rc);
        return;
    }

    struct ble_gap_adv_params adv = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &adv, gap_event, NULL);
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_addr_type);
    advertise();
    ESP_LOGI(TAG, "advertising");
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_gatt_init(const char *device_name, const ble_gatt_cbs_t *cbs)
{
    s_cbs = *cbs;

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(SERVICES));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(SERVICES));
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(device_name));

    nimble_port_freertos_init(host_task);
}

/* ---------- publishing ---------- */

void ble_gatt_set_config_json(const char *json, size_t len)
{
    if (len >= sizeof s_cfg_out) len = sizeof s_cfg_out - 1;
    memcpy(s_cfg_out, json, len);
    s_cfg_out[len] = '\0';
    s_cfg_out_len = len;
}

bool ble_gatt_is_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

void ble_gatt_set_config_mode(bool enabled)
{
    if (s_config_mode != enabled) {
        ESP_LOGI(TAG, "config mode %s", enabled ? "OPEN" : "closed");
    }
    s_config_mode = enabled;
}

void ble_gatt_notify_tracks(const track_t *tracks, uint8_t count, uint32_t seq)
{
    if (!ble_gatt_is_connected()) return;

    /* [seq u16][n u8][ id u8, x i16, y i16, vx i16, vy i16, state u8 ] * n */
    uint8_t buf[3 + ROOM_MAX_TRACKS * 10];
    size_t o = 0;
    buf[o++] = (uint8_t)(seq & 0xFF);
    buf[o++] = (uint8_t)(seq >> 8);
    buf[o++] = count;

    for (uint8_t i = 0; i < count; i++) {
        const track_t *t = &tracks[i];
        int16_t x  = (int16_t)t->x_mm;
        int16_t y  = (int16_t)t->y_mm;
        int16_t vx = (int16_t)t->vx_mms;
        int16_t vy = (int16_t)t->vy_mms;
        buf[o++] = t->id;
        buf[o++] = x & 0xFF;  buf[o++] = (x >> 8) & 0xFF;
        buf[o++] = y & 0xFF;  buf[o++] = (y >> 8) & 0xFF;
        buf[o++] = vx & 0xFF; buf[o++] = (vx >> 8) & 0xFF;
        buf[o++] = vy & 0xFF; buf[o++] = (vy >> 8) & 0xFF;
        buf[o++] = (uint8_t)t->motion;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, o);
    if (om) {
        ble_gatts_notify_custom(s_conn_handle, s_h_tracks, om);
    }
}

void ble_gatt_notify_zone_state(const zone_cfg_t *zones,
                                const zone_state_t *states, uint8_t count)
{
    if (!ble_gatt_is_connected()) return;

    /* [n u8][ index u8, active u8, count u8 ] * n */
    uint8_t buf[1 + ROOM_MAX_ZONES * 3];
    size_t o = 0;
    buf[o++] = count;
    for (uint8_t i = 0; i < count; i++) {
        buf[o++] = i;
        buf[o++] = zone_is_active(&states[i]) ? 1 : 0;
        buf[o++] = states[i].match_count;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, o);
    if (om) {
        ble_gatts_notify_custom(s_conn_handle, s_h_zone, om);
    }
}

void ble_gatt_set_status(const char *node_id, const char *name,
                         uint32_t config_version, uint32_t uptime_s,
                         bool config_mode)
{
    /* JSON: STATUS is read rarely, so legibility beats compactness here. */
    int n = snprintf(s_status, sizeof s_status,
        "{\"node_id\":\"%s\",\"name\":\"%s\",\"config_version\":%u,"
        "\"uptime_s\":%u,\"config_mode\":%s}",
        node_id, name, (unsigned)config_version, (unsigned)uptime_s,
        config_mode ? "true" : "false");

    s_status_len = (n > 0 && n < (int)sizeof s_status) ? (size_t)n : 0;

    if (ble_gatt_is_connected() && s_status_len > 0) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(s_status, s_status_len);
        if (om) {
            ble_gatts_notify_custom(s_conn_handle, s_h_status, om);
        }
    }
}
