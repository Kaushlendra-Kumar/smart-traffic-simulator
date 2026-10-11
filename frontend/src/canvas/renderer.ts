// Layered Canvas renderer. World coordinates are metres with y pointing north; the
// view transform (pan/zoom) is purely visual and never affects the simulation.
//
// Layers: 1 roads & lanes, 2 junction markings, 3 active movements + signal heads,
// 4 congestion overlay, 5 incidents, 6 vehicles, 7 pedestrians, 8 labels/debug.

import { interpolateVehicles, type DrawVehicle } from "../state/interpolate";
import type { NetworkMsg, Pt, RoadInfo, SnapshotMsg } from "../types/protocol";

export const palette = {
  ground: "#C9CCC4",
  asphalt: "#363A3E",
  junction: "#3B3F43",
  marking: "#EEF0EA",
  yellow: "#E9B32A",
  green: "#3FB37F",
  amber: "#E9B32A",
  red: "#D2412F",
  ink: "#1D2A35",
  pedestrian: "#1D2A35",
};

const CAR_COLOURS = ["#E7E9E4", "#9FB4C7", "#5C7389", "#B7B09C", "#7E8B7A", "#C7CDD2", "#8A97A6", "#D8D2C2"];

export interface ViewState {
  cx: number; // world point at the canvas centre
  cy: number;
  scale: number; // pixels per metre
}

export interface RenderOptions {
  colourBySpeed: boolean;
  showCongestion: boolean;
  showLabels: boolean;
  selectedRoad: number | null;
  hoverRoad: number | null;
}

interface StaticGeometry {
  asphalt: Path2D;
  junctions: Path2D;
  edgeLines: Path2D;
  laneDividers: Path2D;
  centreLines: Path2D;
  stopBars: Path2D;
  zebra: Path2D;
}

function sub(a: Pt, b: Pt): Pt {
  return [a[0] - b[0], a[1] - b[1]];
}
function unit(v: Pt): Pt {
  const l = Math.hypot(v[0], v[1]) || 1;
  return [v[0] / l, v[1] / l];
}
/** Right-hand normal of a direction (matches Vec2::right_normal in the engine). */
function right(d: Pt): Pt {
  return [d[1], -d[0]];
}
function along(r: RoadInfo, s: number, offset: number): Pt {
  const d = unit(sub(r.end, r.start));
  const n = right(d);
  return [r.start[0] + d[0] * s + n[0] * offset, r.start[1] + d[1] * s + n[1] * offset];
}
/** Lateral extent [inner, outer] of a directed road, relative to its centre line. */
function roadBand(r: RoadInfo): [number, number] {
  const w = r.lane_width;
  if (r.reverse >= 0) return [0, r.lanes * w];
  return [(-r.lanes * w) / 2, (r.lanes * w) / 2];
}

export function speedColour(speed: number, limit = 14): string {
  const t = Math.max(0, Math.min(1, speed / limit));
  // red (stopped) -> amber -> green (free flow)
  const hue = 6 + t * 140;
  return `hsl(${hue}, 62%, 52%)`;
}

export class TrafficRenderer {
  private ctx: CanvasRenderingContext2D;
  private geom: StaticGeometry | null = null;
  private net: NetworkMsg | null = null;
  view: ViewState = { cx: 0, cy: 0, scale: 2 };
  private dpr = 1;

  constructor(private canvas: HTMLCanvasElement) {
    const ctx = canvas.getContext("2d");
    if (!ctx) throw new Error("Canvas 2D is not available in this browser.");
    this.ctx = ctx;
  }

  resize(width: number, height: number) {
    this.dpr = window.devicePixelRatio || 1;
    this.canvas.width = Math.round(width * this.dpr);
    this.canvas.height = Math.round(height * this.dpr);
  }

  get width() {
    return this.canvas.width / this.dpr;
  }
  get height() {
    return this.canvas.height / this.dpr;
  }

  setNetwork(net: NetworkMsg | null) {
    if (net === this.net) return;
    this.net = net;
    this.geom = net ? buildGeometry(net) : null;
  }

