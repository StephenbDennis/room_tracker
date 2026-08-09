import { useEffect, useRef, useState } from 'react';
import type { RoomConfig } from '../model/config';
import type { Track, ZoneState } from '../ble/codec';
import { pointInRect, rectCorners, sensorWedge, type Pt } from '../model/geometry';

export type Selection =
  | { kind: 'zone'; id: string }
  | { kind: 'sensor' }
  | null;

interface Props {
  config: RoomConfig;
  tracks: Track[];
  zoneStates: Map<number, ZoneState>;
  selected: Selection;
  onSelect: (s: Selection) => void;
  onChange: (cfg: RoomConfig) => void;
}

/* World (mm, +y up) <-> screen (px, +y down). */
interface View {
  scale: number;
  ox: number;
  oy: number;
  h: number;
}

const PAD = 32;
const HANDLE_PX = 9;

function makeView(cw: number, ch: number, rw: number, rh: number): View {
  const scale = Math.min((cw - PAD * 2) / rw, (ch - PAD * 2) / rh);
  return {
    scale,
    ox: (cw - rw * scale) / 2,
    oy: (ch - rh * scale) / 2,
    h: ch,
  };
}

const toScreen = (v: View, p: Pt): Pt => ({
  x: v.ox + p.x * v.scale,
  y: v.h - v.oy - p.y * v.scale,
});

const toWorld = (v: View, sx: number, sy: number): Pt => ({
  x: (sx - v.ox) / v.scale,
  y: (v.h - sy - v.oy) / v.scale,
});

type DragKind =
  | { type: 'zone-move'; id: string; dx: number; dy: number }
  | { type: 'zone-resize'; id: string }
  | { type: 'sensor-move'; dx: number; dy: number }
  | null;

