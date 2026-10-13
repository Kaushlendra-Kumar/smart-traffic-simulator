// Runs the live simulation (C++ engine compiled to WebAssembly) off the main thread.
// Mirrors the native SimulationCoordinator: accumulator-based fixed-step pacing,
// snapshots at 20 Hz while running, status about once per second and after commands,
// and acknowledgements sent only after the state reflecting a command was published.

import createTrafficEngine, { type LiveSim } from "../wasm/traffic-engine.js";
import type { FromLiveWorker, ToLiveWorker } from "./protocol";

const ctx = self as unknown as {
  postMessage(message: FromLiveWorker): void;
  onmessage: ((e: MessageEvent<ToLiveWorker>) => void) | null;
};

const PUBLISH_MS = 50;
const STATUS_MS = 1000;
const TICK_MS = 16;

let sim: LiveSim | null = null;
let scenarios: Record<string, string> = {};
let seq = 0;
let accumulator = 0;
let lastTick = 0;
let lastPublish = 0;
let lastStatus = 0;
let lagging = false;
let stepAvgUs = 0;
let serializeAvgUs = 0;
let lastFrameBytes = 0;
let rateWindowStart = 0;
let rateWindowSim = 0;
let realtimeFactor = 0;

const send = (m: FromLiveWorker) => ctx.postMessage(m);
const emit = (json: string) => send({ type: "message", json });

function publishSnapshot() {
  const t0 = performance.now();
  const json = sim!.snapshot(++seq);
  const us = (performance.now() - t0) * 1000;
  serializeAvgUs = serializeAvgUs === 0 ? us : 0.9 * serializeAvgUs + 0.1 * us;
  lastFrameBytes = json.length;
  emit(json);
}

function publishStatus() {
  const s = sim!;
  emit(
    JSON.stringify({
      v: 1,
      type: "status",
      state: s.state(),
      speed: s.speed(),
      sim_time: s.time(),
      step: s.step(),
      scenario: s.scenarioId(),
      scenario_name: s.scenarioName(),
      seed: s.seed(),
      controller: s.controller(),
      lagging,
      queue_depth: 0,
      queue_capacity: 0,
      frames_published: seq,
      last_frame_bytes: lastFrameBytes,
      avg_serialize_us: Math.round(serializeAvgUs),
      avg_step_us: Math.round(stepAvgUs * 10) / 10,
      realtime_factor: s.state() === "running" ? Math.round(realtimeFactor * 100) / 100 : 0,
    }),
  );
}

function fullState() {
  emit(JSON.stringify({ v: 1, type: "hello", server: "smart-traffic-simulator (in-browser WebAssembly)", protocol: 1 }));
  emit(sim!.network());
  publishStatus();
  publishSnapshot();
}

function tick() {
  if (!sim) return;
  const now = performance.now();
  const running = sim.state() === "running";
  if (running) {
    const dt = sim.dt();
    const speed = sim.speed();
    accumulator += ((now - lastTick) / 1000) * speed;
    let steps = Math.floor(accumulator / dt);
    // Same rule as the native coordinator: never catch up more than 250 ms at once.
    const cap = Math.max(1, Math.ceil((0.25 * speed) / dt));
    lagging = steps > cap;
    if (lagging) {
      steps = cap;
      accumulator = 0;
    } else {
      accumulator -= steps * dt;
    }
    if (steps > 0) {
      const t0 = performance.now();
      sim.advance(steps);
      const us = ((performance.now() - t0) * 1000) / steps;
      stepAvgUs = stepAvgUs === 0 ? us : 0.9 * stepAvgUs + 0.1 * us;
      rateWindowSim += steps * dt;
    }
    if (now - lastPublish >= PUBLISH_MS) {
      publishSnapshot();
      lastPublish = now;
    }
  }
  if (now - rateWindowStart >= 1000) {
    realtimeFactor = (rateWindowSim * 1000) / (now - rateWindowStart);
    rateWindowSim = 0;
    rateWindowStart = now;
  }
  if (now - lastStatus >= STATUS_MS) {
    publishStatus();
    lastStatus = now;
  }
  lastTick = now;
  setTimeout(tick, running ? TICK_MS : 100);
}

ctx.onmessage = async (e) => {
  const msg = e.data;
  if (msg.type === "init") {
    try {
      const engine = await createTrafficEngine();
      scenarios = msg.scenarios;
      sim = new engine.LiveSim();
      const errors = JSON.parse(sim.load(scenarios[msg.initial] ?? Object.values(scenarios)[0])) as string[];
      if (errors.length) throw new Error(errors[0]);
      send({ type: "ready" });
      fullState();
      lastTick = rateWindowStart = performance.now();
      tick();
    } catch (err) {
      send({ type: "fatal", error: String((err as Error)?.message ?? err) });
    }
    return;
  }
  if (!sim) return;
  if (msg.type === "resync") return fullState();
  if (msg.type === "metrics") return send({ type: "metrics", id: msg.id, json: sim.metrics() });
  if (msg.type === "command") {
    const wasRunning = sim.state() === "running";
    let scenario = "";
    if (msg.cmd.cmd === "load_scenario") {
      scenario = scenarios[String(msg.cmd.scenario)] ?? "";
      if (!scenario) {
        send({ type: "reply", id: msg.id, result: { ok: false, code: "not_found", error: `unknown scenario '${msg.cmd.scenario}'` } });
        return;
      }
    }
    const result = JSON.parse(sim.command(JSON.stringify(msg.cmd), scenario));
    if (result.network_changed) emit(sim.network());
    if (result.network_changed || (!wasRunning && sim.state() === "running")) {
      accumulator = 0;
      lastTick = performance.now();
    }
    // Publish first, then acknowledge (same ordering guarantee as the server).
    publishSnapshot();
    publishStatus();
    send({ type: "reply", id: msg.id, result: { ok: result.ok, error: result.error, code: result.code } });
  }
};