  /** Fits the signalised core of the network (plus approach margin), or everything. */
  fit() {
    if (!this.net) return;
    let [x0, y0, x1, y1] = this.net.bounds;
    const core = this.net.nodes.filter((n) => n.signalized);
    if (core.length > 0) {
      const margin = 140;
      const xs = core.map((n) => n.pos[0]), ys = core.map((n) => n.pos[1]);
      x0 = Math.max(x0, Math.min(...xs) - margin);
      x1 = Math.min(x1, Math.max(...xs) + margin);
      y0 = Math.max(y0, Math.min(...ys) - margin);
      y1 = Math.min(y1, Math.max(...ys) + margin);
    }
    const pad = 40;
    this.view.cx = (x0 + x1) / 2;
    this.view.cy = (y0 + y1) / 2;
    this.view.scale = Math.max(
      0.2,
      Math.min((this.width - 2 * pad) / Math.max(x1 - x0, 50), (this.height - 2 * pad) / Math.max(y1 - y0, 50)),
    );
  }

  screenToWorld(px: number, py: number): Pt {
    return [this.view.cx + (px - this.width / 2) / this.view.scale, this.view.cy - (py - this.height / 2) / this.view.scale];
  }

  zoomAt(px: number, py: number, factor: number) {
    const before = this.screenToWorld(px, py);
    this.view.scale = Math.max(0.15, Math.min(40, this.view.scale * factor));
    const after = this.screenToWorld(px, py);
    this.view.cx += before[0] - after[0];
    this.view.cy += before[1] - after[1];
  }

  panBy(dxPx: number, dyPx: number) {
    this.view.cx -= dxPx / this.view.scale;
    this.view.cy += dyPx / this.view.scale;
  }

  /** Road under a world point, if any (closest band containing it). */
  hitRoad(p: Pt): number | null {
    if (!this.net) return null;
    let best: number | null = null;
    let bestDist = Infinity;
    for (const r of this.net.roads) {
      const d = unit(sub(r.end, r.start));
      const rel = sub(p, r.start);
      const s = rel[0] * d[0] + rel[1] * d[1];
      if (s < 0 || s > r.length) continue;
      const n = right(d);
      const off = rel[0] * n[0] + rel[1] * n[1];
      const [lo, hi] = roadBand(r);
      if (off < lo - 1 || off > hi + 1) continue;
      const dist = Math.abs(off - (lo + hi) / 2);
      if (dist < bestDist) {
        bestDist = dist;
        best = r.id;
      }
    }
    return best;
  }

  draw(prev: SnapshotMsg | undefined, curr: SnapshotMsg | undefined, alpha: number, opt: RenderOptions, timeMs: number) {
    const { ctx } = this;
    const W = this.width, H = this.height;
    ctx.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);
    ctx.fillStyle = palette.ground;
    ctx.fillRect(0, 0, W, H);
    if (!this.net || !this.geom) return;
    const s = this.view.scale;
    // World -> screen: x' = (x - cx) * s + W/2 ; y' = -(y - cy) * s + H/2
    ctx.setTransform(this.dpr * s, 0, 0, -this.dpr * s, this.dpr * (W / 2 - this.view.cx * s), this.dpr * (H / 2 + this.view.cy * s));
    const px = 1 / s; // one screen pixel in metres

    // 1. roads & lanes
    const g = this.geom;
    ctx.fillStyle = palette.asphalt;
    ctx.fill(g.asphalt);
    ctx.fillStyle = palette.junction;
    ctx.fill(g.junctions);
    ctx.lineCap = "butt";
    ctx.strokeStyle = palette.marking;
    ctx.lineWidth = Math.max(0.15, px);
    ctx.stroke(g.edgeLines);
    ctx.setLineDash([3, 6]);
    ctx.stroke(g.laneDividers);
    ctx.setLineDash([]);
    ctx.strokeStyle = palette.yellow;
    ctx.lineWidth = Math.max(0.12, px * 0.8);
    ctx.stroke(g.centreLines);
    // 2. junction markings
    ctx.strokeStyle = palette.marking;
    ctx.lineWidth = 0.45;
    ctx.stroke(g.stopBars);
    ctx.fillStyle = "rgba(238,240,234,0.85)";
    ctx.fill(g.zebra);

    if (opt.hoverRoad !== null || opt.selectedRoad !== null) {
      for (const id of [opt.hoverRoad, opt.selectedRoad]) {
        if (id === null) continue;
        const r = this.net.roads[id];
        const [lo, hi] = roadBand(r);
        ctx.fillStyle = id === opt.selectedRoad ? "rgba(233,179,42,0.35)" : "rgba(233,179,42,0.18)";
        ctx.fill(bandPath(r, lo, hi, 0, r.length));
      }
    }
    if (!curr) return;

