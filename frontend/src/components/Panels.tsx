import { useEffect, useMemo, useState } from "react";
import { controllerName, fmt } from "../format";
import { store, useSim } from "../state/store";
import type { ControllerConfig } from "../types/protocol";

// ---------------------------------------------------------------- demand

export function DemandPanel() {
  const sim = useSim();
  const snap = sim.curr?.snap;
  const [demand, setDemand] = useState(1);
  const [peds, setPeds] = useState(1);
  const [bus, setBus] = useState(true);
  const [ems, setEms] = useState(true);
  const boundary = sim.network?.boundary_nodes ?? [];
  const [origin, setOrigin] = useState<number | "">("");
  const [dest, setDest] = useState<number | "">("");

  // Follow the server's value whenever it changes (e.g. after reset or another client).
  useEffect(() => {
    if (snap) setDemand(snap.demand);
  }, [snap?.demand]);
  useEffect(() => {
    if (snap) setPeds(snap.ped_demand);
  }, [snap?.ped_demand]);
  useEffect(() => {
    setOrigin(boundary[0] ?? "");
    setDest(boundary[Math.min(2, boundary.length - 1)] ?? "");
  }, [sim.network]);

  return (
    <div className="panel-grid">
      <div className="control">
        <label htmlFor="demand">Vehicle demand</label>
        <input id="demand" type="range" min={0} max={3} step={0.05} value={demand}
          onChange={(e) => setDemand(Number(e.target.value))}
          onPointerUp={() => store.command({ cmd: "set_demand", multiplier: demand })}
          onKeyUp={() => store.command({ cmd: "set_demand", multiplier: demand })} />
        <output>{demand.toFixed(2)}× of the scenario rate{snap ? `, about ${fmt.int(snap.demand_vph * (demand / Math.max(snap.demand, 1e-9)))} veh/h now` : ""}</output>
      </div>
      <div className="control">
        <label htmlFor="peds">Pedestrian demand</label>
        <input id="peds" type="range" min={0} max={5} step={0.1} value={peds}
          onChange={(e) => setPeds(Number(e.target.value))}
          onPointerUp={() => store.command({ cmd: "set_pedestrian_demand", multiplier: peds })}
          onKeyUp={() => store.command({ cmd: "set_pedestrian_demand", multiplier: peds })} />
        <output>{peds.toFixed(1)}× of the scenario rate</output>
      </div>
      <fieldset className="control">
        <legend>Vehicle types</legend>
        <label className="check">
          <input type="checkbox" checked={bus} onChange={(e) => { setBus(e.target.checked); store.command({ cmd: "set_vehicle_type", type: "bus", enabled: e.target.checked }); }} />
          Buses
        </label>
        <label className="check">
          <input type="checkbox" checked={ems} onChange={(e) => { setEms(e.target.checked); store.command({ cmd: "set_vehicle_type", type: "emergency", enabled: e.target.checked }); }} />
          Emergency vehicles
        </label>
        <p className="muted small">Disabled types are replaced by cars, so the arrival pattern stays the same.</p>
      </fieldset>
      <fieldset className="control">
        <legend>Send an emergency vehicle</legend>
        <div className="row">
          <label className="field">
            <span>From</span>
            <select value={origin} onChange={(e) => setOrigin(Number(e.target.value))}>
              {boundary.map((n) => <option key={n} value={n}>Edge node {n}</option>)}
            </select>
          </label>
          <label className="field">
            <span>To</span>
            <select value={dest} onChange={(e) => setDest(Number(e.target.value))}>
              {boundary.map((n) => <option key={n} value={n}>Edge node {n}</option>)}
            </select>
          </label>
          <button onClick={() => store.command({ cmd: "spawn_vehicle", type: "emergency", origin, destination: dest }, "Emergency vehicle dispatched")}>
            Dispatch
          </button>
        </div>
      </fieldset>
    </div>
  );
}

// ---------------------------------------------------------------- signal plans

