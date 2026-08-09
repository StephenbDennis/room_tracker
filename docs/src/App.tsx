import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { BleLink } from './ble/client';
import type { NodeStatus, Track, ZoneState } from './ble/codec';
import { bluetoothAvailable, type DeviceLink } from './ble/link';
import { Cmd } from './ble/uuids';
import {
  loadLocal,
  mergeNetworkDefaults,
  newZone,
  saveLocal,
  validateConfig,
  withBumpedVersion,
  type RoomConfig,
} from './model/config';
import { RoomCanvas, type Selection } from './render/RoomCanvas';
import { SimLink } from './sim/simulator';
import {
  DevicePanel,
  NetworkPanel,
  RoomPanel,
  SensorPanel,
  ZonePanel,
} from './ui/Panels';

export default function App() {
  const [config, setConfig] = useState<RoomConfig>(() => loadLocal());
  const [link, setLink] = useState<DeviceLink | null>(null);
  const [tracks, setTracks] = useState<Track[]>([]);
  const [zoneStates, setZoneStates] = useState<Map<number, ZoneState>>(new Map());
  const [status, setStatus] = useState<NodeStatus | null>(null);
  const [selected, setSelected] = useState<Selection>(null);
  const [error, setError] = useState<string | null>(null);
  const [dirty, setDirty] = useState(false);

  const configRef = useRef(config);
  const asideRef = useRef<HTMLElement>(null);
  configRef.current = config;

  useEffect(() => saveLocal(config), [config]);

  const wire = useCallback((l: DeviceLink) => {
    l.onTracks((f) => setTracks(f.tracks));
    l.onZoneState((zs) => setZoneStates(new Map(zs.map((z) => [z.index, z]))));
    l.onStatus(setStatus);
    l.onDisconnected(() => {
      setLink(null);
      setTracks([]);
      setZoneStates(new Map());
      // Otherwise the device panel keeps describing a board that is gone, and
      // `locked` below keeps reading its last-known config mode.
      setStatus(null);
    });
  }, []);

  async function connectBle() {
    setError(null);
    const l = new BleLink();
    wire(l);
    try {
      await l.connect();
      setLink(l);

      // Adopt whatever the device already holds, so a fresh browser does not
      // silently overwrite a working room with an empty local default.
      const remote = await l.readConfig();
      if (remote && validateConfig(remote)) {
        // A board fresh from a factory reset has no network settings. Fill in
        // the ones remembered from the last board so the WiFi and broker do
        // not have to be retyped for every room.
        const merged = mergeNetworkDefaults(remote);
        setConfig(merged);
        setDirty(JSON.stringify(merged) !== JSON.stringify(remote));
      }
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }

  async function connectSim() {
    setError(null);
    const l = new SimLink(configRef.current);
    wire(l);
    await l.connect();
    setLink(l);
  }

  // Keep the simulator's copy of the rules current as they are edited.
  useEffect(() => {
    if (link instanceof SimLink) link.setConfig(config);
  }, [config, link]);

  function disconnect() {
    link?.disconnect();
    setLink(null);
    setStatus(null);
  }

  async function pushConfig() {
    if (!link) return;
    setError(null);
    const next = withBumpedVersion(config);
    try {
      await link.writeConfig(next);
      setConfig(next);
      setDirty(false);
    } catch (e) {
      // Web Bluetooth surfaces the firmware's ATT error as an opaque "GATT
      // Error Unknown", so name the cause we can actually identify.
      setError(
        locked
          ? 'The device refused the write because it is locked. It accepts ' +
            'configuration only for the first 5 minutes after boot.'
          : e instanceof Error
            ? e.message
            : String(e),
      );
    }
  }

  async function rebootDevice() {
    if (!link) return;
    setError(null);
    try {
      await link.sendCommand(new Uint8Array([Cmd.Reboot]));
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }

  const update = useCallback((c: RoomConfig) => {
    setConfig(c);
    setDirty(true);
  }, []);

  function addZone() {
    const id = `z${Date.now().toString(36).slice(-5)}`;
    update({
      ...config,
      zones: [
        ...config.zones,
        newZone(id, config.room.w_mm / 2, config.room.h_mm / 2),
      ],
    });
    setSelected({ kind: 'zone', id });
  }

  function exportJson() {
    const blob = new Blob([JSON.stringify(config, null, 2)], {
      type: 'application/json',
    });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'room-config.json';
    a.click();
    URL.revokeObjectURL(a.href);
  }

  function importJson(file: File) {
    file.text().then((text) => {
      try {
        const parsed: unknown = JSON.parse(text);
        if (validateConfig(parsed)) {
          update(parsed);
        } else {
          setError('That file is not a valid room configuration.');
        }
      } catch {
        setError('Could not parse that file as JSON.');
      }
    });
  }

  // Stacked on a phone, the editor sits below the fold: tapping a zone would
  // otherwise look like it did nothing at all.
  useEffect(() => {
    if (!selected) return;
    if (!window.matchMedia('(max-width: 760px)').matches) return;
    asideRef.current?.scrollIntoView({ behavior: 'smooth', block: 'start' });
  }, [selected]);

  const selectedZone = useMemo(
    () =>
      selected?.kind === 'zone'
        ? config.zones.find((z) => z.id === selected.id)
        : undefined,
    [selected, config.zones],
  );

  const noBluetooth = !bluetoothAvailable();

  // The firmware closes config mode 5 minutes after boot unless BOOT is held,
  // and refuses writes after that. Without this the push button looks
  // available right up until it fails with an opaque GATT error.
  const locked = link !== null && status !== null && !status.config_mode;

  return (
    <div className="app">
      <header>
        <h1>Room Tracker</h1>
        <div className="actions">
          {link ? (
            <>
              <span className="badge ok">{link.label}</span>
              <button onClick={pushConfig} disabled={!dirty || locked}>
                {locked
                  ? 'Device locked'
                  : dirty
                    ? 'Push config to device'
                    : 'Config in sync'}
              </button>
              <button onClick={disconnect}>Disconnect</button>
            </>
          ) : (
            <>
              <button onClick={connectBle} disabled={noBluetooth}>
                Connect sensor
              </button>
              <button onClick={connectSim}>Run simulator</button>
            </>
          )}
        </div>
      </header>

      {noBluetooth && !link && (
        <div className="notice">
          This browser has no Web Bluetooth support. Use Chrome or Edge on
          desktop, or Chrome on Android — iOS Safari and Firefox cannot connect
          to the sensors. The simulator works everywhere.
        </div>
      )}
      {locked && (
        <div className="notice">
          This device is locked. It accepts configuration only for the first
          5 minutes after boot, so nothing left on a wall can be reconfigured
          by anyone in range. Hold its BOOT button to unlock, or{' '}
          <button className="small" onClick={rebootDevice}>
            reboot it
          </button>{' '}
          for a fresh window.
        </div>
      )}
      {error && <div className="notice error">{error}</div>}

      <main>
        <div className="canvas-wrap">
          <RoomCanvas
            config={config}
            tracks={tracks}
            zoneStates={zoneStates}
            selected={selected}
            onSelect={setSelected}
            onChange={update}
          />
        </div>

        <aside ref={asideRef}>
          <div className="toolbar">
            <button onClick={addZone}>+ Event box</button>
            <button onClick={exportJson}>Export</button>
            <label className="file-btn">
              Import
              <input
                type="file"
                accept="application/json"
                onChange={(e) => {
                  const f = e.target.files?.[0];
                  if (f) importJson(f);
                  e.target.value = '';
                }}
              />
            </label>
          </div>

          <RoomPanel config={config} onChange={update} />

          {selectedZone && (
            <ZonePanel
              zone={selectedZone}
              config={config}
              onChange={update}
              onDelete={() => {
                update({
                  ...config,
                  zones: config.zones.filter((z) => z.id !== selectedZone.id),
                });
                setSelected(null);
              }}
            />
          )}

          {selected?.kind === 'sensor' && (
            <SensorPanel config={config} onChange={update} />
          )}

          <NetworkPanel config={config} onChange={update} />

          <DevicePanel status={status} configVersion={config.version} />
        </aside>
      </main>
    </div>
  );
}
