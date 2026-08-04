import { describe, expect, it } from 'vitest';
import { newZone, type Zone } from '../model/config';
import type { Track } from '../ble/codec';
import { isActive, newRuntime, updateZone } from '../sim/zoneEngine';

/* These mirror firmware/test/test_zones.c. Keeping the two suites in step is
 * what stops the simulator from teaching the user behaviour the hardware does
 * not actually have. */

function zone(patch: (z: Zone) => void = () => {}): Zone {
  const z = newZone('z1', 2000, 2000);
  z.trigger.states = ['moving'];
  z.trigger.on_delay_ms = 0;
  z.untrigger.off_delay_ms = 0;
  z.untrigger.max_on_ms = 0;
  patch(z);
  return z;
}

const at = (x: number, y: number, motion: Track['motion'] = 'moving'): Track[] => [
  { id: 1, x_mm: x, y_mm: y, vx_mms: 0, vy_mms: 0, motion },
];

const inside = at(2000, 2000);
const outside = at(9000, 9000);

describe('zone state machine', () => {
  it('triggers immediately with no delays', () => {
    const z = zone();
    const rt = newRuntime();
    expect(updateZone(z, rt, inside, 1000)).toBe(true);
    expect(isActive(rt)).toBe(true);
  });

  it('debounces entry with on_delay_ms', () => {
    const z = zone((x) => (x.trigger.on_delay_ms = 300));
    const rt = newRuntime();

    updateZone(z, rt, inside, 1000);
    expect(isActive(rt)).toBe(false);
    updateZone(z, rt, inside, 1200);
    expect(isActive(rt)).toBe(false);
    updateZone(z, rt, inside, 1300);
    expect(isActive(rt)).toBe(true);
  });

  it('rejects a single-frame blip', () => {
    const z = zone((x) => (x.trigger.on_delay_ms = 300));
    const rt = newRuntime();

    updateZone(z, rt, inside, 1000);
    updateZone(z, rt, outside, 1100);
    updateZone(z, rt, outside, 2000);
    expect(isActive(rt)).toBe(false);
  });

  it('holds for off_delay_ms then releases', () => {
    const z = zone((x) => (x.untrigger.off_delay_ms = 5000));
    const rt = newRuntime();

    updateZone(z, rt, inside, 1000);
    updateZone(z, rt, outside, 2000);
    expect(isActive(rt)).toBe(true);
    updateZone(z, rt, outside, 6999);
    expect(isActive(rt)).toBe(true);
    updateZone(z, rt, outside, 7000);
    expect(isActive(rt)).toBe(false);
  });

  it('cancels the release when conditions return', () => {
    const z = zone((x) => (x.untrigger.off_delay_ms = 5000));
    const rt = newRuntime();

    updateZone(z, rt, inside, 1000);
    updateZone(z, rt, outside, 2000);
    expect(rt.phase).toBe('releasing');
    expect(updateZone(z, rt, inside, 3000)).toBe(false); // no output change
    expect(rt.phase).toBe('triggered');
  });

  it('force-releases at max_on_ms and stays suppressed while occupied', () => {
    const z = zone((x) => (x.untrigger.max_on_ms = 10000));
    const rt = newRuntime();

    updateZone(z, rt, inside, 1000);
    updateZone(z, rt, inside, 11000);
    expect(isActive(rt)).toBe(false);

    updateZone(z, rt, inside, 60000);
    expect(isActive(rt)).toBe(false); // still there: must not re-fire

    updateZone(z, rt, outside, 61000);
    updateZone(z, rt, inside, 62000);
    expect(isActive(rt)).toBe(true); // left and returned: fires again
  });

  it('releases on the timer regardless of occupancy', () => {
    const z = zone((x) => {
      x.untrigger.mode = 'timer';
      x.untrigger.off_delay_ms = 30000;
    });
    const rt = newRuntime();

    updateZone(z, rt, inside, 1000);
    updateZone(z, rt, inside, 20000);
    expect(isActive(rt)).toBe(true);
    updateZone(z, rt, inside, 31000);
    expect(isActive(rt)).toBe(false);
  });

  it('filters on motion state', () => {
    const z = zone();
    const rt = newRuntime();
    updateZone(z, rt, at(2000, 2000, 'stopped'), 1000);
    expect(isActive(rt)).toBe(false);
    expect(rt.matchCount).toBe(0);
  });

  it('honours the count operator', () => {
    const z = zone((x) => {
      x.trigger.states = ['any'];
      x.trigger.count_op = '>=';
      x.trigger.count_n = 2;
    });
    const rt = newRuntime();

    updateZone(z, rt, inside, 1000);
    expect(isActive(rt)).toBe(false);

    const two: Track[] = [
      { id: 1, x_mm: 1900, y_mm: 1900, vx_mms: 0, vy_mms: 0, motion: 'moving' },
      { id: 2, x_mm: 2100, y_mm: 2100, vx_mms: 0, vy_mms: 0, motion: 'stopped' },
    ];
    updateZone(z, rt, two, 1100);
    expect(isActive(rt)).toBe(true);
    expect(rt.matchCount).toBe(2);
  });

  it('never triggers when disabled', () => {
    const z = zone((x) => (x.enabled = false));
    const rt = newRuntime();
    updateZone(z, rt, inside, 1000);
    expect(isActive(rt)).toBe(false);
  });
});
