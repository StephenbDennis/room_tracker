import { describe, expect, it } from 'vitest';
import { pointInRect, rectCorners, sensorToRoom } from '../model/geometry';
import type { SensorPose, ZoneRect } from '../model/config';

const rect = (o: Partial<ZoneRect> = {}): ZoneRect => ({
  cx: 2000,
  cy: 2000,
  w: 1000,
  h: 1000,
  rot_deg: 0,
  ...o,
});

const pose = (x: number, y: number, theta: number): SensorPose => ({
  x_mm: x,
  y_mm: y,
  theta_deg: theta,
});

describe('pointInRect', () => {
  it('accepts interior and boundary points', () => {
    const r = rect();
    expect(pointInRect(r, 2000, 2000)).toBe(true);
    expect(pointInRect(r, 1500, 1500)).toBe(true);
    expect(pointInRect(r, 2500, 2500)).toBe(true);
  });

  it('rejects exterior points', () => {
    const r = rect();
    expect(pointInRect(r, 1499, 2000)).toBe(false);
    expect(pointInRect(r, 2000, 2501)).toBe(false);
  });

  it('respects rotation', () => {
    const r = rect({ cx: 0, cy: 0, w: 1000, h: 200, rot_deg: 45 });
    expect(pointInRect(r, 300, 300)).toBe(true); // along the long axis
    expect(pointInRect(r, 300, -300)).toBe(false); // across the short axis
  });
});

describe('rectCorners', () => {
  it('returns the axis-aligned corners in order', () => {
    const c = rectCorners(rect());
    expect(c[0]).toEqual({ x: 1500, y: 1500 });
    expect(c[2].x).toBeCloseTo(2500);
    expect(c[2].y).toBeCloseTo(2500);
  });

  it('rotates corners about the centre', () => {
    const c = rectCorners(rect({ cx: 0, cy: 0, rot_deg: 90 }));
    // A 90 degree turn maps (-500,-500) to (500,-500).
    expect(c[0].x).toBeCloseTo(500);
    expect(c[0].y).toBeCloseTo(-500);
  });
});

/* These must agree with fusion_transform() in firmware/main/fusion.c; the
 * canvas would otherwise draw sensor coverage somewhere the firmware is not
 * actually looking. */
describe('sensorToRoom', () => {
  it('maps forward to +y when facing 90 degrees', () => {
    const p = sensorToRoom(pose(0, 0, 90), 100, 1000);
    expect(p.x).toBeCloseTo(100);
    expect(p.y).toBeCloseTo(1000);
  });

  it('maps the sensor right to -y when facing +x', () => {
    const p = sensorToRoom(pose(0, 0, 0), 100, 1000);
    expect(p.x).toBeCloseTo(1000);
    expect(p.y).toBeCloseTo(-100);
  });

  it('applies translation', () => {
    const p = sensorToRoom(pose(2000, 3000, 90), 0, 1500);
    expect(p.x).toBeCloseTo(2000);
    expect(p.y).toBeCloseTo(4500);
  });

  it('lets two sensors agree on one physical point', () => {
    const tx = 2500;
    const ty = 2000;
    const range = Math.hypot(tx, ty);
    const theta = (Math.atan2(ty, tx) * 180) / Math.PI;

    const a = sensorToRoom(pose(0, 0, theta), 0, range);
    const b = sensorToRoom(pose(5000, 4000, theta + 180), 0, range);

    expect(a.x).toBeCloseTo(tx, 6);
    expect(a.y).toBeCloseTo(ty, 6);
    expect(b.x).toBeCloseTo(tx, 6);
    expect(b.y).toBeCloseTo(ty, 6);
  });
});
