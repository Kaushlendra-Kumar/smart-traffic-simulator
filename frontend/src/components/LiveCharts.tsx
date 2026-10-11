import { useEffect, useState } from "react";
import { Area, AreaChart, CartesianGrid, Line, LineChart, ResponsiveContainer, Tooltip, XAxis, YAxis } from "recharts";
import { api } from "../api/client";
import { fmt } from "../format";
import { useSim } from "../state/store";
import type { MetricsMsg } from "../types/protocol";

/** Authoritative time series from the server (polled), so charts survive reconnects. */
export function LiveCharts() {
  const sim = useSim();
  const [metrics, setMetrics] = useState<MetricsMsg | null>(null);
  const running = sim.status?.state === "running";
  const step = sim.status?.step;

  useEffect(() => {
    let cancelled = false;
    const load = () => api.metrics().then((m) => !cancelled && setMetrics(m)).catch(() => {});
    load();
    if (!running) return () => { cancelled = true; };
    const id = window.setInterval(load, 1500);
    return () => {
      cancelled = true;
      window.clearInterval(id);
    };
  }, [running, step === 0, sim.network]);

  const data = metrics?.series ?? [];
  if (data.length < 2)
    return <p className="muted charts-empty">Charts fill in as the simulation runs. Press Run to start.</p>;

  const axis = { tick: { fontSize: 11, fill: "#4b5a66" }, stroke: "#9aa39a" };
  const charts = [
    { title: "Vehicles in network and queued", lines: [["in_network", "#1D2A35", "In network"], ["total_queue", "#D2412F", "Queued"]] },
    { title: "Average delay of completed trips (s)", lines: [["avg_delay", "#1F6B4F", "Delay"]] },
    { title: "Throughput, last minute (veh/h)", lines: [["throughput_vph", "#B7791F", "Throughput"]] },
  ] as const;

  return (
    <div className="charts">
      {charts.map((c, i) => (
        <figure key={c.title}>
          <figcaption>{c.title}</figcaption>
          <ResponsiveContainer width="100%" height={150}>
            {i === 0 ? (
              <AreaChart data={data} margin={{ top: 6, right: 8, bottom: 0, left: -18 }}>
                <CartesianGrid vertical={false} stroke="#d4d7d0" />
                <XAxis dataKey="t" type="number" domain={["dataMin", "dataMax"]} tickFormatter={fmt.clock} {...axis} />
                <YAxis {...axis} />
                <Tooltip labelFormatter={(t) => fmt.clock(Number(t))} />
                {c.lines.map(([k, col, name]) => (
                  <Area key={k} dataKey={k} name={name} stroke={col} fill={col} fillOpacity={0.12} isAnimationActive={false} />
                ))}
              </AreaChart>
            ) : (
              <LineChart data={data} margin={{ top: 6, right: 8, bottom: 0, left: -18 }}>
                <CartesianGrid vertical={false} stroke="#d4d7d0" />
                <XAxis dataKey="t" type="number" domain={["dataMin", "dataMax"]} tickFormatter={fmt.clock} {...axis} />
                <YAxis {...axis} />
                <Tooltip labelFormatter={(t) => fmt.clock(Number(t))} formatter={(v: number) => v.toFixed(1)} />
                {c.lines.map(([k, col, name]) => (
                  <Line key={k} dataKey={k} name={name} stroke={col} dot={false} strokeWidth={2} isAnimationActive={false} />
                ))}
              </LineChart>
            )}
          </ResponsiveContainer>
        </figure>
      ))}
    </div>
  );
}
