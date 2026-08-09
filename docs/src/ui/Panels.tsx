import type {
  CountOp,
  MotionState,
  NetworkCfg,
  RoomConfig,
  SensorPose,
  UntriggerMode,
  Zone,
} from '../model/config';
import { loadNetworkDefaults, saveNetworkDefaults } from '../model/config';
import type { NodeStatus } from '../ble/codec';
import { useState } from 'react';

const num = (v: string, fallback: number) => {
  const n = Number(v);
  return Number.isFinite(n) ? n : fallback;
};

export function RoomPanel({
  config,
  onChange,
}: {
  config: RoomConfig;
  onChange: (c: RoomConfig) => void;
}) {
  return (
    <section className="panel">
      <h3>Room</h3>
      <div className="row">
        <label>
          Width (mm)
          <input
            type="number"
            value={config.room.w_mm}
            onChange={(e) =>
              onChange({
                ...config,
                room: { ...config.room, w_mm: num(e.target.value, 5000) },
              })
            }
          />
        </label>
        <label>
          Depth (mm)
          <input
            type="number"
            value={config.room.h_mm}
            onChange={(e) =>
              onChange({
                ...config,
                room: { ...config.room, h_mm: num(e.target.value, 4000) },
              })
            }
          />
        </label>
      </div>
      <p className="hint">Config version {config.version}</p>
    </section>
  );
}

export function SensorPanel({
  config,
  onChange,
}: {
  config: RoomConfig;
  onChange: (c: RoomConfig) => void;
}) {
  const sensor = config.sensor;
  const patch = (p: Partial<SensorPose>) =>
    onChange({ ...config, sensor: { ...sensor, ...p } });

  return (
    <section className="panel">
      <h3>Sensor</h3>
      <div className="row">
        <label>
          x (mm)
          <input
            type="number"
            value={Math.round(sensor.x_mm)}
            onChange={(e) => patch({ x_mm: num(e.target.value, 0) })}
          />
        </label>
        <label>
          y (mm)
          <input
            type="number"
            value={Math.round(sensor.y_mm)}
            onChange={(e) => patch({ y_mm: num(e.target.value, 0) })}
          />
        </label>
      </div>
      <label>
        Facing (deg, CCW from +x)
        <input
          type="range"
          min={0}
          max={359}
          value={Math.round(sensor.theta_deg)}
          onChange={(e) => patch({ theta_deg: num(e.target.value, 0) })}
        />
        <span className="mono">{Math.round(sensor.theta_deg)}&deg;</span>
      </label>
      <p className="hint">Drag the sensor on the plan to reposition it.</p>
    </section>
  );
}

