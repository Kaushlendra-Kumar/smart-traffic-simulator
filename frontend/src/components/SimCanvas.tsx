import { useEffect, useRef, useState } from "react";
import { engineMode, remoteBackend } from "../api/backend";
import { TrafficRenderer } from "../canvas/renderer";
import { store, useSim } from "../state/store";

export interface CanvasOptions {
  colourBySpeed: boolean;
  showCongestion: boolean;
  showLabels: boolean;
}

interface Props {
  options: CanvasOptions;
  selectedRoad: number | null;
  onSelectRoad: (road: number | null) => void;
}

export function SimCanvas({ options, selectedRoad, onSelectRoad }: Props) {
  const sim = useSim();
  const wrapRef = useRef<HTMLDivElement>(null);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const rendererRef = useRef<TrafficRenderer | null>(null);
  const hoverRef = useRef<number | null>(null);
  const optsRef = useRef({ ...options, selectedRoad });
  const fittedFor = useRef<object | null>(null);
  const [hoverName, setHoverName] = useState<string | null>(null);
  optsRef.current = { ...options, selectedRoad };

  // Renderer lifecycle + animation loop (reads the store directly; no React renders per frame).
  useEffect(() => {
    const canvas = canvasRef.current!;
    const wrap = wrapRef.current!;
    const renderer = new TrafficRenderer(canvas);
    rendererRef.current = renderer;
    const ro = new ResizeObserver(() => {
      renderer.resize(wrap.clientWidth, wrap.clientHeight);
      if (fittedFor.current === store.network) renderer.fit();
    });
    ro.observe(wrap);
    renderer.resize(wrap.clientWidth, wrap.clientHeight);
    let raf = 0;
    const loop = (now: number) => {
      renderer.setNetwork(store.network);
      if (store.network && fittedFor.current !== store.network) {
        fittedFor.current = store.network;
        renderer.fit();
      }
      // Render one frame interval behind the newest snapshot and blend towards it.
      let alpha = 1;
      if (store.prev && store.curr) alpha = (now - store.curr.receivedAt) / Math.max(16, store.frameInterval);
      renderer.draw(store.prev?.snap, store.curr?.snap, alpha, { ...optsRef.current, hoverRoad: hoverRef.current }, now);
      raf = requestAnimationFrame(loop);
    };
    raf = requestAnimationFrame(loop);
    return () => {
      cancelAnimationFrame(raf);
      ro.disconnect();
    };
  }, []);

  // Pointer interactions: drag to pan, wheel to zoom, click to select a road.
  useEffect(() => {
    const canvas = canvasRef.current!;
    let drag: { x: number; y: number; moved: boolean } | null = null;
    const local = (e: PointerEvent | WheelEvent) => {
      const r = canvas.getBoundingClientRect();
      return [e.clientX - r.left, e.clientY - r.top] as const;
    };
    const down = (e: PointerEvent) => {
      drag = { x: e.clientX, y: e.clientY, moved: false };
      canvas.setPointerCapture(e.pointerId);
    };
    const move = (e: PointerEvent) => {
      const renderer = rendererRef.current;
      if (!renderer) return;
      if (drag) {
        const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
        if (Math.abs(dx) + Math.abs(dy) > 3) drag.moved = true;
        if (drag.moved) {
          renderer.panBy(dx, dy);
          drag.x = e.clientX;
          drag.y = e.clientY;
        }
        return;
      }
      const [x, y] = local(e);
      const id = renderer.hitRoad(renderer.screenToWorld(x, y));
      if (id !== hoverRef.current) {
        hoverRef.current = id;
        const road = id !== null ? store.network?.roads[id] : undefined;
        setHoverName(road ? `${road.name || "Road"} ${road.from}→${road.to}, ${road.lanes} lanes, ${road.speed_limit_kmh} km/h` : null);
      }
    };
    const up = (e: PointerEvent) => {
      const renderer = rendererRef.current;
      if (drag && !drag.moved && renderer) {
        const [x, y] = local(e);
        onSelectRoad(renderer.hitRoad(renderer.screenToWorld(x, y)));
      }
      drag = null;
    };
    const wheel = (e: WheelEvent) => {
      e.preventDefault();
      const [x, y] = local(e);
      rendererRef.current?.zoomAt(x, y, Math.exp(-e.deltaY * 0.0015));
    };
    const leave = () => {
      hoverRef.current = null;
      setHoverName(null);
    };
    canvas.addEventListener("pointerdown", down);
    canvas.addEventListener("pointermove", move);
    canvas.addEventListener("pointerup", up);
    canvas.addEventListener("pointerleave", leave);
    canvas.addEventListener("wheel", wheel, { passive: false });
    return () => {
      canvas.removeEventListener("pointerdown", down);
      canvas.removeEventListener("pointermove", move);
      canvas.removeEventListener("pointerup", up);
      canvas.removeEventListener("pointerleave", leave);
      canvas.removeEventListener("wheel", wheel);
    };
  }, [onSelectRoad]);

  const keyNav = (e: React.KeyboardEvent) => {
    const r = rendererRef.current;
    if (!r) return;
    const step = 60;
    const actions: Record<string, () => void> = {
      ArrowLeft: () => r.panBy(step, 0),
      ArrowRight: () => r.panBy(-step, 0),
      ArrowUp: () => r.panBy(0, step),
      ArrowDown: () => r.panBy(0, -step),
      "+": () => r.zoomAt(r.width / 2, r.height / 2, 1.25),
      "=": () => r.zoomAt(r.width / 2, r.height / 2, 1.25),
      "-": () => r.zoomAt(r.width / 2, r.height / 2, 0.8),
      "0": () => r.fit(),
      Escape: () => onSelectRoad(null),
    };
    const action = actions[e.key];
    if (action) {
      e.preventDefault();
      action();
    }
  };

  let overlay: { title: string; body: string } | null = null;
  if (sim.connection !== "open" && !sim.network && engineMode === "wasm")
    overlay =
      sim.connection === "closed"
        ? {
            title: "The simulation engine couldn't start",
            body: "This page runs the C++ engine in your browser with WebAssembly. Try a current version of Chrome, Edge, Firefox or Safari.",
          }
        : { title: "Starting the simulation engine…", body: "Loading the C++ engine (WebAssembly) into your browser." };
  else if (sim.connection !== "open" && !sim.network)
    overlay = remoteBackend
      ? {
          title: "Waking up the simulation server…",
          body: "The free server sleeps after 15 minutes without visitors and takes about a minute to start. This page connects as soon as it's up.",
        }
      : {
          title: sim.connection === "connecting" ? "Connecting to the simulation server…" : "Can't reach the simulation server",
          body: "Start it with ./build/release/traffic_server (or docker compose up) from the repository root. This page reconnects automatically.",
        };
  else if (sim.connection === "closed")
    overlay = { title: "Connection lost", body: "Reconnecting. The view shows the last state received." };

  return (
    <div className="canvas-wrap" ref={wrapRef}>
      <canvas
        ref={canvasRef}
        tabIndex={0}
        onKeyDown={keyNav}
        onDoubleClick={() => rendererRef.current?.fit()}
        aria-label="Road network view. Drag to pan, scroll to zoom, click a road to select it, double-click to fit."
      />
      {overlay && (
        <div className="canvas-overlay" role="status">
          <strong>{overlay.title}</strong>
          <span>{overlay.body}</span>
        </div>
      )}
      {sim.status?.state === "paused" && !overlay && <div className="canvas-chip">Paused</div>}
      {sim.status?.state === "ready" && !overlay && <div className="canvas-chip">Ready — press Run</div>}
      {sim.status?.lagging && <div className="canvas-chip warn">Engine can't keep up at this speed</div>}
      {hoverName && <div className="canvas-hover">{hoverName}</div>}
      <Legend />
    </div>
  );
}

function Legend() {
  return (
    <details className="legend">
      <summary>Legend</summary>
      <ul>
        <li><span className="sw arrow-g" /> Movement has green</li>
        <li><span className="sw arrow-y" /> Yellow (clearing)</li>
        <li><span className="sw arrow-r" /> Red</li>
        <li><span className="sw trace" /> Path currently served</li>
        <li><span className="sw queue" /> Queue at the stop line</li>
        <li><span className="sw bus" /> Bus</li>
        <li><span className="sw ems" /> Emergency vehicle</li>
        <li><span className="sw ped" /> Pedestrian</li>
        <li><span className="sw closed" /> Closed road</li>
      </ul>
    </details>
  );
}