export function RoomCanvas({
  config,
  tracks,
  zoneStates,
  selected,
  onSelect,
  onChange,
}: Props) {
  const ref = useRef<HTMLCanvasElement>(null);
  const [size, setSize] = useState({ w: 800, h: 600 });
  const drag = useRef<DragKind>(null);
  // Which pointer owns the drag. The canvas allows pinch-zoom, so a second
  // finger must not hijack an in-flight drag or start one of its own.
  const dragPointer = useRef<number | null>(null);

  // Keep the backing store matched to the element's CSS size and DPR, or the
  // whole scene renders blurry on retina displays.
  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    const ro = new ResizeObserver(([entry]) => {
      const { width, height } = entry.contentRect;
      setSize({ w: Math.max(320, width), h: Math.max(240, height) });
    });
    ro.observe(el.parentElement ?? el);
    return () => ro.disconnect();
  }, []);

  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    const ctx = el.getContext('2d');
    if (!ctx) return;

    const dpr = window.devicePixelRatio || 1;
    el.width = size.w * dpr;
    el.height = size.h * dpr;
    el.style.width = `${size.w}px`;
    el.style.height = `${size.h}px`;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

    draw(ctx, size, config, tracks, zoneStates, selected);
  }, [size, config, tracks, zoneStates, selected]);

  const view = () =>
    makeView(size.w, size.h, config.room.w_mm, config.room.h_mm);

  function hitTest(wx: number, wy: number): {
    sel: Selection;
    resize: boolean;
  } {
    const v = view();
    const tol = HANDLE_PX / v.scale;

    // Zones first, topmost last-drawn wins.
    for (let i = config.zones.length - 1; i >= 0; i--) {
      const z = config.zones[i];
      const corners = rectCorners(z.rect);
      const br = corners[2];
      if (Math.abs(wx - br.x) < tol * 1.6 && Math.abs(wy - br.y) < tol * 1.6) {
        return { sel: { kind: 'zone', id: z.id }, resize: true };
      }
      if (pointInRect(z.rect, wx, wy)) {
        return { sel: { kind: 'zone', id: z.id }, resize: false };
      }
    }

    const s = config.sensor;
    if (Math.hypot(wx - s.x_mm, wy - s.y_mm) < tol * 2.2) {
      return { sel: { kind: 'sensor' }, resize: false };
    }
    return { sel: null, resize: false };
  }

  function onPointerDown(e: React.PointerEvent<HTMLCanvasElement>) {
    // A second finger is the start of a pinch, not a new drag.
    if (dragPointer.current !== null) return;

    const rect = e.currentTarget.getBoundingClientRect();
    const w = toWorld(view(), e.clientX - rect.left, e.clientY - rect.top);
    const { sel, resize } = hitTest(w.x, w.y);

    onSelect(sel);
    if (!sel) return;

    dragPointer.current = e.pointerId;
    e.currentTarget.setPointerCapture(e.pointerId);

    if (sel.kind === 'zone') {
      const z = config.zones.find((x) => x.id === sel.id)!;
      drag.current = resize
        ? { type: 'zone-resize', id: sel.id }
        : { type: 'zone-move', id: sel.id, dx: w.x - z.rect.cx, dy: w.y - z.rect.cy };
    } else {
      drag.current = {
        type: 'sensor-move',
        dx: w.x - config.sensor.x_mm,
        dy: w.y - config.sensor.y_mm,
      };
    }
  }

  function onPointerMove(e: React.PointerEvent<HTMLCanvasElement>) {
    const d = drag.current;
    if (!d || e.pointerId !== dragPointer.current) return;

    const rect = e.currentTarget.getBoundingClientRect();
    const w = toWorld(view(), e.clientX - rect.left, e.clientY - rect.top);

    if (d.type === 'zone-move') {
      onChange({
        ...config,
        zones: config.zones.map((z) =>
          z.id === d.id
            ? { ...z, rect: { ...z.rect, cx: w.x - d.dx, cy: w.y - d.dy } }
            : z,
        ),
      });
    } else if (d.type === 'zone-resize') {
      onChange({
        ...config,
        zones: config.zones.map((z) => {
          if (z.id !== d.id) return z;
          // Resize about the centre; 200 mm floor keeps a zone grabbable.
          const w2 = Math.max(200, Math.abs(w.x - z.rect.cx) * 2);
          const h2 = Math.max(200, Math.abs(w.y - z.rect.cy) * 2);
          return { ...z, rect: { ...z.rect, w: w2, h: h2 } };
        }),
      });
    } else if (d.type === 'sensor-move') {
      onChange({
        ...config,
        sensor: { ...config.sensor, x_mm: w.x - d.dx, y_mm: w.y - d.dy },
      });
    }
  }

  /* Also the pointercancel handler: the browser fires that on the first finger
   * the moment a pinch begins, which is exactly when the drag should stop. */
  function onPointerUp(e: React.PointerEvent<HTMLCanvasElement>) {
    if (e.pointerId !== dragPointer.current) return;
    drag.current = null;
    dragPointer.current = null;
    if (e.currentTarget.hasPointerCapture(e.pointerId)) {
      e.currentTarget.releasePointerCapture(e.pointerId);
    }
  }

  return (
    <canvas
      ref={ref}
      className="room-canvas"
      onPointerDown={onPointerDown}
      onPointerMove={onPointerMove}
      onPointerUp={onPointerUp}
      onPointerCancel={onPointerUp}
    />
  );
}

/* ---------- drawing ---------- */