export function ZonePanel({
  zone,
  config,
  onChange,
  onDelete,
}: {
  zone: Zone;
  config: RoomConfig;
  onChange: (c: RoomConfig) => void;
  onDelete: () => void;
}) {
  const patch = (p: Partial<Zone>) =>
    onChange({
      ...config,
      zones: config.zones.map((z) => (z.id === zone.id ? { ...z, ...p } : z)),
    });

  const states: MotionState[] = ['any', 'moving', 'stopped'];

  return (
    <section className="panel">
      <h3>Event box</h3>
      <label>
        Name
        <input value={zone.name} onChange={(e) => patch({ name: e.target.value })} />
      </label>

      <div className="row">
        <label>
          Width (mm)
          <input
            type="number"
            value={Math.round(zone.rect.w)}
            onChange={(e) =>
              patch({ rect: { ...zone.rect, w: num(e.target.value, 1000) } })
            }
          />
        </label>
        <label>
          Height (mm)
          <input
            type="number"
            value={Math.round(zone.rect.h)}
            onChange={(e) =>
              patch({ rect: { ...zone.rect, h: num(e.target.value, 1000) } })
            }
          />
        </label>
      </div>
      <label>
        Rotation (deg)
        <input
          type="range"
          min={0}
          max={179}
          value={Math.round(zone.rect.rot_deg)}
          onChange={(e) =>
            patch({ rect: { ...zone.rect, rot_deg: num(e.target.value, 0) } })
          }
        />
        <span className="mono">{Math.round(zone.rect.rot_deg)}&deg;</span>
      </label>

      <h4>Trigger when</h4>
      <div className="row">
        <label>
          Targets
          <select
            value={zone.trigger.states[0] ?? 'any'}
            onChange={(e) =>
              patch({
                trigger: {
                  ...zone.trigger,
                  states: [e.target.value as MotionState],
                },
              })
            }
          >
            {states.map((s) => (
              <option key={s} value={s}>
                {s}
              </option>
            ))}
          </select>
        </label>
        <label>
          Count
          <select
            value={zone.trigger.count_op}
            onChange={(e) =>
              patch({
                trigger: { ...zone.trigger, count_op: e.target.value as CountOp },
              })
            }
          >
            <option value=">=">at least</option>
            <option value="==">exactly</option>
            <option value="<=">at most</option>
          </select>
        </label>
        <label>
          n
          <input
            type="number"
            min={0}
            value={zone.trigger.count_n}
            onChange={(e) =>
              patch({
                trigger: { ...zone.trigger, count_n: num(e.target.value, 1) },
              })
            }
          />
        </label>
      </div>
      <label>
        Entry debounce (ms)
        <input
          type="number"
          min={0}
          step={100}
          value={zone.trigger.on_delay_ms}
          onChange={(e) =>
            patch({
              trigger: { ...zone.trigger, on_delay_ms: num(e.target.value, 0) },
            })
          }
        />
      </label>
      <p className="hint">
        Radar output is jumpy. With no debounce a single stray frame at the
        boundary will fire the output.
      </p>

      <h4>Untrigger</h4>
      <label>
        Mode
        <select
          value={zone.untrigger.mode}
          onChange={(e) =>
            patch({
              untrigger: {
                ...zone.untrigger,
                mode: e.target.value as UntriggerMode,
              },
            })
          }
        >
          <option value="conditions_unmet">when conditions stop</option>
          <option value="timer">after a fixed time</option>
        </select>
      </label>
      <label>
        {zone.untrigger.mode === 'timer' ? 'Stay on for (ms)' : 'Hold after clear (ms)'}
        <input
          type="number"
          min={0}
          step={500}
          value={zone.untrigger.off_delay_ms}
          onChange={(e) =>
            patch({
              untrigger: {
                ...zone.untrigger,
                off_delay_ms: num(e.target.value, 0),
              },
            })
          }
        />
      </label>
      <label>
        Max on time (ms, 0 = no cap)
        <input
          type="number"
          min={0}
          step={1000}
          value={zone.untrigger.max_on_ms}
          onChange={(e) =>
            patch({
              untrigger: { ...zone.untrigger, max_on_ms: num(e.target.value, 0) },
            })
          }
        />
      </label>

      <h4>Actions</h4>
      {zone.actions.map((a, i) => (
        <div className="row action" key={i}>
          <label>
            GPIO
            <input
              type="number"
              min={0}
              max={48}
              value={a.pin}
              onChange={(e) => {
                const actions = [...zone.actions];
                actions[i] = { ...a, pin: num(e.target.value, 0) };
                patch({ actions });
              }}
            />
          </label>
          <label>
            Active
            <select
              value={a.active_level}
              onChange={(e) => {
                const actions = [...zone.actions];
                actions[i] = { ...a, active_level: num(e.target.value, 1) as 0 | 1 };
                patch({ actions });
              }}
            >
              <option value={1}>high</option>
              <option value={0}>low</option>
            </select>
          </label>
          <button
            className="danger small"
            onClick={() => patch({ actions: zone.actions.filter((_, j) => j !== i) })}
          >
            &times;
          </button>
        </div>
      ))}
      <button
        onClick={() =>
          patch({
            actions: [
              ...zone.actions,
              {
                type: 'gpio',
                pin: 12,
                active_level: 1,
                mode: 'latch',
                pulse_ms: 0,
              },
            ],
          })
        }
      >
        + GPIO action
      </button>

      <label className="check">
        <input
          type="checkbox"
          checked={zone.enabled}
          onChange={(e) => patch({ enabled: e.target.checked })}
        />
        Enabled
      </label>

      <button className="danger" onClick={onDelete}>
        Delete event box
      </button>
    </section>
  );
}