    // 3. active movements traced through the junction + signal heads
    const roadsById = this.net.roads;
    const movementState = new Map<number, string>();
    for (const sig of curr.sig) {
      const node = this.net.nodes[sig.node];
      node.movements.forEach((m, i) => movementState.set(m, sig.mv[i] ?? "R"));
    }
    ctx.lineCap = "round";
    for (const m of this.net.movements) {
      const st = movementState.get(m.id);
      if (st !== "G" && st !== "Y") continue;
      ctx.strokeStyle = st === "G" ? "rgba(63,179,127,0.38)" : "rgba(233,179,42,0.45)";
      ctx.lineWidth = 1.6;
      for (const path of m.paths) {
        ctx.beginPath();
        path.forEach(([x, y], i) => (i ? ctx.lineTo(x, y) : ctx.moveTo(x, y)));
        ctx.stroke();
      }
    }
    for (const node of this.net.nodes) {
      if (!node.signalized) continue;
      for (const arm of node.arms) {
        if (arm.in < 0) continue;
        drawSignalHead(ctx, roadsById[arm.in], this.net.movements.filter((m) => m.in === arm.in), movementState);
      }
    }
    // crosswalk WALK indication
    for (const sig of curr.sig) {
      const node = this.net.nodes[sig.node];
      node.crosswalks.forEach((cw, i) => {
        const st = sig.cw[i];
        if (st !== "W" && st !== "C") return;
        ctx.strokeStyle = st === "W" ? "rgba(63,179,127,0.9)" : "rgba(233,179,42,0.9)";
        ctx.lineWidth = 0.5;
        ctx.setLineDash(st === "C" ? [1, 1] : []);
        ctx.beginPath();
        ctx.moveTo(cw.a[0], cw.a[1]);
        ctx.lineTo(cw.b[0], cw.b[1]);
        ctx.stroke();
        ctx.setLineDash([]);
      });
    }

    // 4. congestion overlay: a band whose length is the queue at the stop line
    if (opt.showCongestion) {
      for (const [roadId, q] of curr.q) {
        if (q <= 0) continue;
        const r = roadsById[roadId];
        const len = Math.min(r.length, (q / r.lanes) * 7.5 + 4);
        const t = Math.min(1, len / r.length);
        const [lo, hi] = roadBand(r);
        ctx.fillStyle = `rgba(${Math.round(233 - 23 * t)}, ${Math.round(150 - 90 * t)}, 42, ${0.14 + 0.24 * t})`;
        ctx.fill(bandPath(r, lo - 0.6, hi + 0.6, r.length - len, r.length));
      }
    }

    // 5. incidents
    for (const id of curr.closed) {
      const r = roadsById[id];
      const [lo, hi] = roadBand(r);
      ctx.fillStyle = "rgba(210,65,47,0.28)";
      ctx.fill(bandPath(r, lo, hi, 0, r.length));
      ctx.strokeStyle = palette.red;
      ctx.lineWidth = 0.8;
      for (const s0 of [3, r.length - 3]) {
        const a = along(r, s0, lo), b = along(r, s0, hi);
        ctx.beginPath();
        ctx.moveTo(a[0], a[1]);
        ctx.lineTo(b[0], b[1]);
        ctx.stroke();
      }
    }
    for (const [roadId, lane, pos] of curr.blk) {
      const r = roadsById[roadId];
      const off = r.lane_offsets[lane];
      for (let k = 0; k < 4; k++) drawCone(ctx, along(r, pos - k * 2.5, off + (k % 2 ? -0.6 : 0.6)));
      const d = unit(sub(r.end, r.start));
      const c = along(r, pos + 1.5, off);
      ctx.save();
      ctx.translate(c[0], c[1]);
      ctx.rotate(Math.atan2(d[1], d[0]));
      ctx.fillStyle = "#8C6A5A";
      ctx.fillRect(-2.2, -0.95, 4.4, 1.9);
      ctx.restore();
    }

    // 6. vehicles (interpolated between the last two snapshots; render-only)
    const vehicles = interpolateVehicles(prev?.veh, curr.veh, alpha);
    const blink = Math.floor(timeMs / 250) % 2 === 0;
    for (const v of vehicles) drawVehicle(ctx, v, opt.colourBySpeed, blink);

    // 7. pedestrians
    for (const [, x, y, crossing] of curr.ped) {
      ctx.fillStyle = crossing ? palette.pedestrian : "rgba(29,42,53,0.7)";
      ctx.beginPath();
      ctx.arc(x, y, 0.45, 0, Math.PI * 2);
      ctx.fill();
    }

