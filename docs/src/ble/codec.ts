/* Binary packet codecs. Wire formats are documented in docs/protocol.md and
 * produced by ble_gatt.c — keep the three in step. */

export type Motion = 'unknown' | 'moving' | 'stopped';

export interface Track {
  id: number;
  x_mm: number;
  y_mm: number;
  vx_mms: number;
  vy_mms: number;
  motion: Motion;
}

export interface TrackFrame {
  seq: number;
  tracks: Track[];
}

export interface ZoneState {
  index: number;
  active: boolean;
  count: number;
}

export interface NodeStatus {
  node_id: string;
  name: string;
  config_version: number;
  uptime_s: number;
  config_mode: boolean;
}

const MOTION: Motion[] = ['unknown', 'moving', 'stopped'];

/** [seq u16][n u8][ id u8, x i16, y i16, vx i16, vy i16, state u8 ] * n */
export function decodeTracks(dv: DataView): TrackFrame {
  if (dv.byteLength < 3) return { seq: 0, tracks: [] };

  const seq = dv.getUint16(0, true);
  const n = dv.getUint8(2);
  const tracks: Track[] = [];

  for (let i = 0; i < n; i++) {
    const o = 3 + i * 10;
    // Guard against a truncated notification rather than throwing mid-render.
    if (o + 10 > dv.byteLength) break;
    tracks.push({
      id: dv.getUint8(o),
      x_mm: dv.getInt16(o + 1, true),
      y_mm: dv.getInt16(o + 3, true),
      vx_mms: dv.getInt16(o + 5, true),
      vy_mms: dv.getInt16(o + 7, true),
      motion: MOTION[dv.getUint8(o + 9)] ?? 'unknown',
    });
  }
  return { seq, tracks };
}

/** [n u8][ index u8, active u8, count u8 ] * n */
export function decodeZoneState(dv: DataView): ZoneState[] {
  if (dv.byteLength < 1) return [];

  const n = dv.getUint8(0);
  const out: ZoneState[] = [];
  for (let i = 0; i < n; i++) {
    const o = 1 + i * 3;
    if (o + 3 > dv.byteLength) break;
    out.push({
      index: dv.getUint8(o),
      active: dv.getUint8(o + 1) !== 0,
      count: dv.getUint8(o + 2),
    });
  }
  return out;
}

export function decodeStatus(dv: DataView): NodeStatus | null {
  try {
    const text = new TextDecoder().decode(dv);
    return JSON.parse(text) as NodeStatus;
  } catch {
    return null;
  }
}

export const CHUNK_HDR_LEN = 4;
/* Conservative payload size: Web Bluetooth does not expose the negotiated MTU,
 * and platforms differ in what a single write will carry. */
export const CHUNK_PAYLOAD = 180;

/** Split a config blob into [seq u16][total u16][bytes] chunks. */
export function encodeConfigChunks(json: string): Uint8Array[] {
  const bytes = new TextEncoder().encode(json);
  const total = Math.max(1, Math.ceil(bytes.length / CHUNK_PAYLOAD));
  const chunks: Uint8Array[] = [];

  for (let i = 0; i < total; i++) {
    const slice = bytes.subarray(i * CHUNK_PAYLOAD, (i + 1) * CHUNK_PAYLOAD);
    const buf = new Uint8Array(CHUNK_HDR_LEN + slice.length);
    const dv = new DataView(buf.buffer);
    dv.setUint16(0, i, true);
    dv.setUint16(2, total, true);
    buf.set(slice, CHUNK_HDR_LEN);
    chunks.push(buf);
  }
  return chunks;
}

/** Inverse of encodeConfigChunks; used by tests and the simulator. */
export function decodeConfigChunks(chunks: Uint8Array[]): string {
  const parts: Uint8Array[] = [];
  for (const c of chunks) {
    parts.push(c.subarray(CHUNK_HDR_LEN));
  }
  const len = parts.reduce((a, p) => a + p.length, 0);
  const out = new Uint8Array(len);
  let o = 0;
  for (const p of parts) {
    out.set(p, o);
    o += p.length;
  }
  return new TextDecoder().decode(out);
}

export function encodeGpioTest(pin: number, level: number, ms: number): Uint8Array {
  const buf = new Uint8Array(5);
  const dv = new DataView(buf.buffer);
  buf[0] = 0x04;
  buf[1] = pin;
  buf[2] = level;
  dv.setUint16(3, ms, true);
  return buf;
}
