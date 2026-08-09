/* Room tracker: LD2450 -> tracking -> zones -> GPIO, with BLE for
 * configuration and visualisation.
 *
 * One board per room, owning that room's sensor, zones and outputs. The zone
 * engine and its outputs run entirely here; the webpage is a client, and
 * disconnecting it must not change what the GPIO pins do. */

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "actions.h"
#include "ble_gatt.h"
#include "config.h"
#include "fusion.h"
#include "ld2450.h"
#include "zones.h"

static const char *TAG = "app";

/* --- board wiring --- */
#define LD2450_UART        UART_NUM_1
#define LD2450_RX_PIN      17
#define LD2450_TX_PIN      18
#define CONFIG_BUTTON_PIN  0      /* BOOT */

#define UART_BUF_SIZE      1024
#define TICK_PERIOD_MS     100    /* the LD2450 reports at 10 Hz */
#define STATUS_MS          1000
#define CONFIG_MODE_MS     (5 * 60 * 1000)

static room_config_t s_cfg;
static fusion_t      s_fusion;
static zone_state_t  s_zone_states[ROOM_MAX_ZONES];
static char          s_node_id[ROOM_ID_LEN];
static char          s_node_name[24];
static ld2450_parser_t s_parser;
static uint32_t      s_seq;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* --- config plumbing --- */

static void apply_config(void)
{
    fusion_init(&s_fusion, &s_cfg.fusion);
    memset(s_zone_states, 0, sizeof s_zone_states);
    actions_all_off();
    actions_init(&s_cfg);

    char *json = malloc(CONFIG_JSON_MAX);
    if (json) {
        size_t n = config_to_json(&s_cfg, json, CONFIG_JSON_MAX);
        if (n) ble_gatt_set_config_json(json, n);
        free(json);
    }
    ESP_LOGI(TAG, "config v%u applied: %u zones",
             (unsigned)s_cfg.version, s_cfg.zone_count);
}

static bool on_config_written(const char *json, size_t len)
{
    room_config_t incoming;
    if (!config_from_json(json, len, &incoming)) {
        ESP_LOGW(TAG, "rejected malformed config");
        return false;
    }
    s_cfg = incoming;
    if (!config_save(&s_cfg)) {
        ESP_LOGW(TAG, "config accepted but NVS write failed");
    }
    apply_config();
    return true;
}

static void on_command(ble_cmd_t cmd, const uint8_t *args, size_t len)
{
    switch (cmd) {
    case BLE_CMD_IDENTIFY:
        ESP_LOGI(TAG, "identify");
        break;
    case BLE_CMD_REBOOT:
        ESP_LOGI(TAG, "reboot requested");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        break;
    case BLE_CMD_FACTORY_RESET:
        ESP_LOGW(TAG, "factory reset");
        config_defaults(&s_cfg);
        config_save(&s_cfg);
        apply_config();
        break;
    case BLE_CMD_GPIO_TEST:
        if (len >= 4) {
            uint16_t ms = (uint16_t)(args[2] | (args[3] << 8));
            actions_test_pulse(args[0], args[1], ms, now_ms());
        }
        break;
    case BLE_CMD_SAVE:
        config_save(&s_cfg);
        break;
    }
}

/* --- LD2450 UART --- */

