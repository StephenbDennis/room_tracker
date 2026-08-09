# Wire protocols

Single source of truth for every byte crossing a boundary. Three implementations
must stay in step:

| Layer | Files |
|---|---|
| Radar UART | `firmware/main/ld2450.c` |
| BLE GATT | `firmware/main/ble_gatt.c`, `docs/src/ble/codec.ts` |
| Config JSON | `firmware/main/config.c`, `docs/src/model/config.ts` |

---

## 1. HLK-LD2450 UART

`256000` baud, 8N1, 10 frames/second — the datasheet default.

Baud is a persistent module setting, so a reconfigured unit can differ, and
`LD2450_BAUD` in `firmware/main/ld2450.h` must match whatever the module is
actually set to. A mismatch is silent — the parser simply never frames, and the
room reads as permanently empty. `ld2450_parser_t` counts `frames_ok` and
`bytes_discarded` for exactly this case; logging them separates a wrong baud
from a radar that genuinely sees nobody.

### Target frame — 30 bytes

```
AA FF 03 00 | target1 (8B) | target2 (8B) | target3 (8B) | 55 CC
```

Per target, little-endian:

| Offset | Field | Type |
|---|---|---|
| 0 | X | sign-magnitude int16, mm |
| 2 | Y | sign-magnitude int16, mm |
| 4 | speed | sign-magnitude int16, cm/s |
| 6 | distance resolution | uint16, mm |

An all-zero 8-byte block means the slot holds no target. There is **no
checksum**; integrity comes from the header/footer framing plus the fixed
length. The parser resynchronises byte-by-byte on the header.

Local axes: **+y forward** along boresight, **+x to the sensor's right**.

### Sign encoding — read this before touching the parser

X, Y and speed are **sign-magnitude, not two's complement**:

```c
int16_t mag = raw & 0x7FFF;
return (raw & 0x8000) ? mag : -mag;   // high bit SET means POSITIVE
```

Published sources contradict each other on the polarity. It is pinned by
decoding the datasheet's own example frame:

```
AA FF 03 00  0E 03 B1 86 10 00 40 01  ... 55 CC

X     = 0x030E = 782    high bit clear -> -782 mm
Y     = 0x86B1 = 34481  high bit set   -> +1713 mm
speed = 0x0010 = 16     high bit clear -> -16 cm/s
res   = 0x0140 = 320 mm
```

The inverted reading would put the target 1713 mm *behind* a sensor with no
rear lobe. `firmware/test/test_ld2450.c` asserts this exact frame; if that test
ever fails, the parser is wrong, not the test.

### Configuration commands

```
FD FC FB FA | len(u16 LE) | word(u16 LE) | value... | 04 03 02 01
```

`len` counts the command word plus its value — not the header, the length field
itself, or the footer.

| Function | Word | Value | `len` |
|---|---|---|---|
| Enable configuration | `FF 00` | `01 00` | 4 |
| End configuration | `FE 00` | — | 2 |
| Single-target mode | `80 00` | — | 2 |
| Multi-target mode | `90 00` | — | 2 |
| Query tracking mode | `91 00` | — | 2 |
| Bluetooth on / off | `A4 00` | `01 00` / `00 00` | 4 |
| Get MAC address | `A5 00` | `01 00` | 4 |

Every command must be bracketed by enable/end configuration. At boot the
firmware selects multi-target mode and **disables the module's own Bluetooth** —
it is a needless 2.4 GHz emitter competing with the ESP32's own BLE radio.

---

## 2. BLE GATT

Service `f0d2a450-1e4b-4c3a-9d1f-000000000000`. Characteristics take the same
base with the final byte varying.

| Byte | Characteristic | Properties |
|---|---|---|
| `01` | CONFIG_WRITE | write |
| `02` | CONFIG_READ | read |
| `03` | TRACKS | notify |
| `04` | ZONE_STATE | notify |
| `05` | STATUS | read, notify |
| `06` | COMMAND | write |

MTU is negotiated to 517.

### TRACKS — notify, ~10 Hz

```
[seq u16][n u8]  then n * 10 bytes:
  [id u8][x i16][y i16][vx i16][vy i16][motion u8]
```

Room coordinates in mm, velocity in mm/s, all little-endian.
`motion`: `0` unknown, `1` moving, `2` stopped.

This carries the **fused** room state, not one sensor's raw targets.

### ZONE_STATE — notify on change only

```
[n u8]  then n * 3 bytes:
  [zone_index u8][active u8][count u8]
```

### STATUS — read / notify, 1 Hz

JSON, because it is read rarely and legibility beats compactness:

```json
{
  "node_id": "a1b2c3",
  "name": "node-a1b2c3",
  "config_version": 7,
  "uptime_s": 4210,
  "config_mode": true
}
```

`config_version` is what lets the UI flag an edit that never reached the board.

### CONFIG_WRITE / CONFIG_READ — chunked

Config exceeds the ATT MTU, so both directions are chunked:

```
[seq u16][total u16][payload bytes]
```

