# Room Tracker

Indoor presence tracking with an ESP32-S3 and an HLK-LD2450 24 GHz mmWave radar,
configured from a static webpage over Web Bluetooth.

Draw a room, place the sensor in it, draw event boxes, and have a box drive a
GPIO pin when the occupancy conditions you set are met.

```
  GitHub Pages (static)  --Web Bluetooth-->  ESP32-S3 board
   room + zone editor                        tracking + zones
   live floor plan                           GPIO outputs  --UART--  LD2450
```

**One board per room**, owning that room's sensor, zones and outputs. Rooms are
independent: boards never talk to each other.

## What runs where

**Zone evaluation and GPIO run on the devices.** The webpage is a configuration
and visualisation client — close the browser and the automation keeps working.
That is the constraint the whole architecture is built around.

Each board is self-contained: it reads its own radar, tracks the people in its
room, evaluates its own zones and drives its own pins. There is no coordinator,
no peer protocol and no shared state to get out of sync.

## Before you start

- **Web Bluetooth is Chrome/Edge desktop and Chrome Android only.** No iOS
  Safari, no Firefox. The simulator works everywhere.
- **The LD2450 tracks at most 3 targets.** That is a hard ceiling on how many
  people a room can distinguish.
- **mmWave loses genuinely motionless people.** "Stopped" rules need the hold
  windows, or they will flap.

## Layout

```
firmware/          ESP-IDF project
  main/
    ld2450.c       radar frame parsing            [pure C, host-tested]
    fusion.c       transform, associate, track    [pure C, host-tested]
    zones.c        trigger/untrigger machine      [pure C, host-tested]
    config.c       JSON <-> struct, NVS
    actions.c      GPIO outputs
    ble_gatt.c     NimBLE server
    net_ha.c       WiFi + MQTT reporting to Home Assistant
    app_main.c     wiring and the 10 Hz loop
  test/            host tests, plain gcc
docs/              Vite + TypeScript + React configurator
  src/sim/         offline simulator — no hardware needed
info/protocol.md   every wire format, in one place
```

`ld2450.c`, `fusion.c` and `zones.c` deliberately have **zero ESP-IDF
dependencies**. The three modules most likely to harbour bugs — sign decoding,
geometry, hysteresis timing — are testable with plain `gcc` in milliseconds
instead of via flash-and-squint.

## Firmware

```bash
cd firmware/test && make          # host tests, no toolchain needed

cd firmware                       # requires ESP-IDF v6.x
idf.py set-target esp32s3
idf.py build flash monitor
```

`sdkconfig` is generated and gitignored, and **`sdkconfig.defaults` only seeds a
new one**. After pulling changes that touch it — flash size, partition table,
WiFi, the NimBLE task stack — delete `firmware/sdkconfig` and rebuild, or the
old values silently persist.

The board is 16 MB and the app partition is 4 MB (`partitions.csv`); ESP-IDF
defaults to 2 MB and a 1 MB app, which WiFi plus NimBLE does not fit in.

Wiring:

| LD2450 | ESP32-S3 |
|---|---|
| TX | GPIO 17 (RX) |
| RX | GPIO 18 (TX) |
| 5V | 5V |
| GND | GND |

Pins are at the top of `firmware/main/app_main.c`.

The board derives its ID from its MAC (last three bytes) and advertises as
`node-<id>`, so a fresh board is addressable before it has been configured.

## Web app

```bash
cd docs
npm install
npm run dev        # http://localhost:5173
npm test
npm run build
```

Push to `main` and the Pages workflow deploys it. `vite.config.ts` uses a
relative base, so it works under any repository path without edits.

**Start with the simulator.** Click *Run simulator* and you get synthetic people
walking the room with realistic jitter, driving the same zone rules the firmware
runs. The whole UI is developable with nothing powered on.

## Configuring

Configuration is **data**, never a reflash. Reflash only to add new capability,
such as an action type beyond GPIO.

1. Set the room size, drag the sensor into place, set its facing.
2. Add event boxes; drag to move, corner handle to resize.
3. Set trigger conditions (motion state, count) and untrigger behaviour.
4. Add a GPIO action and pick the pin.
5. *Connect sensor*, then *Push config to device*.

The device panel shows the board's stored config version, so an edit that never
reached it shows up as a badge rather than a silent inconsistency.

### Home Assistant

Optional, and off until you fill it in — with reporting disabled the radio
stays BLE-only.

**In Home Assistant first:** install and start the **Mosquitto broker** add-on,
add the **MQTT** integration (usually offered automatically once Mosquitto is
running), and create a user under *Settings → People → Users* for the board.
The add-on authenticates against Home Assistant accounts.

**Then on the board**, connected over BLE:

1. Open **Home Assistant** in the side panel and tick *Report zones over MQTT*.
2. *Scan for networks*, pick yours, and enter the WiFi password.
3. Set the broker to `mqtt://<your-ha-ip>:1883` and enter the MQTT user and
   password. This is **not** the Home Assistant web address: that is
   `http://<host>:8123`, while the broker is a separate service on 1883.
   `homeassistant.local` resolves too — lwIP answers `.local` by mDNS — but an
   IP survives VLANs and flaky multicast.
4. Optionally tick *Also report room size and people*.
5. *Push config to device*, then **reboot it**: network settings are read only
   at startup.

The monitor should then show `wifi up`, `MQTT connected` and
`published discovery for N zone(s)`. Each event box arrives in Home Assistant
as an occupancy `binary_sensor`, grouped under one device, with no YAML.

**If nothing appears, check you have at least one event box.** Discovery
publishes an entity per zone, so a board with no zones and people reporting off
publishes nothing at all and never shows up — which looks exactly like a broker
problem and is not one.

Network settings belong to the house rather than to a room, so the web app can
remember them: tick *Remember for other boards* and the next board you connect
to is pre-filled instead of retyped. Anything the board already holds wins, so
this never overwrites a working setup.

State is retained, so Home Assistant recovers the current picture on restart
instead of waiting for someone to walk through the room, and an MQTT last will
marks the node unavailable if it drops off rather than reporting an empty room.

Passwords are write-only: the device serves a redacted config over BLE and
reports only whether one is stored, so anyone in Bluetooth range during the
config window cannot read your WiFi password back out.

### Tuning that matters

- **Entry debounce** (`on_delay_ms`, default 300 ms) — without it a single
  jittery frame at a boundary fires the output. Do not set it to zero.
- **Hold after clear** (`off_delay_ms`) — how long the output stays on after the
  box empties. Re-entry during the hold cancels the release.
- **Max on time** (`max_on_ms`) — safety cap. After it fires the zone stays
  suppressed until conditions clear, so a stuck target cannot latch a pin
  forever.

## Bring-up checklist

1. Log raw UART at 256000 and confirm a 10 Hz frame cadence.
2. Walk a known path; confirm the rendered position tracks reality to ~200 mm.
3. Two people in the room: assert two distinct tracks, not one.
4. Meter the GPIO pin through an entry/exit cycle; verify the delay timings.
5. **Kill the browser mid-session and confirm GPIO keeps firing.** This is the
   assertion that validates the on-device architecture.
