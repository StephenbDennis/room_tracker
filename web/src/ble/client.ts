import { defaultConfig, validateConfig, type RoomConfig } from '../model/config';
import {
  decodeStatus,
  decodeTracks,
  decodeZoneState,
  encodeConfigChunks,
  type NodeStatus,
  type TrackFrame,
  type ZoneState,
} from './codec';
import type { DeviceLink } from './link';
import {
  CHR_COMMAND,
  CHR_CONFIG_READ,
  CHR_CONFIG_WRITE,
  CHR_STATUS,
  CHR_TRACKS,
  CHR_ZONE_STATE,
  SVC_UUID,
} from './uuids';

/* Talks to one node at a time.
 *
 * Web Bluetooth requires a fresh user gesture for every requestDevice() call,
 * which is exactly why the design connects to a single node and reads the peer
 * list from its STATUS characteristic rather than pairing with each board. */
export class BleLink implements DeviceLink {
  readonly kind = 'ble' as const;

  private device: BluetoothDevice | null = null;
  private server: BluetoothRemoteGATTServer | null = null;
  private chrConfigWrite: BluetoothRemoteGATTCharacteristic | null = null;
  private chrConfigRead: BluetoothRemoteGATTCharacteristic | null = null;
  private chrCommand: BluetoothRemoteGATTCharacteristic | null = null;

  private cbTracks: (f: TrackFrame) => void = () => {};
  private cbZone: (z: ZoneState[]) => void = () => {};
  private cbStatus: (s: NodeStatus) => void = () => {};
  private cbGone: () => void = () => {};

  get label(): string {
    return this.device?.name ?? 'no device';
  }

  async connect(): Promise<void> {
    if (!('bluetooth' in navigator)) {
      throw new Error(
        'Web Bluetooth is unavailable. Use Chrome or Edge on desktop, or ' +
          'Chrome on Android — iOS Safari and Firefox do not support it.',
      );
    }

    this.device = await navigator.bluetooth.requestDevice({
      filters: [{ services: [SVC_UUID] }],
      optionalServices: [SVC_UUID],
    });

    this.device.addEventListener('gattserverdisconnected', () => {
      this.server = null;
      this.cbGone();
    });

    const server = await this.device.gatt!.connect();
    this.server = server;
    const svc = await server.getPrimaryService(SVC_UUID);

    this.chrConfigWrite = await svc.getCharacteristic(CHR_CONFIG_WRITE);
    this.chrConfigRead = await svc.getCharacteristic(CHR_CONFIG_READ);
    this.chrCommand = await svc.getCharacteristic(CHR_COMMAND);

    const tracks = await svc.getCharacteristic(CHR_TRACKS);
    tracks.addEventListener('characteristicvaluechanged', (e) => {
      const dv = (e.target as BluetoothRemoteGATTCharacteristic).value;
      if (dv) this.cbTracks(decodeTracks(dv));
    });
    await tracks.startNotifications();

    const zone = await svc.getCharacteristic(CHR_ZONE_STATE);
    zone.addEventListener('characteristicvaluechanged', (e) => {
      const dv = (e.target as BluetoothRemoteGATTCharacteristic).value;
      if (dv) this.cbZone(decodeZoneState(dv));
    });
    await zone.startNotifications();

    const status = await svc.getCharacteristic(CHR_STATUS);
    status.addEventListener('characteristicvaluechanged', (e) => {
      const dv = (e.target as BluetoothRemoteGATTCharacteristic).value;
      const s = dv && decodeStatus(dv);
      if (s) this.cbStatus(s);
    });
    await status.startNotifications();

    // Prime the panels rather than waiting for the first 1 Hz notification.
    const first = await status.readValue();
    const s = decodeStatus(first);
    if (s) this.cbStatus(s);
  }

  disconnect(): void {
    this.server?.disconnect();
    this.server = null;
  }

  isConnected(): boolean {
    return this.server?.connected ?? false;
  }

  onTracks(cb: (f: TrackFrame) => void) { this.cbTracks = cb; }
  onZoneState(cb: (z: ZoneState[]) => void) { this.cbZone = cb; }
  onStatus(cb: (s: NodeStatus) => void) { this.cbStatus = cb; }
  onDisconnected(cb: () => void) { this.cbGone = cb; }

  async writeConfig(cfg: RoomConfig): Promise<void> {
    if (!this.chrConfigWrite) throw new Error('not connected');

    const chunks = encodeConfigChunks(JSON.stringify(cfg));
    // Sequential writes with response: the firmware rejects out-of-order
    // chunks outright, so parallelism here would corrupt the transfer.
    for (const c of chunks) {
      await this.chrConfigWrite.writeValueWithResponse(c as BufferSource);
    }
  }

  async readConfig(): Promise<RoomConfig | null> {
    if (!this.chrConfigRead) return null;
    const dv = await this.chrConfigRead.readValue();
    try {
      const parsed: unknown = JSON.parse(new TextDecoder().decode(dv));
      return validateConfig(parsed) ? parsed : defaultConfig();
    } catch {
      return null;
    }
  }

  async sendCommand(bytes: Uint8Array): Promise<void> {
    if (!this.chrCommand) throw new Error('not connected');
    await this.chrCommand.writeValueWithResponse(bytes as BufferSource);
  }
}
