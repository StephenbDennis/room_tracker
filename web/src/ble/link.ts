import type { RoomConfig } from '../model/config';
import type { NodeStatus, TrackFrame, ZoneState } from './codec';

/* A source of live room data. The real BLE client and the offline simulator
 * both implement this, so every panel and the renderer work identically with
 * or without hardware attached. */
export interface DeviceLink {
  readonly kind: 'ble' | 'sim';
  readonly label: string;

  connect(): Promise<void>;
  disconnect(): void;
  isConnected(): boolean;

  onTracks(cb: (f: TrackFrame) => void): void;
  onZoneState(cb: (z: ZoneState[]) => void): void;
  onStatus(cb: (s: NodeStatus) => void): void;
  onDisconnected(cb: () => void): void;

  writeConfig(cfg: RoomConfig): Promise<void>;
  readConfig(): Promise<RoomConfig | null>;
  sendCommand(bytes: Uint8Array): Promise<void>;
}

/** Web Bluetooth is Chrome/Edge desktop and Chrome Android only. */
export function bluetoothAvailable(): boolean {
  return typeof navigator !== 'undefined' && 'bluetooth' in navigator;
}
