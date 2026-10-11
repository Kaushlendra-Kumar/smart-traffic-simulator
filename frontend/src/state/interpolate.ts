// Render-only interpolation between two authoritative snapshots. Never fed back to the server.

import type { VehicleTuple } from "../types/protocol";

export interface DrawVehicle {
  id: number;
  x: number;
  y: number;
  heading: number;
  speed: number;
  type: number;
  length: number;
}

export function lerpAngle(a: number, b: number, t: number): number {
  let d = b - a;
  while (d > Math.PI) d -= 2 * Math.PI;
  while (d < -Math.PI) d += 2 * Math.PI;
  return a + d * t;
}

/**
 * Blends vehicle positions between `prev` and `curr` with weight `alpha` in [0,1].
 * Vehicles that only exist in `curr` appear at their current position; vehicles that
 * vanished (completed trips) are dropped. Large jumps (e.g. a seek) are not smoothed.
 */
export function interpolateVehicles(prev: VehicleTuple[] | undefined, curr: VehicleTuple[], alpha: number): DrawVehicle[] {
  const a = Math.min(1, Math.max(0, alpha));
  const before = new Map<number, VehicleTuple>();
  if (prev) for (const v of prev) before.set(v[0], v);
  return curr.map((v) => {
    const p = before.get(v[0]);
    if (!p || Math.hypot(v[1] - p[1], v[2] - p[2]) > 40) {
      return { id: v[0], x: v[1], y: v[2], heading: v[3], speed: v[4], type: v[5], length: v[6] };
    }
    return {
      id: v[0],
      x: p[1] + (v[1] - p[1]) * a,
      y: p[2] + (v[2] - p[2]) * a,
      heading: lerpAngle(p[3], v[3], a),
      speed: p[4] + (v[4] - p[4]) * a,
      type: v[5],
      length: v[6],
    };
  });
}
