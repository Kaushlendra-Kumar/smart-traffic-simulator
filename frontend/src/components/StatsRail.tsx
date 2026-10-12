import { controllerName, fmt } from "../format";
import { useSim } from "../state/store";

export function StatsRail() {
  const sim = useSim();
  const snap = sim.curr?.snap;
  const m = snap?.m;
  const queueNow = snap ? snap.q.reduce((a, [, q]) => a + q, 0) : 0;
  const net = sim.network;

  const measured = (m?.measured_completed ?? 0) > 0;
  const avg = (x: number) => (measured ? fmt.secs(x) : "–");
  const rows: [string, string, string?][] = m
    ? [
        ["Average travel time", avg(m.avg_travel_time), "Completed trips, including time waiting to enter"],
        ["Average delay", m.measured_seconds > 0 ? fmt.secs(m.avg_delay_all) : "–", "Versus free-flow, including trips still under way"],
        ["Vehicles completed", fmt.int(m.completed)],
        ["In the network", fmt.int(m.in_network), m.backlog ? `${m.backlog} waiting to enter` : undefined],
        ["Queued right now", fmt.int(queueNow)],
        ["Average queue", m.measured_seconds > 0 ? fmt.num(m.avg_queue) : "–", "Time-weighted, vehicles below 2 m/s at signals"],
        ["Longest queue", fmt.int(m.max_queue), "Single approach, since warm-up"],
        ["Throughput", m.measured_seconds > 0 ? `${fmt.int(m.throughput_vph)} veh/h` : "–"],
        ["Pedestrian wait", m.measured_seconds > 0 && m.peds_completed > 0 ? fmt.secs(m.ped_avg_wait) : "–", `${fmt.int(m.peds_completed)} crossed`],
        ["Stops per vehicle", measured ? fmt.num(m.avg_stops, 2) : "–"],
      ]
    : [];

  return (
    <aside className="rail" aria-label="Live statistics">
      <h2>Live statistics</h2>
      {!m && <p className="muted">Waiting for the first snapshot.</p>}
      <dl className="stat-list">
        {rows.map(([k, v, hint]) => (
          <div key={k} className="stat">
            <dt>{k}</dt>
            <dd>{v}</dd>
            {hint && <span className="hint">{hint}</span>}
          </div>
        ))}
      </dl>
      {m && m.measured_seconds === 0 && <p className="muted small">Warm-up in progress: averages start after {net?.scenario.warmup ?? 0} s.</p>}

      {snap && snap.sig.length > 0 && (
        <section className="signals">
          <h2>{snap.sig.length === 1 ? "Signal" : `Signals (${snap.sig.length})`}</h2>
          {snap.sig.slice(0, 4).map((s) => {
            const phase = net?.signals.find((x) => x.node === s.node)?.phases[s.phase];
            return (
              <div key={s.node} className="signal-row">
                <div className="signal-head">
                  <span className={`lamp lamp-${s.stage}`} aria-hidden="true" />
                  <span>
                    {snap.sig.length > 1 ? `Node ${s.node}: ` : ""}
                    {phase?.name ?? `Phase ${s.phase + 1}`}
                  </span>
                  <span className="muted">{s.t.toFixed(0)} s</span>
                </div>
                {s.why && <p className="why">{s.why}</p>}
              </div>
            );
          })}
          {snap.sig.length > 4 && <p className="muted small">Turn on labels to see every intersection on the map.</p>}
        </section>
      )}

      {sim.status && (
        <section className="engine">
          <h2>Engine</h2>
          <p className="small">
            {controllerName(snap?.controller ?? "")} control. Simulating at {fmt.num(sim.status.realtime_factor)}× real time,{" "}
            {fmt.num(sim.status.avg_step_us, 0)} µs per step, {fmt.num(sim.status.last_frame_bytes / 1024)} KB per frame.
            {sim.socket.gaps > 0 && ` ${sim.socket.gaps} frames skipped to stay current.`}
          </p>
          {m && (m.red_light_guard_events > 0 || m.following_violations > 0) && (
            <p className="small warn-text">
              Safety guards triggered {m.red_light_guard_events + m.following_violations} times — please report this scenario.
            </p>
          )}
        </section>
      )}
    </aside>
  );
}
