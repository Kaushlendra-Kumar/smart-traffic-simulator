// Runs paired multi-seed experiments with the WebAssembly engine, one run at a time,
// reporting progress between runs. Separate from the live worker so the live view
// keeps running while an experiment computes.

import createTrafficEngine, { type TrafficEngineModule } from "../wasm/traffic-engine.js";
import type { FromExperimentWorker, ToExperimentWorker } from "./protocol";

const ctx = self as unknown as {
  postMessage(message: FromExperimentWorker): void;
  onmessage: ((e: MessageEvent<ToExperimentWorker>) => void) | null;
};

let engine: Promise<TrafficEngineModule> | null = null;
const cancelled = new Set<string>();
const yieldToEvents = () => new Promise((r) => setTimeout(r, 0));

ctx.onmessage = async (e) => {
  const msg = e.data;
  if (msg.type === "cancel") {
    cancelled.add(msg.id);
    return;
  }
  engine ??= createTrafficEngine();
  const mod = await engine;
  const job = new mod.ExperimentJob();
  try {
    const errors = JSON.parse(job.start(msg.request, msg.scenario, msg.id)) as string[];
    if (errors.length) return ctx.postMessage({ type: "failed", id: msg.id, error: errors.join("; ") });
    const total = job.total();
    const t0 = performance.now();
    ctx.postMessage({ type: "progress", id: msg.id, done: 0, total });
    while (!job.finished()) {
      job.runNext();
      ctx.postMessage({ type: "progress", id: msg.id, done: job.done(), total });
      await yieldToEvents(); // lets a cancel message arrive between runs
      if (cancelled.has(msg.id)) return ctx.postMessage({ type: "cancelled", id: msg.id });
    }
    ctx.postMessage({
      type: "done",
      id: msg.id,
      json: job.resultJson(),
      csv: job.resultCsv(),
      wallSeconds: (performance.now() - t0) / 1000,
    });
  } catch (err) {
    ctx.postMessage({ type: "failed", id: msg.id, error: String((err as Error)?.message ?? err) });
  } finally {
    job.delete();
  }
};
