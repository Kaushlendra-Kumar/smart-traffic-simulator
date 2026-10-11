import { describe, expect, it } from "vitest";
import { interpolateVehicles, lerpAngle } from "../state/interpolate";
import type { VehicleTuple } from "../types/protocol";

describe("interpolateVehicles", () => {
  const a: VehicleTuple[] = [[1, 0, 0, 0, 10, 0, 4.5], [2, 5, 5, 0, 0, 1, 12]];
  const b: VehicleTuple[] = [[1, 10, 0, 0, 12, 0, 4.5], [3, 50, 50, 1, 5, 2, 6]];

  it("blends positions of vehicles present in both frames", () => {
    const out = interpolateVehicles(a, b, 0.5);
    expect(out.find((v) => v.id === 1)).toMatchObject({ x: 5, y: 0, speed: 11 });
  });
  it("shows new vehicles at their current position and drops finished ones", () => {
    const out = interpolateVehicles(a, b, 0.25);
    expect(out.map((v) => v.id).sort()).toEqual([1, 3]);
    expect(out.find((v) => v.id === 3)).toMatchObject({ x: 50, y: 50 });
  });
  it("clamps alpha and never extrapolates past the newest snapshot", () => {
    expect(interpolateVehicles(a, b, 7)[0].x).toBe(10);
    expect(interpolateVehicles(a, b, -1)[0].x).toBe(0);
  });
  it("does not smooth teleports such as seeks", () => {
    const far: VehicleTuple[] = [[1, 500, 0, 0, 0, 0, 4.5]];
    expect(interpolateVehicles(a, far, 0.5)[0].x).toBe(500);
  });
});

describe("lerpAngle", () => {
  it("takes the shortest way round", () => {
    expect(lerpAngle(3.0, -3.0, 0.5)).toBeCloseTo(Math.PI, 2);
    expect(lerpAngle(0, Math.PI / 2, 0.5)).toBeCloseTo(Math.PI / 4, 6);
  });
});
