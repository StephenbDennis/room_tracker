#include "actions.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "actions";

#define MAX_PINS  (ROOM_MAX_ZONES * ZONE_MAX_ACTIONS)

typedef struct {
    uint8_t  pin;
    uint8_t  active_level;
    uint32_t holders;         /* bitmask of zone indices currently asserting */
    uint32_t pulse_until_ms;  /* 0 when not pulsing */
    bool     in_use;
} pin_state_t;

static pin_state_t s_pins[MAX_PINS];
static uint8_t     s_pin_count;
static char        s_self_id[ROOM_ID_LEN];

static pin_state_t *find_pin(uint8_t pin)
{
    for (uint8_t i = 0; i < s_pin_count; i++) {
        if (s_pins[i].in_use && s_pins[i].pin == pin) {
            return &s_pins[i];
        }
    }
    return NULL;
}

static pin_state_t *register_pin(uint8_t pin, uint8_t active_level)
{
    pin_state_t *p = find_pin(pin);
    if (p) {
        return p;
    }
    if (s_pin_count >= MAX_PINS) {
        ESP_LOGW(TAG, "pin table full, ignoring GPIO %u", pin);
        return NULL;
    }

    p = &s_pins[s_pin_count++];
    memset(p, 0, sizeof *p);
    p->pin = pin;
    p->active_level = active_level;
    p->in_use = true;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(pin, active_level ? 0 : 1);   /* start inactive */
    return p;
}

static void drive(pin_state_t *p)
{
    bool on = (p->holders != 0) || (p->pulse_until_ms != 0);
    gpio_set_level(p->pin, on ? p->active_level : !p->active_level);
}

void actions_init(const room_config_t *cfg, const char *self_node_id)
{
    memset(s_pins, 0, sizeof s_pins);
    s_pin_count = 0;
    strncpy(s_self_id, self_node_id, ROOM_ID_LEN - 1);
    s_self_id[ROOM_ID_LEN - 1] = '\0';

    for (uint8_t i = 0; i < cfg->zone_count; i++) {
        const zone_cfg_t *z = &cfg->zones[i];
        for (uint8_t j = 0; j < z->action_count; j++) {
            const zone_action_t *a = &z->actions[j];
            if (a->type != ACTION_GPIO) continue;
            if (strncmp(a->node_id, s_self_id, ROOM_ID_LEN) != 0) continue;
            register_pin(a->pin, a->active_level);
        }
    }
    ESP_LOGI(TAG, "node %s owns %u output pin(s)", s_self_id, s_pin_count);
}

void actions_on_zone_change(const zone_cfg_t *z, uint8_t zone_index,
                            bool active, uint32_t now_ms)
{
    if (zone_index >= 32) {
        return;   /* holder bitmask is 32 wide; ROOM_MAX_ZONES is 16 */
    }

    for (uint8_t j = 0; j < z->action_count; j++) {
        const zone_action_t *a = &z->actions[j];
        if (a->type != ACTION_GPIO) continue;
        if (strncmp(a->node_id, s_self_id, ROOM_ID_LEN) != 0) continue;

        pin_state_t *p = register_pin(a->pin, a->active_level);
        if (!p) continue;

        if (a->pulse) {
            /* Pulse fires on the rising edge only; the pin returns low on its
             * own regardless of how long the zone stays triggered. */
            if (active && a->pulse_ms > 0) {
                p->pulse_until_ms = now_ms + a->pulse_ms;
                if (p->pulse_until_ms == 0) p->pulse_until_ms = 1;   /* wrap */
            }
        } else {
            /* Latch. Several zones may share one pin, so track holders as a
             * bitmask: the pin stays asserted while ANY of them is active. */
            if (active) {
                p->holders |= (1u << zone_index);
            } else {
                p->holders &= ~(1u << zone_index);
            }
        }
        drive(p);
    }
}

void actions_tick(uint32_t now_ms)
{
    for (uint8_t i = 0; i < s_pin_count; i++) {
        pin_state_t *p = &s_pins[i];
        if (!p->in_use || p->pulse_until_ms == 0) continue;

        /* Signed comparison of the difference handles the uint32 wrap. */
        if ((int32_t)(now_ms - p->pulse_until_ms) >= 0) {
            p->pulse_until_ms = 0;
            drive(p);
        }
    }
}

void actions_all_off(void)
{
    for (uint8_t i = 0; i < s_pin_count; i++) {
        s_pins[i].holders = 0;
        s_pins[i].pulse_until_ms = 0;
        drive(&s_pins[i]);
    }
}

void actions_test_pulse(uint8_t pin, uint8_t active_level, uint32_t ms,
                        uint32_t now_ms)
{
    pin_state_t *p = register_pin(pin, active_level);
    if (!p) return;
    p->pulse_until_ms = now_ms + (ms ? ms : 500);
    if (p->pulse_until_ms == 0) p->pulse_until_ms = 1;
    drive(p);
    ESP_LOGI(TAG, "test pulse GPIO %u for %u ms", pin, (unsigned)(ms ? ms : 500));
}
