import { useCallback, useEffect, useState } from "react";
import { ExperimentsPanel } from "./components/Experiments";
import { LiveCharts } from "./components/LiveCharts";
import { ControllerPanel, DemandPanel, IncidentPanel, SignalPanel } from "./components/Panels";
import { SimCanvas, type CanvasOptions } from "./components/SimCanvas";
import { StatsRail } from "./components/StatsRail";
import { TopBar } from "./components/TopBar";
import { store, useSim } from "./state/store";

const TABS = [
  { id: "demand", label: "Demand" },
  { id: "signals", label: "Signal plan" },
  { id: "controller", label: "Controller" },
  { id: "incidents", label: "Incidents" },
  { id: "experiments", label: "Experiments" },
] as const;
type TabId = (typeof TABS)[number]["id"];

export default function App() {
  const sim = useSim();
  const [tab, setTab] = useState<TabId>("demand");
  const [road, setRoad] = useState<number | null>(null);
  const [opts, setOpts] = useState<CanvasOptions>({ colourBySpeed: false, showCongestion: true, showLabels: false });

  useEffect(() => {
    store.socket.connect();
    return () => store.socket.close();
  }, []);
  useEffect(() => setRoad(null), [sim.network]);

  const selectRoad = useCallback((id: number | null) => {
    setRoad(id);
    if (id !== null) setTab("incidents");
  }, []);

  return (
    <div className="app">
      <TopBar />
      <main className="stage">
        <div className="canvas-col">
          <SimCanvas options={opts} selectedRoad={road} onSelectRoad={selectRoad} />
          <div className="view-options" role="group" aria-label="Map display options">
            {sim.network && <span className="scenario-desc">{sim.network.scenario.description}</span>}
            <label className="check"><input type="checkbox" checked={opts.showCongestion} onChange={(e) => setOpts({ ...opts, showCongestion: e.target.checked })} />Queues</label>
            <label className="check"><input type="checkbox" checked={opts.colourBySpeed} onChange={(e) => setOpts({ ...opts, colourBySpeed: e.target.checked })} />Colour by speed</label>
            <label className="check"><input type="checkbox" checked={opts.showLabels} onChange={(e) => setOpts({ ...opts, showLabels: e.target.checked })} />Labels</label>
          </div>
        </div>
        <StatsRail />
      </main>

      <section className="controls">
        <div className="tabs" role="tablist" aria-label="Controls">
          {TABS.map((t) => (
            <button key={t.id} role="tab" id={`tab-${t.id}`} aria-selected={tab === t.id} aria-controls={`panel-${t.id}`}
              className={tab === t.id ? "on" : undefined} onClick={() => setTab(t.id)}>
              {t.label}
            </button>
          ))}
        </div>
        <div className="tab-body" role="tabpanel" id={`panel-${tab}`} aria-labelledby={`tab-${tab}`}>
          {tab === "demand" && <DemandPanel />}
          {tab === "signals" && <SignalPanel />}
          {tab === "controller" && <ControllerPanel />}
          {tab === "incidents" && <IncidentPanel road={road} onClear={() => setRoad(null)} />}
          {tab === "experiments" && <ExperimentsPanel />}
        </div>
      </section>

      {tab !== "experiments" && (
        <section className="chart-strip" aria-label="Live charts">
          <LiveCharts />
        </section>
      )}

      <div className="toasts" aria-live="polite">
        {sim.toasts.map((t) => <div key={t.id} className={`toast ${t.kind}`}>{t.text}</div>)}
      </div>
    </div>
  );
}
