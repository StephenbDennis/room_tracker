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
