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

export interface RoomConfig {
  version: number;
  room: { w_mm: number; h_mm: number };
  sensor: SensorPose;
  zones: Zone[];
  fusion: FusionCfg;
}

export const MAX_ZONES = 16;

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