`seq` counts from 0; `total` is the chunk count. The firmware **rejects
out-of-order chunks** and resets the reassembly buffer, so the client must write
sequentially and wait for each response. A dropped chunk would otherwise splice
two configs together. The web client uses a 180-byte payload, conservative
because Web Bluetooth does not expose the negotiated MTU.

### COMMAND — write

```
[cmd u8][args...]
```

| Cmd | Meaning | Args |
|---|---|---|
| `0x01` | Identify | — |
| `0x02` | Reboot | — |
| `0x03` | Factory reset | — |
| `0x04` | GPIO test pulse | `[pin u8][level u8][ms u16]` |
| `0x05` | Save to NVS | — |

Writes to CONFIG_WRITE and COMMAND are refused unless the node is in **config
mode**: the first 5 minutes after boot, or while the BOOT button is held.

---

## 3. Room configuration JSON

Mirrors `room_config_t` (`firmware/main/config.h`) and `RoomConfig`
(`docs/src/model/config.ts`).

```jsonc
{
  "version": 7,
  "room": { "w_mm": 5000, "h_mm": 4000 },
  "sensor": { "x_mm": 2500, "y_mm": 0, "theta_deg": 90 },
  "zones": [
    {
      "id": "z1", "name": "desk", "enabled": true,
      "rect": { "cx": 1200, "cy": 800, "w": 900, "h": 600, "rot_deg": 0 },
      "trigger": {
        "states": ["moving"],        // moving | stopped | any
        "count_op": ">=",            // >= | == | <=
        "count_n": 1,
        "on_delay_ms": 300
      },
      "untrigger": {
        "mode": "conditions_unmet",  // conditions_unmet | timer
        "off_delay_ms": 5000,
        "max_on_ms": 0
      },
      "actions": [
        { "type": "gpio", "pin": 12,
          "active_level": 1, "mode": "latch", "pulse_ms": 0 }
      ]
    }
  ],
  "fusion": {
    "assoc_gate_mm": 800, "coast_ms": 1000,
    "moving_thresh_mms": 100, "stopped_thresh_mms": 50,
    "stopped_hold_ms": 1000
  },
  "network": {
    "enabled": true,
    "wifi_ssid": "house",
    "wifi_pass": "",              // write-only; see below
    "mqtt_uri": "mqtt://homeassistant.local:1883",
    "mqtt_user": "roomtrack",
    "mqtt_pass": "",              // write-only
    "base_topic": "roomtrack",
    "discovery_prefix": "homeassistant"
  }
}
```

Notes:

- **`sensor`** is a single object, not a list: one sensor per room. Its
  `theta_deg` is the boresight bearing counter-clockwise from room +x.
- **`on_delay_ms`** debounces entry. Without it, one jittery frame at a boundary
  fires the output.
- **`max_on_ms`** is a safety cap. After it fires, the zone stays suppressed
  until the conditions clear at least once — otherwise a still-present target
  would immediately re-trigger and oscillate forever. The same suppression
  applies after a `timer`-mode release.
- **Passwords are write-only.** CONFIG_READ serves a redacted copy carrying
  `wifi_pass_set` and `mqtt_pass_set` booleans instead of the values, because
  anything that can reach CONFIG_READ in config mode could otherwise read the
  WiFi password straight back out. An empty password on write keeps the stored
  one, so the webpage can push a config it never had the secrets for. NVS holds
  the unredacted copy.
- The board's display name is **not** in this schema. The firmware reports its
  own name via STATUS; the web app keeps any user-assigned label locally.

## 4. MQTT / Home Assistant

Only active when `network.enabled` and a WiFi SSID are set; otherwise the
station is never started and the radio stays BLE-only.

| Topic | Payload | Retained |
|---|---|---|
| `<prefix>/binary_sensor/roomtrack_<node>_<zone>/config` | discovery JSON | yes |
| `<base>/<node>/zone/<zone>/state` | `ON` / `OFF` | yes |
| `<base>/<node>/availability` | `online` / `offline` | yes (LWT) |

`<zone>` is the zone id slugified to `[a-z0-9_-]`, since Home Assistant object
ids do not accept free text.

State is **retained** so Home Assistant recovers the current picture on restart
rather than waiting for someone to walk through a room. Availability is an MQTT
last will, so a board that drops off shows as unavailable instead of as an
empty room. Every zone shares one `device` block in its discovery payload, so
the entities group under a single device.

Removing a zone publishes an **empty retained payload** to its discovery topic,
which is how Home Assistant is told to delete an entity; without it the entity
outlives the box that created it.

---

### Zone state machine

```
IDLE --conditions met for on_delay_ms--> TRIGGERED
  ^                                          |
  |                                   conditions unmet
  +---- off_delay_ms elapsed ---- RELEASING <+
                                      |
                        conditions met again -> TRIGGERED
```

`max_on_ms` forces TRIGGERED/RELEASING to IDLE regardless. Re-entry during
RELEASING cancels the release, which is what stops someone pacing a boundary
from chattering the output.

Authoritative implementation: `firmware/main/zones.c`, under host test in
`firmware/test/test_zones.c`. `docs/src/sim/zoneEngine.ts` mirrors it for the
offline simulator; if the two disagree, the firmware is right.
