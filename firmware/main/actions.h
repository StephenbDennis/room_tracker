/* Zone action execution. Today: drive a GPIO pin.
 *
 * The dispatch layer is deliberately indirect so MQTT/webhook actions can be
 * added later without touching the zone engine. */
#ifndef ACTIONS_H
#define ACTIONS_H

#include "config.h"

/* self_node_id identifies this board. Actions naming a different node are
 * ignored here — zones are room-level objects, but a GPIO pin lives on one
 * specific board. */
void actions_init(const room_config_t *cfg, const char *self_node_id);

/* Apply a zone's output change. zone_index must match the zone's position in
 * the config, and is used to arbitrate pins driven by more than one zone. */
void actions_on_zone_change(const zone_cfg_t *z, uint8_t zone_index,
                            bool active, uint32_t now_ms);

/* Call regularly (every loop tick) so pulse actions can time out. */
void actions_tick(uint32_t now_ms);

/* Drop every output low. Used on config reload and factory reset. */
void actions_all_off(void);

/* Manual output test from the webpage's COMMAND characteristic. */
void actions_test_pulse(uint8_t pin, uint8_t active_level, uint32_t ms,
                        uint32_t now_ms);

#endif /* ACTIONS_H */
