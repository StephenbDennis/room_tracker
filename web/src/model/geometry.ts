import type { NodePose, ZoneRect } from './config';

/* Room frame: origin at a corner, millimetres, +x right, +y up in plan view. */

export interface Pt {
  x: number;
  y: number;
}

const rad = (deg: number) => (deg * Math.PI) / 180;

/** Mirrors zone_contains_point() in firmware/main/zones.c. */
export function pointInRect(r: ZoneRect, x: number, y: number): boolean {
  let dx = x - r.cx;
  let dy = y - r.cy;

  if (r.rot_deg !== 0) {
    const a = rad(r.rot_deg);
    const c = Math.cos(a);
    const s = Math.sin(a);
    const lx = dx * c + dy * s;
    const ly = -dx * s + dy * c;
    dx = lx;
    dy = ly;
  }

  return Math.abs(dx) <= r.w / 2 && Math.abs(dy) <= r.h / 2;
}

/** The four corners in room coordinates, for drawing and hit handles. */
export function rectCorners(r: ZoneRect): Pt[] {
  const a = rad(r.rot_deg);
  const c = Math.cos(a);
  const s = Math.sin(a);
  const hw = r.w / 2;
  const hh = r.h / 2;

  return [
    [-hw, -hh],
    [hw, -hh],
    [hw, hh],
    [-hw, hh],
  ].map(([lx, ly]) => ({
    x: r.cx + lx * c - ly * s,
    y: r.cy + lx * s + ly * c,
  }));
}

/** Mirrors fusion_transform(): sensor-local +y is forward, +x is right. */
export function sensorToRoom(pose: NodePose, lx: number, ly: number): Pt {
  const a = rad(pose.theta_deg);
  const c = Math.cos(a);
  const s = Math.sin(a);
  return {
    x: pose.x_mm + ly * c + lx * s,
    y: pose.y_mm + ly * s - lx * c,
  };
}

/** The LD2450 sees +/-60 degrees to about 6 m. Used to draw coverage wedges. */
export const SENSOR_FOV_DEG = 60;
export const SENSOR_RANGE_MM = 6000;

export function sensorWedge(pose: NodePose, steps = 16): Pt[] {
  const pts: Pt[] = [{ x: pose.x_mm, y: pose.y_mm }];
  for (let i = 0; i <= steps; i++) {
    const t = -SENSOR_FOV_DEG + (2 * SENSOR_FOV_DEG * i) / steps;
    const a = rad(pose.theta_deg + t);
    pts.push({
      x: pose.x_mm + Math.cos(a) * SENSOR_RANGE_MM,
      y: pose.y_mm + Math.sin(a) * SENSOR_RANGE_MM,
    });
  }
  return pts;
}

export function dist(a: Pt, b: Pt): number {
  return Math.hypot(a.x - b.x, a.y - b.y);
}
