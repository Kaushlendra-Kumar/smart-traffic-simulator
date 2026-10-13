import { useEffect, useMemo, useRef, useState } from "react";
import { Bar, BarChart, CartesianGrid, ErrorBar, Legend, Line, LineChart, ResponsiveContainer, Tooltip, XAxis, YAxis } from "recharts";
import { engineMode } from "../api/backend";
import { api } from "../api/client";
import { controllerName, fmt } from "../format";
import { useSim } from "../state/store";
import type { ExperimentResult, ExperimentStatus, ScenarioSummary } from "../types/protocol";

export const CONTROLLER_COLOURS: Record<string, string> = {
  fixed_time: "#5C7389",
  adaptive: "#1F6B4F",
  max_pressure: "#B7791F",
};
const colourFor = (name: string, i: number) => CONTROLLER_COLOURS[name] ?? ["#5C7389", "#1F6B4F", "#B7791F", "#8A4F7D"][i % 4];

const TABLE_METRICS = [
  "avg_delay_all",
  "avg_travel_time",
  "p95_travel_time",
  "avg_queue",
  "max_queue",
  "throughput_vph",
  "avg_stops",
  "ped_avg_wait",
  "phase_switches",
];

export function ExperimentsPanel() {
  const sim = useSim();
  const [scenarios, setScenarios] = useState<ScenarioSummary[]>([]);
  const [scenario, setScenario] = useState<string>("");
  const [controllers, setControllers] = useState<Record<string, boolean>>({ fixed_time: true, adaptive: true, max_pressure: true });
  const [seeds, setSeeds] = useState(5);
  const [duration, setDuration] = useState(900);
  const [job, setJob] = useState<ExperimentStatus | null>(null);
  const [result, setResult] = useState<ExperimentResult | null>(null);
  const [error, setError] = useState<string | null>(null);
  const poll = useRef<number | null>(null);

  useEffect(() => {
    api.scenarios().then(setScenarios).catch(() => {});
  }, []);
  useEffect(() => {
    if (!scenario && sim.status?.scenario) setScenario(sim.status.scenario);
  }, [sim.status?.scenario]);
  useEffect(() => () => { if (poll.current) window.clearInterval(poll.current); }, []);

  const chosen = (["fixed_time", "adaptive", "max_pressure"] as const).filter((c) => controllers[c]);

  const run = async () => {
    setError(null);
    setResult(null);
    try {
      const { id } = await api.startExperiment({
        scenario,
        controllers: chosen.map((type) => ({ type })),
        seed_count: seeds,
        duration,
      });
      if (poll.current) window.clearInterval(poll.current);
      poll.current = window.setInterval(async () => {
        try {
          const st = await api.experiment(id);
          setJob(st);
          if (st.state === "done" || st.state === "failed") {
            window.clearInterval(poll.current!);
            setResult(await api.experimentResults(id));
          } else if (st.state === "cancelled") {
            window.clearInterval(poll.current!);
          }
        } catch (e) {
          window.clearInterval(poll.current!);
          setError((e as Error).message);
        }
      }, 400);
    } catch (e) {
      setError((e as Error).message);
    }
  };

  const busy = job !== null && (job.state === "queued" || job.state === "running");
  return (
    <div className="experiments">
      <form className="exp-form" onSubmit={(e) => { e.preventDefault(); run(); }}>
        <label className="field">
          <span>Scenario</span>
          <select value={scenario} onChange={(e) => setScenario(e.target.value)}>
            {scenarios.map((s) => <option key={s.id} value={s.id}>{s.name}</option>)}
          </select>
        </label>
        <fieldset className="field">
          <legend>Controllers (first is the baseline)</legend>
          <div className="row">
            {(["fixed_time", "adaptive", "max_pressure"] as const).map((c) => (
              <label key={c} className="check">
                <input type="checkbox" checked={controllers[c]} onChange={(e) => setControllers({ ...controllers, [c]: e.target.checked })} />
                {controllerName(c)}
              </label>
            ))}
          </div>
        </fieldset>
        <label className="field">
          <span>Seeds</span>
          <input type="number" min={1} max={30} value={seeds} onChange={(e) => setSeeds(Number(e.target.value))} />
        </label>
        <label className="field">
          <span>Duration (s)</span>
          <input type="number" min={300} max={7200} step={60} value={duration} onChange={(e) => setDuration(Number(e.target.value))} />
        </label>
        <button className="primary" type="submit" disabled={busy || chosen.length < 1 || !scenario}>
          {busy ? "Running…" : "Run comparison"}
        </button>
      </form>
      <p className="muted small">
        Each seed fixes one demand realisation: every controller sees exactly the same arrivals, routes and drivers, so differences are paired.
        {engineMode === "wasm"
          ? "Runs execute in your browser in a background worker, separate from the live view."
          : "Runs execute in parallel on the server, separate from the live view."}
      </p>
      {busy && job && (
        <div className="progress" role="progressbar" aria-valuemin={0} aria-valuemax={job.progress.total} aria-valuenow={job.progress.done}>
          <div style={{ width: `${(100 * job.progress.done) / Math.max(1, job.progress.total)}%` }} />
          <span>{job.progress.done} of {job.progress.total} runs</span>
          <button className="link" type="button" onClick={() => api.cancelExperiment(job.id)}>Cancel</button>
        </div>
      )}
      {error && <p className="error-text">{error}</p>}
      {result && <ExperimentReport result={result} />}
    </div>
  );
}