    // 8. labels (screen space so text stays upright and readable)
    if (opt.showLabels) {
      ctx.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);
      ctx.font = "600 12px Overpass, system-ui, sans-serif";
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      for (const node of this.net.nodes) {
        const sx = (node.pos[0] - this.view.cx) * s + W / 2;
        const sy = -(node.pos[1] - this.view.cy) * s + H / 2;
        const sig = curr.sig.find((x) => x.node === node.id);
        const label = sig ? `Node ${node.id}, phase ${sig.phase + 1}` : `Node ${node.id}`;
        ctx.fillStyle = "rgba(29,42,53,0.82)";
        const w = ctx.measureText(label).width + 10;
        ctx.fillRect(sx - w / 2, sy - 9, w, 18);
        ctx.fillStyle = "#F4F5F1";
        ctx.fillText(label, sx, sy + 1);
      }
    }
  }
}

function bandPath(r: RoadInfo, lo: number, hi: number, s0: number, s1: number): Path2D {
  const p = new Path2D();
  const a = along(r, s0, lo), b = along(r, s1, lo), c = along(r, s1, hi), d = along(r, s0, hi);
  p.moveTo(a[0], a[1]);
  p.lineTo(b[0], b[1]);
  p.lineTo(c[0], c[1]);
  p.lineTo(d[0], d[1]);
  p.closePath();
  return p;
}

function buildGeometry(net: NetworkMsg): StaticGeometry {
  const g: StaticGeometry = {
    asphalt: new Path2D(),
    junctions: new Path2D(),
    edgeLines: new Path2D(),
    laneDividers: new Path2D(),
    centreLines: new Path2D(),
    stopBars: new Path2D(),
    zebra: new Path2D(),
  };
  const line = (p: Path2D, a: Pt, b: Pt) => {
    p.moveTo(a[0], a[1]);
    p.lineTo(b[0], b[1]);
  };
  for (const r of net.roads) {
    const [lo, hi] = roadBand(r);
    g.asphalt.addPath(bandPath(r, lo - 0.3, hi + 0.3, 0, r.length));
    line(g.edgeLines, along(r, 0, hi - 0.25), along(r, r.length, hi - 0.25));
    for (let i = 1; i < r.lanes; i++) {
      const off = lo + i * r.lane_width;
      line(g.laneDividers, along(r, 0, off), along(r, r.length - 12, off));  // solid near the stop line
      line(g.edgeLines, along(r, r.length - 12, off), along(r, r.length, off));
    }
    if (r.reverse >= 0 && r.id < r.reverse) {
      line(g.centreLines, along(r, 0, 0.15), along(r, r.length, 0.15));
      line(g.centreLines, along(r, 0, -0.15), along(r, r.length, -0.15));
    } else if (r.reverse < 0) {
      line(g.edgeLines, along(r, 0, lo + 0.25), along(r, r.length, lo + 0.25));
    }
    if (net.nodes[r.to].signalized) line(g.stopBars, along(r, r.length - 0.4, lo + 0.2), along(r, r.length - 0.4, hi - 0.2));
  }
  for (const n of net.nodes) {
    if (n.boundary) continue;
    g.junctions.moveTo(n.pos[0] + n.radius, n.pos[1]);
    g.junctions.arc(n.pos[0], n.pos[1], n.radius, 0, Math.PI * 2);
    for (const cw of n.crosswalks) {
      const dir = unit(sub(cw.b, cw.a));
      const len = Math.hypot(cw.b[0] - cw.a[0], cw.b[1] - cw.a[1]);
      const perp: Pt = [-dir[1], dir[0]];
      for (let s = 0.8; s < len - 0.5; s += 1.2) {
        const c: Pt = [cw.a[0] + dir[0] * s, cw.a[1] + dir[1] * s];
        const p0: Pt = [c[0] - perp[0] * 1.5, c[1] - perp[1] * 1.5];
        g.zebra.moveTo(p0[0], p0[1]);
        g.zebra.lineTo(p0[0] + dir[0] * 0.55, p0[1] + dir[1] * 0.55);
        g.zebra.lineTo(p0[0] + dir[0] * 0.55 + perp[0] * 3, p0[1] + dir[1] * 0.55 + perp[1] * 3);
        g.zebra.lineTo(p0[0] + perp[0] * 3, p0[1] + perp[1] * 3);
        g.zebra.closePath();
      }
    }
  }
  return g;
}

