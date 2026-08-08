import type { RoomConfig } from '../model/config';
import type { DeviceLink } from '../ble/link';
import type { NodeStatus, Track, TrackFrame, ZoneState } from '../ble/codec';
import { newRuntime, updateAll, type ZoneRuntime } from './zoneEngine';

/* Offline device: synthesises people walking the room and runs the same zone
 * rules the firmware would.
 *
 * This exists so the whole UI — room editing, zone triggering, colour changes —
 * is developable and demonstrable with no hardware powered on. It emits at the
 * same 10 Hz the LD2450 reports at. */

interface Walker {
  id: number;
  x: number;
  y: number;
  vx: number;
  vy: number;
  /** Seconds remaining of a deliberate pause, to exercise "stopped" rules. */
  pauseFor: number;
  nextPauseAt: number;
}

const TICK_MS = 100;
/* Jitter roughly matching what an LD2450 actually produces, so the smoothing
 * and debounce settings get exercised rather than being fed clean data. */
const JITTER_MM = 60;

export class SimLink implements DeviceLink {
  readonly kind = 'sim' as const;
  readonly label = 'simulator';

  private timer: number | null = null;
  private walkers: Walker[] = [];
  private runtimes: ZoneRuntime[] = [];
  private cfg: RoomConfig;
  private seq = 0;
  private t = 0;

  private cbTracks: (f: TrackFrame) => void = () => {};
  private cbZone: (z: ZoneState[]) => void = () => {};
  private cbStatus: (s: NodeStatus) => void = () => {};
  private cbGone: () => void = () => {};

  constructor(cfg: RoomConfig, private walkerCount = 2) {
    this.cfg = cfg;
    this.reset();
  }

  /** Keep the sim in step with edits made in the UI. */
  setConfig(cfg: RoomConfig): void {
    this.cfg = cfg;
    if (this.runtimes.length !== cfg.zones.length) {
      this.runtimes = cfg.zones.map(() => newRuntime());
    }
  }

  private reset(): void {
    const { w_mm, h_mm } = this.cfg.room;
    this.walkers = Array.from({ length: this.walkerCount }, (_, i) => ({
      id: i + 1,
      x: w_mm * (0.25 + 0.5 * Math.random()),
      y: h_mm * (0.25 + 0.5 * Math.random()),
      vx: (Math.random() - 0.5) * 1600,
      vy: (Math.random() - 0.5) * 1600,
      pauseFor: 0,
      nextPauseAt: 4 + Math.random() * 6,
    }));
    this.runtimes = this.cfg.zones.map(() => newRuntime());
  }

  async connect(): Promise<void> {
    if (this.timer !== null) return;
    this.t = 0;
    this.reset();
    this.timer = window.setInterval(() => this.tick(), TICK_MS);
  }

  disconnect(): void {
    if (this.timer !== null) {
      window.clearInterval(this.timer);
      this.timer = null;
    }
    this.cbGone();
  }

  isConnected(): boolean {
    return this.timer !== null;
  }

  onTracks(cb: (f: TrackFrame) => void) { this.cbTracks = cb; }
  onZoneState(cb: (z: ZoneState[]) => void) { this.cbZone = cb; }
  onStatus(cb: (s: NodeStatus) => void) { this.cbStatus = cb; }
  onDisconnected(cb: () => void) { this.cbGone = cb; }

  async writeConfig(cfg: RoomConfig): Promise<void> {
    this.setConfig(cfg);
  }

  async readConfig(): Promise<RoomConfig | null> {
    return this.cfg;
  }

  async sendCommand(): Promise<void> {
    /* no-op offline */
  }

  private tick(): void {
    const dt = TICK_MS / 1000;
    this.t += TICK_MS;
    const { w_mm, h_mm } = this.cfg.room;

    for (const wk of this.walkers) {
      if (wk.pauseFor > 0) {
        wk.pauseFor -= dt;
      } else {
        wk.x += wk.vx * dt;
        wk.y += wk.vy * dt;

        // Bounce off the walls rather than escaping the room.
        if (wk.x < 0 || wk.x > w_mm) {
          wk.vx = -wk.vx;
          wk.x = Math.max(0, Math.min(w_mm, wk.x));
        }
        if (wk.y < 0 || wk.y > h_mm) {
          wk.vy = -wk.vy;
          wk.y = Math.max(0, Math.min(h_mm, wk.y));
        }

        wk.nextPauseAt -= dt;
        if (wk.nextPauseAt <= 0) {
          wk.pauseFor = 3 + Math.random() * 4;
          wk.nextPauseAt = 8 + Math.random() * 8;
        }
      }
    }

    const tracks: Track[] = this.walkers.map((wk) => {
      const moving = wk.pauseFor <= 0;
      return {
        id: wk.id,
        x_mm: Math.round(wk.x + (Math.random() - 0.5) * JITTER_MM),
        y_mm: Math.round(wk.y + (Math.random() - 0.5) * JITTER_MM),
        vx_mms: moving ? Math.round(wk.vx) : 0,
        vy_mms: moving ? Math.round(wk.vy) : 0,
        motion: moving ? 'moving' : 'stopped',
      };
    });

    this.cbTracks({ seq: this.seq++, tracks });
    this.cbZone(updateAll(this.cfg.zones, this.runtimes, tracks, this.t));

    if (this.seq % 10 === 0) {
      this.cbStatus({
        node_id: 'sim0001',
        name: 'simulator',
        config_version: this.cfg.version,
        uptime_s: Math.floor(this.t / 1000),
        config_mode: true,
      });
    }
  }
}
