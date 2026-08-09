/* Room configuration schema.
 *
 * This mirrors room_config_t in firmware/main/config.h field for field. It is
 * DATA the webpage writes over BLE — changing a room never requires a reflash.
 * If you change anything here, change config.c and docs/protocol.md too. */

export type MotionState = 'moving' | 'stopped' | 'any';
export type CountOp = '>=' | '==' | '<=';
export type UntriggerMode = 'conditions_unmet' | 'timer';
export type ActionMode = 'latch' | 'pulse';

/** Where the room's one sensor sits. */
export interface SensorPose {
  x_mm: number;
  y_mm: number;
  /** Boresight bearing, counter-clockwise from room +x. */
  theta_deg: number;
}

export interface ZoneRect {
  cx: number;
  cy: number;
  w: number;
  h: number;
  rot_deg: number;
}

export interface GpioAction {
  type: 'gpio';
  pin: number;
  active_level: 0 | 1;
  mode: ActionMode;
  pulse_ms: number;
}

export interface ZoneTrigger {
  states: MotionState[];
  count_op: CountOp;
  count_n: number;
  /** Entry debounce. Without it a single jittery frame fires the output. */
  on_delay_ms: number;
}

export interface ZoneUntrigger {
  mode: UntriggerMode;
  off_delay_ms: number;
  /** Safety cap; 0 disables. Stops a stuck target latching a pin forever. */
  max_on_ms: number;
}

export interface Zone {
  id: string;
  name: string;
  enabled: boolean;
  rect: ZoneRect;
  trigger: ZoneTrigger;
  untrigger: ZoneUntrigger;
  actions: GpioAction[];
}

export interface FusionCfg {
  assoc_gate_mm: number;
  coast_ms: number;
  moving_thresh_mms: number;
  stopped_thresh_mms: number;
  stopped_hold_ms: number;
}

/* Passwords are write-only. The device serves a redacted config over BLE, so
 * it reports whether one is stored rather than its value; sending an empty
 * string keeps whatever the device already has. */
export interface NetworkCfg {
  enabled: boolean;
  wifi_ssid: string;
  wifi_pass: string;
  mqtt_uri: string;
  mqtt_user: string;
  mqtt_pass: string;
  base_topic: string;
  discovery_prefix: string;
  /** People count, positions and room size as an HA sensor. */
  publish_tracks: boolean;
  /** Floor for position updates. Tracks move at 10 Hz; that rate would bury
   *  Home Assistant's recorder for no benefit. */
  tracks_interval_ms: number;
  wifi_pass_set?: boolean;
  mqtt_pass_set?: boolean;
}

export interface RoomConfig {
  version: number;
  room: { w_mm: number; h_mm: number };
  sensor: SensorPose;
  zones: Zone[];
  fusion: FusionCfg;
  network: NetworkCfg;
}

export const MAX_ZONES = 16;

export function defaultNetwork(): NetworkCfg {
  return {
    enabled: false,
    wifi_ssid: '',
    wifi_pass: '',
    mqtt_uri: 'mqtt://homeassistant.local:1883',
    mqtt_user: '',
    mqtt_pass: '',
    base_topic: 'roomtrack',
    discovery_prefix: 'homeassistant',
    publish_tracks: false,
    tracks_interval_ms: 1000,
  };
}

/* Network settings are a property of the house, not of a room, so they live
 * outside RoomConfig in storage: set the WiFi and broker once and every board
 * you connect to afterwards is pre-filled. The room config still carries them
 * because that is what the firmware reads. */
const NETWORK_KEY = 'roomtrack.network';

export function loadNetworkDefaults(): NetworkCfg | null {
  try {
    const raw = localStorage.getItem(NETWORK_KEY);
    if (!raw) return null;
    const parsed = JSON.parse(raw) as Partial<NetworkCfg>;
    if (typeof parsed !== 'object' || parsed === null) return null;
    return { ...defaultNetwork(), ...parsed };
  } catch {
    return null;
  }
}

/* keepSecrets=false drops the passwords before they touch localStorage, where
 * they would sit readable by any script on this origin and outlive the tab. */
