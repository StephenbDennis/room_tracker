import type { Zone } from '../model/config';
import { pointInRect } from '../model/geometry';
import type { Track, ZoneState } from '../ble/codec';

/* A TypeScript mirror of the state machine in firmware/main/zones.c, used by
 * the simulator so zone behaviour is demonstrable without hardware.
 *
 * zones.c is authoritative. If the two ever disagree, the firmware is right —
 * it is the one driving real pins, and it is the one under host unit test. */

export type Phase = 'idle' | 'arming' | 'triggered' | 'releasing';

export interface ZoneRuntime {
  phase: Phase;
  phaseSince: number;
  triggeredAt: number;
  matchCount: number;
  suppressed: boolean;
}

export function newRuntime(): ZoneRuntime {
  return {
    phase: 'idle',
    phaseSince: 0,
    triggeredAt: 0,
    matchCount: 0,
    suppressed: false,
  };
}

export function isActive(rt: ZoneRuntime): boolean {
  return rt.phase === 'triggered' || rt.phase === 'releasing';
}

function matches(z: Zone, t: Track): boolean {
  const any = z.trigger.states.includes('any');
  if (!any && !z.trigger.states.includes(t.motion as 'moving' | 'stopped')) {
    return false;
  }
  return pointInRect(z.rect, t.x_mm, t.y_mm);
}

export function countMatching(z: Zone, tracks: Track[]): number {
  return tracks.reduce((n, t) => n + (matches(z, t) ? 1 : 0), 0);
}

export function conditionsMet(z: Zone, tracks: Track[]): [boolean, number] {
  const n = countMatching(z, tracks);
  switch (z.trigger.count_op) {
    case '>=':
      return [n >= z.trigger.count_n, n];
    case '==':
      return [n === z.trigger.count_n, n];
    case '<=':
      return [n <= z.trigger.count_n, n];
    default:
      return [false, n];
  }
}

export function updateZone(
  z: Zone,
  rt: ZoneRuntime,
  tracks: Track[],
  now: number,
): boolean {
  const wasActive = isActive(rt);

  if (!z.enabled) {
    rt.phase = 'idle';
    rt.matchCount = 0;
    rt.suppressed = false;
    rt.triggeredAt = 0;
    return wasActive;
  }

  const [met, count] = conditionsMet(z, tracks);
  rt.matchCount = count;

  const enter = (p: Phase) => {
    rt.phase = p;
    rt.phaseSince = now;
    if (p === 'triggered' && rt.triggeredAt === 0) rt.triggeredAt = now;
  };

  // Zero delays let transitions cascade within one tick.
  for (let guard = 0; guard < 4; guard++) {
    const before = rt.phase;

    switch (rt.phase) {
      case 'idle':
        if (rt.suppressed) {
          if (!met) rt.suppressed = false;
          break;
        }
        if (met) {
          rt.triggeredAt = 0;
          enter('arming');
        }
        break;

      case 'arming':
        if (!met) enter('idle');
        else if (now - rt.phaseSince >= z.trigger.on_delay_ms) enter('triggered');
        break;

      case 'triggered':
        if (z.untrigger.max_on_ms !== 0 && now - rt.triggeredAt >= z.untrigger.max_on_ms) {
          rt.suppressed = true;
          enter('idle');
          break;
        }
        if (z.untrigger.mode === 'timer') {
          if (now - rt.triggeredAt >= z.untrigger.off_delay_ms) {
            rt.suppressed = true;
            enter('idle');
          }
        } else if (!met) {
          enter('releasing');
        }
        break;

      case 'releasing':
        if (z.untrigger.max_on_ms !== 0 && now - rt.triggeredAt >= z.untrigger.max_on_ms) {
          rt.suppressed = true;
          enter('idle');
          break;
        }
        if (met) {
          rt.phase = 'triggered'; // cancel the release, keep triggeredAt
        } else if (now - rt.phaseSince >= z.untrigger.off_delay_ms) {
          rt.triggeredAt = 0;
          enter('idle');
        }
        break;
    }

    if (rt.phase === before) break;
  }

  return isActive(rt) !== wasActive;
}

export function updateAll(
  zones: Zone[],
  runtimes: ZoneRuntime[],
  tracks: Track[],
  now: number,
): ZoneState[] {
  return zones.map((z, i) => {
    updateZone(z, runtimes[i], tracks, now);
    return {
      index: i,
      active: isActive(runtimes[i]),
      count: runtimes[i].matchCount,
    };
  });
}