export function NetworkPanel({
  config,
  onChange,
}: {
  config: RoomConfig;
  onChange: (c: RoomConfig) => void;
}) {
  const net = config.network;
  const [remember, setRemember] = useState(() => loadNetworkDefaults() !== null);

  const patch = (p: Partial<NetworkCfg>) => {
    const next = { ...net, ...p };
    // Network settings describe the house, not this room, so mirror them out
    // of the room config as they are edited.
    if (remember) saveNetworkDefaults(next, true);
    onChange({ ...config, network: next });
  };

  return (
    <section className="panel">
      <h3>Home Assistant</h3>

      <label className="check">
        <input
          type="checkbox"
          checked={net.enabled}
          onChange={(e) => patch({ enabled: e.target.checked })}
        />
        Report zones over MQTT
      </label>

      {net.enabled && (
        <>
          <label>
            WiFi network
            <input
              value={net.wifi_ssid}
              onChange={(e) => patch({ wifi_ssid: e.target.value })}
            />
          </label>
          <label>
            WiFi password
            <input
              type="password"
              value={net.wifi_pass}
              placeholder={net.wifi_pass_set ? 'saved on device' : ''}
              onChange={(e) => patch({ wifi_pass: e.target.value })}
            />
          </label>

          <label>
            Broker
            <input
              value={net.mqtt_uri}
              placeholder="mqtt://homeassistant.local:1883"
              onChange={(e) => patch({ mqtt_uri: e.target.value })}
            />
          </label>
          <div className="row">
            <label>
              MQTT user
              <input
                value={net.mqtt_user}
                onChange={(e) => patch({ mqtt_user: e.target.value })}
              />
            </label>
            <label>
              MQTT password
              <input
                type="password"
                value={net.mqtt_pass}
                placeholder={net.mqtt_pass_set ? 'saved on device' : ''}
                onChange={(e) => patch({ mqtt_pass: e.target.value })}
              />
            </label>
          </div>

          <label className="check">
            <input
              type="checkbox"
              checked={net.publish_tracks}
              onChange={(e) => patch({ publish_tracks: e.target.checked })}
            />
            Also report room size and people
          </label>
          {net.publish_tracks && (
            <label>
              Position update interval (ms)
              <input
                type="number"
                min={200}
                step={100}
                value={net.tracks_interval_ms}
                onChange={(e) =>
                  patch({ tracks_interval_ms: num(e.target.value, 1000) })
                }
              />
            </label>
          )}

          <label className="check">
            <input
              type="checkbox"
              checked={remember}
              onChange={(e) => {
                setRemember(e.target.checked);
                saveNetworkDefaults(net, e.target.checked);
              }}
            />
            Remember for other boards
          </label>

          <p className="hint">
            Passwords are never read back from the device. Leave a field blank
            to keep the one already stored. Network changes need a reboot to
            take effect.
          </p>
          <p className="hint">
            Remembering keeps these in this browser so the next board is
            pre-filled. That includes the passwords, in browser storage — untick
            it if this machine is shared, and the other fields still carry over.
          </p>
        </>
      )}
    </section>
  );
}

export function DevicePanel({
  status,
  configVersion,
}: {
  status: NodeStatus | null;
  configVersion: number;
}) {
  if (!status) {
    return (
      <section className="panel">
        <h3>Device</h3>
        <p className="hint">Not connected.</p>
      </section>
    );
  }

  // The board persists its own copy, so an edit that never reached it leaves
  // the page showing a room the hardware is not actually running.
  const drift = status.config_version !== configVersion;

  return (
    <section className="panel">
      <h3>Device</h3>
      <table className="devices">
        <tbody>
          <tr>
            <td>
              <span className={drift ? 'dot dot-warn' : 'dot dot-ok'} />
            </td>
            <td className="mono">{status.name || status.node_id}</td>
            <td className="mono">v{status.config_version}</td>
            <td className="hint">
              {drift ? 'unsaved changes' : `up ${Math.round(status.uptime_s)}s`}
            </td>
          </tr>
        </tbody>
      </table>
    </section>
  );
}
