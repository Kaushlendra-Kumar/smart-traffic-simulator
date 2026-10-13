import { useEffect, useState } from "react";
import { engineMode } from "../api/backend";
import { api } from "../api/client";
import { controllerName, fmt } from "../format";
import { store, useSim } from "../state/store";
import type { ScenarioSummary } from "../types/protocol";

const SPEEDS = [0.5, 1, 2, 5, 10, 20, 50];

export function TopBar() {
  const sim = useSim();
  const [scenarios, setScenarios] = useState<ScenarioSummary[]>([]);
  const status = sim.status;
  const running = status?.state === "running";

  useEffect(() => {
    api.scenarios().then(setScenarios).catch(() => setScenarios([]));
  }, [sim.connection]);

  const t = sim.curr?.snap.t ?? status?.sim_time ?? 0;
  return (
    <header className="topbar">
      <div className="brand">
        <svg viewBox="0 0 14 32" width="11" height="26" aria-hidden="true">
          <rect width="14" height="32" rx="5" fill="#1D2A35" />
          <circle cx="7" cy="7" r="3.4" fill={running ? "#5a2a24" : "#D2412F"} />
          <circle cx="7" cy="16" r="3.4" fill="#5c4a1e" />
          <circle cx="7" cy="25" r="3.4" fill={running ? "#3FB37F" : "#24453a"} />
        </svg>
        <span>Smart Traffic Lab</span>
      </div>

      <label className="field inline">
        <span>Scenario</span>
        <select
          value={status?.scenario ?? ""}
          onChange={(e) => store.command({ cmd: "load_scenario", scenario: e.target.value }, "Scenario loaded")}
          disabled={sim.connection !== "open"}
        >
          {scenarios.map((s) => (
            <option key={s.id} value={s.id}>
              {s.name}
            </option>
          ))}
        </select>
      </label>

      <div className="transport" role="group" aria-label="Simulation controls">
        <button
          className="primary"
          onClick={() => store.command({ cmd: running ? "pause" : status?.state === "ready" ? "start" : "resume" })}
          disabled={sim.connection !== "open"}
        >
          {running ? "Pause" : status?.state === "ready" ? "Run" : "Resume"}
        </button>
        <button onClick={() => store.command({ cmd: "step", count: 10 })} disabled={running || sim.connection !== "open"} title="Advance 1 simulated second (10 steps)">
          Step 1 s
        </button>
        <button onClick={() => store.command({ cmd: "reset" }, "Reset to the start of the scenario")} disabled={sim.connection !== "open"}>
          Reset
        </button>
      </div>

      <label className="field inline">
        <span>Speed</span>
        <select value={status?.speed ?? 1} onChange={(e) => store.command({ cmd: "set_speed", speed: Number(e.target.value) })}>
          {SPEEDS.map((s) => (
            <option key={s} value={s}>
              {s}×
            </option>
          ))}
        </select>
      </label>

      <div className="clock" aria-live="off">
        <span className="clock-time">{fmt.clock(t)}</span>
        <span className="clock-meta">{controllerName(sim.curr?.snap.controller ?? status?.controller ?? "")}, seed {status?.seed ?? "–"}</span>
      </div>

      <div
        className={`conn conn-${sim.connection}`}
        title={engineMode === "wasm" ? "The C++ engine runs in this browser (WebAssembly)" : `WebSocket ${sim.connection}`}
      >
        {sim.connection === "open"
          ? engineMode === "wasm" ? "In browser" : "Live"
          : sim.connection === "connecting" ? "Connecting" : "Offline"}
      </div>
    </header>
  );
}