export function SignalPanel() {
  const sim = useSim();
  const net = sim.network;
  const [node, setNode] = useState<number | null>(null);
  const signals = net?.signals ?? [];
  const signal = signals.find((s) => s.node === node) ?? signals[0];
  const [greens, setGreens] = useState<number[]>([]);
  const [timing, setTiming] = useState<{ min: number; max: number }[]>([]);
  const isFixed = (sim.curr?.snap.controller ?? net?.controller.type) === "fixed_time";

  useEffect(() => {
    if (!signal) return;
    const override = net?.controller.node_greens?.[String(signal.node)];
    setGreens(signal.phases.map((p, i) => override?.[i] ?? net?.controller.greens?.[i] ?? p.green));
    setTiming(signal.phases.map((p) => ({ min: p.min_green, max: p.max_green })));
  }, [signal, net]);

  if (!net || !signal) return <p className="muted">This scenario has no signalised intersections.</p>;
  const cycle = signal.phases.reduce((acc, p, i) => acc + (greens[i] ?? p.green) + p.yellow + p.all_red, 0);
  const live = sim.curr?.snap.sig.find((s) => s.node === signal.node);

  return (
    <div>
      <div className="row">
        {signals.length > 1 && (
          <label className="field inline">
            <span>Intersection</span>
            <select value={signal.node} onChange={(e) => setNode(Number(e.target.value))}>
              {signals.map((s) => <option key={s.node} value={s.node}>Node {s.node}</option>)}
            </select>
          </label>
        )}
        <p className="muted small grow">
          Fixed-time cycle {fmt.num(cycle, 0)} s including yellow and all-red.
          {!isFixed && " Green durations apply when the fixed-time controller is active; min/max limits apply to every controller."}
        </p>
      </div>
      <table className="plan">
        <thead>
          <tr><th>Phase</th><th>Fixed green (s)</th><th>Min green</th><th>Max green</th><th>Clearance</th></tr>
        </thead>
        <tbody>
          {signal.phases.map((p, i) => (
            <tr key={i} className={live?.phase === i ? "active" : undefined}>
              <td>
                {p.name}
                {p.crosswalks.length > 0 && <span className="muted small"> with walk {p.walk_time}+{fmt.num(p.ped_clearance, 0)} s</span>}
              </td>
              <td>
                <input type="number" min={p.min_green} max={p.max_green} step={1} value={greens[i] ?? p.green}
                  disabled={p.movements.length === 0}
                  onChange={(e) => setGreens(greens.map((g, k) => (k === i ? Number(e.target.value) : g)))} />
              </td>
              <td>
                <input type="number" min={1} max={300} value={timing[i]?.min ?? p.min_green}
                  onChange={(e) => setTiming(timing.map((t, k) => (k === i ? { ...t, min: Number(e.target.value) } : t)))} />
              </td>
              <td>
                <input type="number" min={1} max={600} value={timing[i]?.max ?? p.max_green}
                  onChange={(e) => setTiming(timing.map((t, k) => (k === i ? { ...t, max: Number(e.target.value) } : t)))} />
              </td>
              <td className="muted">{p.yellow}+{p.all_red} s</td>
            </tr>
          ))}
        </tbody>
      </table>
      <div className="row actions">
        <button className="primary" onClick={async () => {
          for (let i = 0; i < signal.phases.length; i++) {
            const p = signal.phases[i];
            const t = timing[i];
            if (t && (t.min !== p.min_green || t.max !== p.max_green)) {
              const r = await store.command({ cmd: "set_phase_timing", node: signal.node, phase: i, min_green: t.min, max_green: t.max });
              if (!r.ok) return;
            }
          }
          await store.command({ cmd: "set_signal_plan", node: signal.node, greens }, "Signal plan saved");
        }}>Save signal plan</button>
        <span className="muted small">Saved plans survive Reset. Every value is validated by the server's safety layer.</span>
      </div>
    </div>
  );
}