/** Arrow glyph for a turn, drawn in a local frame where +y is "ahead". */
function arrowPath(ctx: CanvasRenderingContext2D, turn: string) {
  ctx.beginPath();
  if (turn === "S") {
    ctx.moveTo(0, -0.9);
    ctx.lineTo(0, 0.5);
    ctx.moveTo(-0.45, 0.1);
    ctx.lineTo(0, 0.7);
    ctx.lineTo(0.45, 0.1);
  } else {
    const sgn = turn === "L" ? -1 : 1;  // local frame: +x is the driver's right
    ctx.moveTo(0, -0.9);
    ctx.lineTo(0, 0.2);
    ctx.quadraticCurveTo(0, 0.6, 0.6 * sgn, 0.6);
    ctx.moveTo(0.25 * sgn, 0.2);
    ctx.lineTo(0.75 * sgn, 0.6);
    ctx.lineTo(0.25 * sgn, 1.0);
  }
  ctx.stroke();
}

function drawSignalHead(
  ctx: CanvasRenderingContext2D,
  road: RoadInfo,
  movements: NetworkMsg["movements"],
  state: Map<number, string>,
) {
  const order = { L: 0, U: 0, S: 1, R: 2 } as const;
  const sorted = [...movements].sort((a, b) => order[a.turn] - order[b.turn]);
  const [, hi] = roadBand(road);
  const d = unit(sub(road.end, road.start));
  const base = along(road, road.length - 2.2, hi + 1.6);
  const w = sorted.length * 1.9 + 0.5;
  ctx.save();
  ctx.translate(base[0], base[1]);
  // local frame: +y along the road (towards the junction), +x to the driver's right
  const n = right(d);
  ctx.transform(n[0], n[1], d[0], d[1], 0, 0);
  ctx.fillStyle = "#1D2A35";
  ctx.beginPath();
  ctx.roundRect(-0.6, -1.2, w, 2.4, 0.5);
  ctx.fill();
  ctx.lineWidth = 0.32;
  ctx.lineCap = "round";
  ctx.lineJoin = "round";
  sorted.forEach((m, i) => {
    const st = state.get(m.id) ?? "R";
    ctx.strokeStyle = st === "G" ? palette.green : st === "Y" ? palette.amber : palette.red;
    ctx.save();
    ctx.translate(0.35 + i * 1.9, 0);
    arrowPath(ctx, m.turn);
    ctx.restore();
  });
  ctx.restore();
}

function drawCone(ctx: CanvasRenderingContext2D, p: Pt) {
  ctx.fillStyle = "#E37B2C";
  ctx.beginPath();
  ctx.arc(p[0], p[1], 0.38, 0, Math.PI * 2);
  ctx.fill();
}

function drawVehicle(ctx: CanvasRenderingContext2D, v: DrawVehicle, bySpeed: boolean, blink: boolean) {
  const width = v.type === 1 ? 2.5 : 1.9;
  const len = v.length;
  ctx.save();
  ctx.translate(v.x, v.y);
  ctx.rotate(v.heading);
  let body: string;
  if (bySpeed) body = speedColour(v.speed);
  else if (v.type === 1) body = palette.yellow;
  else if (v.type === 2) body = "#F4F5F1";
  else body = CAR_COLOURS[v.id % CAR_COLOURS.length];
  ctx.fillStyle = body;
  ctx.beginPath();
  ctx.roundRect(-len / 2, -width / 2, len, width, 0.45);
  ctx.fill();
  ctx.strokeStyle = "rgba(20,28,35,0.55)";
  ctx.lineWidth = 0.12;
  ctx.stroke();
  // windscreen
  ctx.fillStyle = "rgba(29,42,53,0.55)";
  if (v.type === 1) ctx.fillRect(len / 2 - 1.2, -width / 2 + 0.25, 0.7, width - 0.5);
  else ctx.fillRect(len / 2 - 1.6, -width / 2 + 0.2, 0.55, width - 0.4);
  if (v.type === 2) {
    ctx.fillStyle = blink ? palette.red : "#2F6FD6";
    ctx.fillRect(-0.3, -width / 2 + 0.15, 0.6, width / 2 - 0.15);
    ctx.fillStyle = blink ? "#2F6FD6" : palette.red;
    ctx.fillRect(-0.3, 0, 0.6, width / 2 - 0.15);
  }
  if (v.speed < 0.5) {  // brake lights
    ctx.fillStyle = "#FF3B2F";
    ctx.fillRect(-len / 2, -width / 2 + 0.1, 0.25, 0.45);
    ctx.fillRect(-len / 2, width / 2 - 0.55, 0.25, 0.45);
  }
  ctx.restore();
}