function draw(
  ctx: CanvasRenderingContext2D,
  size: { w: number; h: number },
  cfg: RoomConfig,
  tracks: Track[],
  zoneStates: Map<number, ZoneState>,
  selected: Selection,
) {
  const v = makeView(size.w, size.h, cfg.room.w_mm, cfg.room.h_mm);

  ctx.clearRect(0, 0, size.w, size.h);
  ctx.fillStyle = '#0e1117';
  ctx.fillRect(0, 0, size.w, size.h);

  // 1 m grid
  ctx.strokeStyle = '#1c2230';
  ctx.lineWidth = 1;
  for (let x = 0; x <= cfg.room.w_mm; x += 1000) {
    const a = toScreen(v, { x, y: 0 });
    const b = toScreen(v, { x, y: cfg.room.h_mm });
    ctx.beginPath();
    ctx.moveTo(a.x, a.y);
    ctx.lineTo(b.x, b.y);
    ctx.stroke();
  }
  for (let y = 0; y <= cfg.room.h_mm; y += 1000) {
    const a = toScreen(v, { x: 0, y });
    const b = toScreen(v, { x: cfg.room.w_mm, y });
    ctx.beginPath();
    ctx.moveTo(a.x, a.y);
    ctx.lineTo(b.x, b.y);
    ctx.stroke();
  }

  // room outline
  const tl = toScreen(v, { x: 0, y: cfg.room.h_mm });
  ctx.strokeStyle = '#3d4a63';
  ctx.lineWidth = 2;
  ctx.strokeRect(tl.x, tl.y, cfg.room.w_mm * v.scale, cfg.room.h_mm * v.scale);

  // sensor coverage wedge, drawn under everything else
  {
    const pts = sensorWedge(cfg.sensor).map((p) => toScreen(v, p));
    ctx.beginPath();
    ctx.moveTo(pts[0].x, pts[0].y);
    for (const p of pts.slice(1)) ctx.lineTo(p.x, p.y);
    ctx.closePath();
    ctx.fillStyle = 'rgba(90, 160, 255, 0.07)';
    ctx.fill();
  }

  // zones
  cfg.zones.forEach((z, i) => {
    const st = zoneStates.get(i);
    const active = st?.active ?? false;
    const pts = rectCorners(z.rect).map((p) => toScreen(v, p));

    ctx.beginPath();
    ctx.moveTo(pts[0].x, pts[0].y);
    for (const p of pts.slice(1)) ctx.lineTo(p.x, p.y);
    ctx.closePath();

    // The colour change on trigger is the whole point of the live view.
    ctx.fillStyle = active ? 'rgba(255, 96, 92, 0.30)' : 'rgba(120, 200, 160, 0.13)';
    ctx.fill();
    ctx.strokeStyle = active ? '#ff605c' : '#4fbf8b';
    ctx.lineWidth = selected?.kind === 'zone' && selected.id === z.id ? 3 : 1.5;
    if (!z.enabled) ctx.setLineDash([6, 4]);
    ctx.stroke();
    ctx.setLineDash([]);

    const c = toScreen(v, { x: z.rect.cx, y: z.rect.cy });
    ctx.fillStyle = active ? '#ffd7d6' : '#a9c7ba';
    ctx.font = '12px ui-sans-serif, system-ui, sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText(z.name, c.x, c.y - 4);
    ctx.fillText(`${st?.count ?? 0} inside`, c.x, c.y + 12);

    if (selected?.kind === 'zone' && selected.id === z.id) {
      const br = pts[2];
      ctx.fillStyle = '#4fbf8b';
      ctx.fillRect(br.x - HANDLE_PX / 2, br.y - HANDLE_PX / 2, HANDLE_PX, HANDLE_PX);
    }
  });

  // sensor
  {
    const n = cfg.sensor;
    const p = toScreen(v, { x: n.x_mm, y: n.y_mm });
    const sel = selected?.kind === 'sensor';

    ctx.beginPath();
    ctx.arc(p.x, p.y, sel ? 10 : 7, 0, Math.PI * 2);
    ctx.fillStyle = '#5aa0ff';
    ctx.fill();
    ctx.strokeStyle = '#0e1117';
    ctx.lineWidth = 2;
    ctx.stroke();

    // boresight tick, so rotation is visible at a glance
    const a = (n.theta_deg * Math.PI) / 180;
    ctx.beginPath();
    ctx.moveTo(p.x, p.y);
    ctx.lineTo(p.x + Math.cos(a) * 22, p.y - Math.sin(a) * 22);
    ctx.strokeStyle = '#5aa0ff';
    ctx.lineWidth = 2;
    ctx.stroke();
  }

  // tracked people
  for (const t of tracks) {
    const p = toScreen(v, { x: t.x_mm, y: t.y_mm });
    const moving = t.motion === 'moving';

    ctx.beginPath();
    ctx.arc(p.x, p.y, 9, 0, Math.PI * 2);
    ctx.fillStyle = moving ? '#ffc857' : '#8b93a5';
    ctx.fill();

    if (moving) {
      // velocity vector, scaled to about a second of travel
      const e = toScreen(v, { x: t.x_mm + t.vx_mms, y: t.y_mm + t.vy_mms });
      ctx.beginPath();
      ctx.moveTo(p.x, p.y);
      ctx.lineTo(e.x, e.y);
      ctx.strokeStyle = 'rgba(255, 200, 87, 0.65)';
      ctx.lineWidth = 2;
      ctx.stroke();
    }

    ctx.fillStyle = '#0e1117';
    ctx.font = 'bold 10px ui-sans-serif, system-ui, sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText(String(t.id), p.x, p.y + 3);
  }
}