// ---------------------------------------------------------------- controller

const DESCRIPTIONS: Record<ControllerConfig["type"], string> = {
  fixed_time: "Cycles through every phase with set green times, whether or not anyone is waiting. The baseline for comparisons.",
  adaptive:
    "Scores each phase from queue length, accumulated and oldest wait, arrivals and pedestrians. Extends a green while vehicles are still discharging, skips empty phases, and switches only when a rival is clearly more urgent.",
  max_pressure:
    "Every few seconds serves the phase with the largest difference between vehicles waiting upstream and vehicles already on the roads they would enter.",
};

export function ControllerPanel() {
  const sim = useSim();
  const active = (sim.curr?.snap.controller ?? sim.network?.controller.type ?? "fixed_time") as ControllerConfig["type"];
  const base = sim.network?.controller;
  const [cfg, setCfg] = useState<ControllerConfig>({ type: active });
  useEffect(() => {
    if (base) setCfg({ ...base, type: active });
  }, [base, active]);
  const num = (key: keyof ControllerConfig, label: string, step = 0.1, help?: string) => (
    <label className="field" title={help}>
      <span>{label}</span>
      <input type="number" step={step} value={(cfg[key] as number | undefined) ?? 0}
        onChange={(e) => setCfg({ ...cfg, [key]: Number(e.target.value) })} />
    </label>
  );
  return (
    <div>
      <div className="segmented" role="radiogroup" aria-label="Controller">
        {(["fixed_time", "adaptive", "max_pressure"] as const).map((t) => (
          <button key={t} role="radio" aria-checked={cfg.type === t} className={cfg.type === t ? "on" : undefined}
            onClick={() => setCfg({ ...cfg, type: t })}>
            {controllerName(t)}
            {active === t && <span className="dot" title="Currently running" />}
          </button>
        ))}
      </div>
      <p className="desc">{DESCRIPTIONS[cfg.type]}</p>
      {cfg.type === "adaptive" && (
        <div className="params">
          {num("w_queue", "Queue weight", 0.1, "Points per queued vehicle")}
          {num("w_wait", "Wait weight", 0.1, "Points per minute of accumulated waiting")}
          {num("w_oldest", "Oldest-wait weight", 0.1, "Points per 30 s waited by the oldest vehicle")}
          {num("w_arrivals", "Arrival weight", 0.1, "Points per approaching vehicle")}
          {num("hysteresis_ratio", "Switch margin (ratio)", 0.05, "A rival must beat the current phase by this fraction")}
          {num("gap_out_seconds", "Gap-out (s)", 0.5, "End a green when no vehicle will reach the line within this time")}
          {num("max_wait", "Max vehicle wait (s)", 5, "Starvation guard")}
          {num("max_pedestrian_wait", "Max pedestrian wait (s)", 5, "Starvation guard for crossings")}
          <label className="check">
            <input type="checkbox" checked={cfg.emergency_priority ?? true} onChange={(e) => setCfg({ ...cfg, emergency_priority: e.target.checked })} />
            Give emergency vehicles priority
          </label>
        </div>
      )}
      {cfg.type === "max_pressure" && (
        <div className="params">
          {num("decision_interval", "Decision interval (s)", 1)}
          {num("max_wait", "Max vehicle wait (s)", 5, "Starvation guard")}
          {num("max_pedestrian_wait", "Max pedestrian wait (s)", 5)}
        </div>
      )}
      <div className="row actions">
        <button className="primary" onClick={() => store.command({ cmd: "set_controller", controller: cfg }, `${controllerName(cfg.type)} control active`)}>
          Use {controllerName(cfg.type).toLowerCase()} control
        </button>
        <span className="muted small">Switching takes effect immediately; the current phase finishes safely.</span>
      </div>
    </div>
  );
}

// ---------------------------------------------------------------- incidents

