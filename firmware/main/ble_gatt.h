/* NimBLE GATT server: the webpage's only interface to the board.
 *
 * One board per room, so the browser talks to exactly one of these and it
 * serves the whole room: track state, zone outputs, and its own config. */
#ifndef BLE_GATT_H
#define BLE_GATT_H

#include "config.h"
#include "room_types.h"
#include "zones.h"

/* Config payloads exceed the ATT MTU, so both directions are chunked as
 *   [seq u16][total u16][bytes...]
 * with seq counting from 0 and total giving the chunk count. */
#define BLE_CHUNK_HDR_LEN  4

typedef enum {
    BLE_CMD_IDENTIFY      = 0x01,
    BLE_CMD_REBOOT        = 0x02,
    BLE_CMD_FACTORY_RESET = 0x03,
    BLE_CMD_GPIO_TEST     = 0x04,  /* [pin u8][level u8][ms u16] */
    BLE_CMD_SAVE          = 0x05,
} ble_cmd_t;

typedef struct {
    /* Called once a complete config blob has been reassembled. Return true to
     * accept; false marks the write as rejected. */
    bool (*on_config)(const char *json, size_t len);
    void (*on_command)(ble_cmd_t cmd, const uint8_t *args, size_t len);
} ble_gatt_cbs_t;

void ble_gatt_init(const char *device_name, const ble_gatt_cbs_t *cbs);

/* Publish the current config so CONFIG_READ can serve it. */
void ble_gatt_set_config_json(const char *json, size_t len);

/* Room track state, ~10 Hz while a client is subscribed. */
void ble_gatt_notify_tracks(const track_t *tracks, uint8_t count, uint32_t seq);

/* Zone outputs, sent only on change. */
void ble_gatt_notify_zone_state(const zone_cfg_t *zones,
                                const zone_state_t *states, uint8_t count);

/* Refresh the STATUS payload (served as JSON; read or notify). */
void ble_gatt_set_status(const char *node_id, const char *name,
                         uint32_t config_version, uint32_t uptime_s,
                         bool config_mode);

bool ble_gatt_is_connected(void);

/* Writes are refused unless the node is in config mode. */
void ble_gatt_set_config_mode(bool enabled);

#endif /* BLE_GATT_H */