static void uart_init(void)
{
    uart_config_t cfg = {
        .baud_rate = LD2450_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LD2450_UART, UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LD2450_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LD2450_UART, LD2450_TX_PIN, LD2450_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    /* uart_set_pin() only routes the signal; it leaves the pull resistors
     * alone. An unpowered or disconnected LD2450 therefore leaves RX floating,
     * and the drifting input keeps tripping the start-bit detector -- which
     * reads as a steady trickle of rail-to-rail bytes that is easy to mistake
     * for real data at the wrong baud. Idle-high makes "nothing connected"
     * report as silence instead. */
    ESP_ERROR_CHECK(gpio_set_pull_mode(LD2450_RX_PIN, GPIO_PULLUP_ONLY));
}

static void send_ld2450_command(uint16_t word, const uint8_t *value, size_t vlen)
{
    uint8_t buf[32];
    size_t n = ld2450_build_command(buf, sizeof buf, word, value, vlen);
    if (n) {
        uart_write_bytes(LD2450_UART, buf, n);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void ld2450_configure(void)
{
    const uint8_t on[2]  = { 0x01, 0x00 };
    const uint8_t off[2] = { 0x00, 0x00 };

    send_ld2450_command(LD2450_CMD_ENABLE_CONFIG, on, 2);
    send_ld2450_command(LD2450_CMD_MULTI_TARGET, NULL, 0);
    /* The module's own Bluetooth is a needless 2.4 GHz emitter competing with
     * the ESP32's radio, which already has BLE and ESP-NOW to schedule. */
    send_ld2450_command(LD2450_CMD_BLUETOOTH, off, 2);
    send_ld2450_command(LD2450_CMD_END_CONFIG, NULL, 0);

    ESP_LOGI(TAG, "LD2450 set to multi-target, module BT off");
}

/* Drain the UART and return the newest complete frame, if any. Only the latest
 * matters: an older queued frame is stale data about a moving person. */
static bool read_latest_frame(ld2450_frame_t *out)
{
    uint8_t buf[UART_BUF_SIZE];
    bool got = false;

    int n = uart_read_bytes(LD2450_UART, buf, sizeof buf, 0);
    if (n <= 0) {
        return false;
    }

    size_t off = 0;
    while (off < (size_t)n) {
        size_t consumed = 0;
        ld2450_frame_t f;
        if (ld2450_parser_feed(&s_parser, buf + off, n - off, &consumed, &f)) {
            *out = f;
            got = true;
        }
        if (consumed == 0) break;
        off += consumed;
    }
    return got;
}

/* --- main loop --- */

static void tracker_task(void *arg)
{
    uint32_t last_status = 0;
    detection_t dets[FUSION_MAX_DETECTIONS];
    track_t     tracks[ROOM_MAX_TRACKS];
    zone_event_t events[ROOM_MAX_ZONES];

    for (;;) {
        uint32_t t = now_ms();

        /* Config mode closes automatically so a board left on a wall is not
         * permanently open to reconfiguration by anyone in range. */
        bool button = gpio_get_level(CONFIG_BUTTON_PIN) == 0;
        ble_gatt_set_config_mode(button || t < CONFIG_MODE_MS);

        uint8_t n_dets = 0;

        ld2450_frame_t frame;
        if (read_latest_frame(&frame)) {
            for (uint8_t i = 0; i < frame.count &&
                                n_dets < FUSION_MAX_DETECTIONS; i++) {
                fusion_transform(&s_cfg.sensor, &frame.targets[i],
                                 &dets[n_dets++]);
            }
        }

        fusion_update(&s_fusion, dets, n_dets, t);
        uint8_t n_tracks = fusion_get_tracks(&s_fusion, tracks, ROOM_MAX_TRACKS);

        uint8_t n_events = zones_update_all(s_cfg.zones, s_zone_states,
                                            s_cfg.zone_count, tracks, n_tracks,
                                            t, events, ROOM_MAX_ZONES);
        for (uint8_t i = 0; i < n_events; i++) {
            uint8_t zi = events[i].zone_index;
            actions_on_zone_change(&s_cfg.zones[zi], zi, events[i].active, t);
            ESP_LOGI(TAG, "zone '%s' %s (%u inside)",
                     s_cfg.zones[zi].name,
                     events[i].active ? "TRIGGERED" : "released",
                     s_zone_states[zi].match_count);
        }
        actions_tick(t);

        ble_gatt_notify_tracks(tracks, n_tracks, s_seq++);
        if (n_events > 0) {
            ble_gatt_notify_zone_state(s_cfg.zones, s_zone_states,
                                       s_cfg.zone_count);
        }

        if (t - last_status >= STATUS_MS) {
            last_status = t;
            ble_gatt_set_status(s_node_id, s_node_name, s_cfg.version,
                                t / 1000, ble_gatt_is_connected());
        }

        vTaskDelay(pdMS_TO_TICKS(TICK_PERIOD_MS));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Identity comes from the MAC so a fresh board is addressable before it
     * has ever been configured. */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_node_id, sizeof s_node_id, "%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_node_name, sizeof s_node_name, "node-%s", s_node_id);
    ESP_LOGI(TAG, "node id %s", s_node_id);

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << CONFIG_BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);

    config_load(&s_cfg);

    uart_init();
    ld2450_configure();

    ble_gatt_cbs_t cbs = {
        .on_config  = on_config_written,
        .on_command = on_command,
    };
    ble_gatt_init(s_node_name, &cbs);

    apply_config();

    xTaskCreate(tracker_task, "tracker", 6144, NULL, 5, NULL);
}
