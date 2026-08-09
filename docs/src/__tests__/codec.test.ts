import { describe, expect, it } from 'vitest';
import {
  CHUNK_PAYLOAD,
  decodeConfigChunks,
  decodeStatus,
  decodeTracks,
  decodeZoneState,
  encodeConfigChunks,
  encodeGpioTest,
} from '../ble/codec';

const statusPacket = (json: string): DataView =>
  new DataView(new TextEncoder().encode(json).buffer);

function trackPacket(
  seq: number,
  rows: [number, number, number, number, number, number][],
): DataView {
  const buf = new Uint8Array(3 + rows.length * 10);
  const dv = new DataView(buf.buffer);
  dv.setUint16(0, seq, true);
  dv.setUint8(2, rows.length);
  rows.forEach(([id, x, y, vx, vy, state], i) => {
    const o = 3 + i * 10;
    dv.setUint8(o, id);
    dv.setInt16(o + 1, x, true);
    dv.setInt16(o + 3, y, true);
    dv.setInt16(o + 5, vx, true);
    dv.setInt16(o + 7, vy, true);
    dv.setUint8(o + 9, state);
  });
  return dv;
}

describe('decodeTracks', () => {
  it('decodes a multi-track frame', () => {
    const dv = trackPacket(42, [
      [1, 1200, 2400, 300, -150, 1],
      [2, -800, 900, 0, 0, 2],
    ]);
    const f = decodeTracks(dv);

    expect(f.seq).toBe(42);
    expect(f.tracks).toHaveLength(2);
    expect(f.tracks[0]).toEqual({
      id: 1,
      x_mm: 1200,
      y_mm: 2400,
      vx_mms: 300,
      vy_mms: -150,
      motion: 'moving',
    });
    expect(f.tracks[1].motion).toBe('stopped');
    expect(f.tracks[1].x_mm).toBe(-800);
  });

  it('handles an empty frame', () => {
    const f = decodeTracks(trackPacket(7, []));
    expect(f.seq).toBe(7);
    expect(f.tracks).toEqual([]);
  });

  // A truncated notification must not throw mid-render.
  it('stops at a truncated payload instead of throwing', () => {
    const full = trackPacket(1, [
      [1, 100, 200, 0, 0, 1],
      [2, 300, 400, 0, 0, 1],
    ]);
    const cut = new DataView(full.buffer.slice(0, 3 + 10 + 4));
    const f = decodeTracks(cut);
    expect(f.tracks).toHaveLength(1);
  });

  it('maps an unknown motion byte to unknown', () => {
    const f = decodeTracks(trackPacket(1, [[1, 0, 0, 0, 0, 99]]));
    expect(f.tracks[0].motion).toBe('unknown');
  });
});

describe('decodeStatus', () => {
  it('decodes a well-formed payload', () => {
    const s = decodeStatus(
      statusPacket(
        '{"node_id":"a1b2c3","name":"node-a1b2c3","config_version":7,' +
          '"uptime_s":4210,"config_mode":true}',
      ),
    );
    expect(s).toEqual({
      node_id: 'a1b2c3',
      name: 'node-a1b2c3',
      config_version: 7,
      uptime_s: 4210,
      config_mode: true,
    });
  });

  // Firmware and this page cannot update atomically, so a board may always
  // send a shape this build does not expect. Missing fields must come back
  // defined rather than undefined, or they surface as a crash during render.
  it('fills in fields the firmware did not send', () => {
    const s = decodeStatus(statusPacket('{"node_id":"a1b2c3"}'));
    expect(s).toEqual({
      node_id: 'a1b2c3',
      name: '',
      config_version: 0,
      uptime_s: 0,
      config_mode: false,
    });
  });

  it('ignores unknown fields from older firmware', () => {
    const s = decodeStatus(
      statusPacket('{"node_id":"a1","name":"n","config_version":2,' +
        '"uptime_s":3,"config_mode":false,"peers":[{"id":"b2"}]}'),
    );
    expect(s).not.toHaveProperty('peers');
    expect(s?.config_version).toBe(2);
  });

  it('rejects wrong-typed fields rather than passing them through', () => {
    const s = decodeStatus(
      statusPacket('{"node_id":42,"config_version":"seven"}'),
    );
    expect(s?.node_id).toBe('');
    expect(s?.config_version).toBe(0);
  });

  it('returns null on malformed JSON and on non-objects', () => {
    expect(decodeStatus(statusPacket('{not json'))).toBeNull();
    expect(decodeStatus(statusPacket('"a string"'))).toBeNull();
    expect(decodeStatus(statusPacket('null'))).toBeNull();
  });
});

describe('decodeZoneState', () => {
  it('decodes zone rows', () => {
    const buf = new Uint8Array([2, 0, 1, 3, 1, 0, 0]);
    const zs = decodeZoneState(new DataView(buf.buffer));
    expect(zs).toEqual([
      { index: 0, active: true, count: 3 },
      { index: 1, active: false, count: 0 },
    ]);
  });

  it('returns empty for an empty buffer', () => {
    expect(decodeZoneState(new DataView(new ArrayBuffer(0)))).toEqual([]);
  });
});

describe('config chunking', () => {
  it('round-trips a payload larger than one chunk', () => {
    const json = JSON.stringify({ zones: 'x'.repeat(CHUNK_PAYLOAD * 3) });
    const chunks = encodeConfigChunks(json);

    expect(chunks.length).toBeGreaterThan(3);
    expect(decodeConfigChunks(chunks)).toBe(json);
  });

  it('numbers chunks sequentially and stamps the same total', () => {
    const chunks = encodeConfigChunks('y'.repeat(CHUNK_PAYLOAD * 2 + 5));
    chunks.forEach((c, i) => {
      const dv = new DataView(c.buffer, c.byteOffset, c.byteLength);
      expect(dv.getUint16(0, true)).toBe(i);
      expect(dv.getUint16(2, true)).toBe(chunks.length);
    });
  });

  it('emits one chunk for a short payload', () => {
    const chunks = encodeConfigChunks('{}');
    expect(chunks).toHaveLength(1);
    expect(decodeConfigChunks(chunks)).toBe('{}');
  });

  // Multi-byte characters must not be split across a chunk boundary in a way
  // that corrupts them, since encoding happens before slicing.
  it('round-trips non-ASCII content', () => {
    const json = JSON.stringify({ name: 'café ' + 'é'.repeat(200) });
    expect(decodeConfigChunks(encodeConfigChunks(json))).toBe(json);
  });
});

describe('encodeGpioTest', () => {
  it('packs pin, level and duration little-endian', () => {
    const b = encodeGpioTest(12, 1, 500);
    expect(Array.from(b)).toEqual([0x04, 12, 1, 0xf4, 0x01]);
  });
});