export function saveNetworkDefaults(net: NetworkCfg, keepSecrets: boolean): void {
  try {
    const out: NetworkCfg = keepSecrets
      ? net
      : { ...net, wifi_pass: '', mqtt_pass: '' };
    localStorage.setItem(NETWORK_KEY, JSON.stringify(out));
  } catch {
    /* storage blocked; the device copy is still the source of truth */
  }
}

/* Fill a device's blank network fields from the remembered ones. Anything the
 * board already has wins, so connecting never clobbers a working setup. */
export function mergeNetworkDefaults(cfg: RoomConfig): RoomConfig {
  const saved = loadNetworkDefaults();
  if (!saved) return cfg;

  const n = cfg.network;
  return {
    ...cfg,
    network: {
      ...n,
      wifi_ssid: n.wifi_ssid || saved.wifi_ssid,
      wifi_pass: n.wifi_pass || (n.wifi_pass_set ? '' : saved.wifi_pass),
      mqtt_uri: n.mqtt_uri || saved.mqtt_uri,
      mqtt_user: n.mqtt_user || saved.mqtt_user,
      mqtt_pass: n.mqtt_pass || (n.mqtt_pass_set ? '' : saved.mqtt_pass),
      base_topic: n.base_topic || saved.base_topic,
      discovery_prefix: n.discovery_prefix || saved.discovery_prefix,
    },
  };
}

export function defaultFusion(): FusionCfg {
  return {
    assoc_gate_mm: 800,
    coast_ms: 1000,
    moving_thresh_mms: 100,
    stopped_thresh_mms: 50,
    stopped_hold_ms: 1000,
  };
}

export function defaultConfig(): RoomConfig {
  return {
    version: 1,
    room: { w_mm: 5000, h_mm: 4000 },
    // Centred on the lower wall looking into the room: the commonest mounting
    // spot, and a placement the user can drag rather than having to invent.
    sensor: { x_mm: 2500, y_mm: 0, theta_deg: 90 },
    zones: [],
    fusion: defaultFusion(),
    network: defaultNetwork(),
  };
}

export function newZone(id: string, cx: number, cy: number): Zone {
  return {
    id,
    name: `zone ${id}`,
    enabled: true,
    rect: { cx, cy, w: 1000, h: 1000, rot_deg: 0 },
    trigger: {
      states: ['any'],
      count_op: '>=',
      count_n: 1,
      // A short debounce by default: mmWave output is jumpy enough that a
      // zero-delay zone will chatter on almost any real install.
      on_delay_ms: 300,
    },
    untrigger: {
      mode: 'conditions_unmet',
      off_delay_ms: 5000,
      max_on_ms: 0,
    },
    actions: [],
  };
}

/** Bump the version on every write so the board can report drift. */
export function withBumpedVersion(cfg: RoomConfig): RoomConfig {
  return { ...cfg, version: cfg.version + 1 };
}

/* ---------- validation ---------- */

export function validateConfig(cfg: unknown): cfg is RoomConfig {
  if (typeof cfg !== 'object' || cfg === null) return false;
  const c = cfg as RoomConfig;
  if (typeof c.version !== 'number') return false;
  if (!c.room || typeof c.room.w_mm !== 'number' || typeof c.room.h_mm !== 'number') {
    return false;
  }
  if (!Array.isArray(c.zones)) return false;
  if (!c.sensor) return false;
  if (typeof c.sensor.x_mm !== 'number' || typeof c.sensor.y_mm !== 'number') {
    return false;
  }
  if (typeof c.sensor.theta_deg !== 'number') return false;
  if (!c.network) c.network = defaultNetwork();
  for (const z of c.zones) {
    if (typeof z.id !== 'string' || !z.rect) return false;
    if (typeof z.rect.cx !== 'number' || typeof z.rect.w !== 'number') return false;
    if (!z.trigger || !z.untrigger || !Array.isArray(z.actions)) return false;
  }
  return true;
}

const STORAGE_KEY = 'roomtrack.config';

export function loadLocal(): RoomConfig {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return defaultConfig();
    const parsed: unknown = JSON.parse(raw);
    return validateConfig(parsed) ? parsed : defaultConfig();
  } catch {
    return defaultConfig();
  }
}

export function saveLocal(cfg: RoomConfig): void {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(cfg));
  } catch {
    /* storage full or blocked; the device copy remains the source of truth */
  }
}