function ExperimentReport({ result }: { result: ExperimentResult }) {
  const defs = useMemo(() => Object.fromEntries(result.metric_definitions.map((d) => [d.key, d])), [result]);
  const delayData = result.aggregates.map((a, i) => ({
    name: controllerName(a.controller),
    delay: Number(a.metrics.avg_delay_all.mean.toFixed(2)),
    ci: Number(a.metrics.avg_delay_all.ci95.toFixed(2)),
    fill: colourFor(a.controller, i),
  }));
  // Average the total-queue time series across seeds for each controller.
  const queueSeries = useMemo(() => {
    const byT = new Map<number, Record<string, number>>();
    result.controllers.forEach((c, ci) => {
      const runs = result.runs.filter((r) => r.controller_index === ci && r.series);
      runs.forEach((r) =>
        r.series!.forEach((s) => {
          const row = byT.get(s.t) ?? { t: s.t };
          row[c] = (row[c] ?? 0) + s.total_queue / runs.length;
          byT.set(s.t, row);
        }),
      );
    });
    return [...byT.values()].sort((a, b) => a.t - b.t).filter((_, i) => i % 3 === 0);
  }, [result]);

  return (
    <section className="report" aria-label="Experiment results">
      <div className="report-head">
        <h3>
          {result.controllers.map(controllerName).join(" vs ")} on {result.scenario_id}, {result.seeds.length} seeds × {fmt.int(result.duration)} s
        </h3>
        <a className="button" href={api.csvUrl(result.id)} download>Download CSV</a>
      </div>
      <ul className="findings">
        {result.explanations.map((e, i) => <li key={i}>{e}</li>)}
      </ul>

      <div className="report-charts">
        <figure>
          <figcaption>Average delay per vehicle (s), with 95% confidence interval</figcaption>
          <ResponsiveContainer width="100%" height={220}>
            <BarChart data={delayData} margin={{ top: 10, right: 10, bottom: 0, left: -10 }}>
              <CartesianGrid vertical={false} stroke="#d4d7d0" />
              <XAxis dataKey="name" tick={{ fontSize: 12 }} />
              <YAxis tick={{ fontSize: 12 }} />
              <Tooltip formatter={(v: number) => `${v.toFixed(1)} s`} />
              <Bar dataKey="delay" isAnimationActive={false}>
                <ErrorBar dataKey="ci" width={8} stroke="#1D2A35" />
              </Bar>
            </BarChart>
          </ResponsiveContainer>
        </figure>
        <figure>
          <figcaption>Vehicles queued across the network over time (mean of seeds)</figcaption>
          <ResponsiveContainer width="100%" height={220}>
            <LineChart data={queueSeries} margin={{ top: 10, right: 10, bottom: 0, left: -10 }}>
              <CartesianGrid vertical={false} stroke="#d4d7d0" />
              <XAxis dataKey="t" type="number" domain={["dataMin", "dataMax"]} tickFormatter={(t) => fmt.clock(t)} tick={{ fontSize: 12 }} />
              <YAxis tick={{ fontSize: 12 }} />
              <Tooltip labelFormatter={(t) => fmt.clock(Number(t))} formatter={(v: number) => v.toFixed(1)} />
              <Legend formatter={(v) => controllerName(String(v))} />
              {result.controllers.map((c, i) => (
                <Line key={c} dataKey={c} stroke={colourFor(c, i)} dot={false} strokeWidth={2} isAnimationActive={false} />
              ))}
            </LineChart>
          </ResponsiveContainer>
        </figure>
      </div>

      <div className="table-scroll">
        <table className="results">
          <thead>
            <tr>
              <th>Metric</th>
              {result.aggregates.map((a) => <th key={a.controller}>{controllerName(a.controller)}</th>)}
              {result.comparisons.map((c) => <th key={c.candidate}>{controllerName(c.candidate)} vs {controllerName(c.baseline)}</th>)}
            </tr>
          </thead>
          <tbody>
            {TABLE_METRICS.filter((k) => defs[k]).map((k) => {
              const d = defs[k];
              return (
                <tr key={k}>
                  <th scope="row">{d.label} <span className="muted">({d.unit})</span></th>
                  {result.aggregates.map((a) => {
                    const m = a.metrics[k];
                    return <td key={a.controller}>{m && m.n > 0 ? <>{fmt.num(m.mean, k === "avg_stops" ? 2 : 1)} <span className="muted">± {fmt.num(m.ci95, 1)}</span></> : "n/a"}</td>;
                  })}
                  {result.comparisons.map((c) => {
                    const m = c.metrics[k];
                    if (!m || m.n === 0) return <td key={c.candidate}>n/a</td>;
                    const better = d.lower_is_better ? m.diff_mean < 0 : m.diff_mean > 0;
                    const significant = m.n > 1 && Math.abs(m.diff_mean) > m.diff_ci95;
                    return (
                      <td key={c.candidate} className={significant ? (better ? "better" : "worse") : "neutral"}>
                        {fmt.signed(m.diff_mean)} ({fmt.pct(m.pct_change)})
                        <span className="muted small"> better in {m.candidate_better}/{m.n}</span>
                      </td>
                    );
                  })}
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
      <p className="muted small">
        Differences are paired by seed. Green and red mark differences larger than the 95% confidence interval; grey ones are within seed-to-seed noise.
        Computed in {fmt.num(result.wall_seconds, 1)} s.
      </p>
    </section>
  );
}