export function IncidentPanel({ road, onClear }: { road: number | null; onClear: () => void }) {
  const sim = useSim();
  const net = sim.network;
  const snap = sim.curr?.snap;
  const r = road !== null ? net?.roads[road] : undefined;
  const [lane, setLane] = useState(0);
  const [pos, setPos] = useState(120);
  const [kmh, setKmh] = useState(50);
  useEffect(() => {
    if (r) {
      setLane(0);
      setPos(Math.round(Math.max(40, Math.min(r.length - 5, r.length * 0.55))));
      setKmh(Math.round(r.speed_limit_kmh));
    }
  }, [road]);
  const closed = useMemo(() => new Set(snap?.closed ?? []), [snap?.closed]);
  const isBoundaryRoad = r && net ? net.nodes[r.from].boundary || net.nodes[r.to].boundary : false;

  return (
    <div className="panel-grid">
      <div className="control">
        {!r ? (
          <p className="muted">Click a road on the map to close it, block a lane or change its speed limit.</p>
        ) : (
          <>
            <h3>
              {r.name || "Road"} from node {r.from} to node {r.to}
              <button className="link" onClick={onClear}>Deselect</button>
            </h3>
            <p className="muted small">{r.lanes} lanes, {fmt.num(r.length, 0)} m.</p>
            <div className="row">
              {closed.has(r.id) ? (
                <button onClick={() => store.command({ cmd: "open_road", road: r.id }, "Road reopened")}>Reopen road</button>
              ) : (
                <button disabled={isBoundaryRoad} title={isBoundaryRoad ? "Entry and exit roads can't be closed" : undefined}
                  onClick={() => store.command({ cmd: "close_road", road: r.id }, "Road closed; vehicles are rerouting")}>
                  Close road
                </button>
              )}
            </div>
            <div className="row">
              <label className="field">
                <span>Lane</span>
                <select value={lane} onChange={(e) => setLane(Number(e.target.value))}>
                  {Array.from({ length: r.lanes }, (_, i) => <option key={i} value={i}>{i === 0 ? "Inner (1)" : i === r.lanes - 1 ? `Outer (${i + 1})` : `Lane ${i + 1}`}</option>)}
                </select>
              </label>
              <label className="field">
                <span>Position (m)</span>
                <input type="number" min={40} max={Math.floor(r.length - 5)} value={pos} onChange={(e) => setPos(Number(e.target.value))} />
              </label>
              <button onClick={() => store.command({ cmd: "block_lane", road: r.id, lane, position: pos }, "Lane blocked")}>Block lane</button>
            </div>
            <div className="row">
              <label className="field">
                <span>Speed limit (km/h)</span>
                <input type="number" min={10} max={130} value={kmh} onChange={(e) => setKmh(Number(e.target.value))} />
              </label>
              <button onClick={() => store.command({ cmd: "set_speed_limit", road: r.id, kmh }, "Speed limit changed")}>Set limit</button>
            </div>
          </>
        )}
      </div>
      <div className="control">
        <h3>Active incidents</h3>
        {(snap?.closed.length ?? 0) + (snap?.blk.length ?? 0) === 0 && <p className="muted">None. Traffic flows normally.</p>}
        <ul className="incidents">
          {snap?.closed.map((id) => (
            <li key={`c${id}`}>
              Road {net?.roads[id].from}→{net?.roads[id].to} closed
              <button className="link" onClick={() => store.command({ cmd: "open_road", road: id }, "Road reopened")}>Reopen</button>
            </li>
          ))}
          {snap?.blk.map(([id, l, p]) => (
            <li key={`b${id}-${l}`}>
              Lane {l + 1} of road {net?.roads[id].from}→{net?.roads[id].to} blocked at {fmt.num(p, 0)} m
              <button className="link" onClick={() => store.command({ cmd: "clear_lane", road: id, lane: l }, "Lane cleared")}>Clear</button>
            </li>
          ))}
        </ul>
      </div>
    </div>
  );
}
