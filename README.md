# Room Tracker

Indoor presence tracking with ESP32-S3 nodes and HLK-LD2450 24 GHz mmWave radar,
configured from a static webpage over Web Bluetooth.

Draw a room, place sensors in it, draw event boxes, and have a box drive a GPIO
pin when the occupancy conditions you set are met.

```
  GitHub Pages (static)  --Web Bluetooth-->  ESP32-S3 node  --ESP-NOW-->  more nodes
   room + zone editor                        fusion + zones                  |
   live floor plan                           GPIO outputs                  LD2450
```

## What runs where

**Zone evaluation and GPIO run on the devices.** The webpage is a configuration
and visualisation client — close the browser and the automation keeps working.
That is the constraint the whole architecture is built around.

Every node runs the identical pipeline over the same merged detection set, so
there is no coordinator and no single point of failure. Nodes can briefly
disagree on track *identity* after packet loss; that is harmless, because zone
rules are expressed over counts and motion states, never over track IDs.

## Before you start

- **Web Bluetooth is Chrome/Edge desktop and Chrome Android only.** No iOS
  Safari, no Firefox. The simulator works everywhere.
- **The LD2450 tracks at most 3 targets per sensor.** Fusing several sensors
  raises the room total, not the per-sensor limit.
- **BLE and ESP-NOW share one radio.** Coexistence is enabled in
  `sdkconfig.defaults`; verify it under load before trusting a multi-node install.
- **mmWave loses genuinely motionless people.** "Stopped" rules need the hold
  windows, or they will flap.

## Layout

```
firmware/          ESP-IDF project
  main/
    ld2450.c       radar frame parsing            [pure C, host-tested]
    fusion.c       transform, merge, track        [pure C, host-tested]
    zones.c        trigger/untrigger machine      [pure C, host-tested]
    config.c       JSON <-> struct, NVS
    actions.c      GPIO outputs
    ble_gatt.c     NimBLE server
    espnow_link.c  peer heartbeat + detections
    app_main.c     wiring and the 10 Hz loop
  test/            host tests, plain gcc
web/               Vite + TypeScript + React configurator
  src/sim/         offline simulator — no hardware needed
docs/protocol.md   every wire format, in one place
```

`ld2450.c`, `fusion.c` and `zones.c` deliberately have **zero ESP-IDF
dependencies**. The three modules most likely to harbour bugs — sign decoding,
geometry, hysteresis timing — are testable with plain `gcc` in milliseconds
instead of via flash-and-squint.

## Firmware

```bash
cd firmware/test && make          # host tests, no toolchain needed

cd firmware                       # requires ESP-IDF v5.x
idf.py set-target esp32s3
idf.py build flash monitor
```

Wiring, per node:

| LD2450 | ESP32-S3 |
|---|---|
| TX | GPIO 17 (RX) |
| RX | GPIO 18 (TX) |
| 5V | 5V |
| GND | GND |

Pins are at the top of `firmware/main/app_main.c`.

Each node derives its ID from its MAC (last three bytes) and registers itself in
the room layout on first boot, so a fresh board is addressable before it has
been configured.

## Web app

```bash
cd web
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

1. Set the room size, drag sensors into place, set each one's facing.
2. Add event boxes; drag to move, corner handle to resize.
3. Set trigger conditions (motion state, count) and untrigger behaviour.
4. Add a GPIO action, choosing which node owns the pin.
5. *Connect sensor*, then *Push config to device*.

Nodes are configured one at a time. The device panel shows every node it hears
over ESP-NOW along with each one's config version, so a node you forgot to
update shows up as a badge rather than a silent inconsistency.

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
3. Two nodes, one person, overlapping coverage: assert one fused track, not two.
4. Meter the GPIO pin through an entry/exit cycle; verify the delay timings.
5. **Kill the browser mid-session and confirm GPIO keeps firing.** This is the
   assertion that validates the on-device architecture.
6. Run 10 Hz notify plus ESP-NOW broadcast for 30 minutes; watch for coexistence
   dropouts.
