// Main-thread side of the in-browser engine: a Transport that speaks to the live
// worker exactly like SimSocket speaks to the server, plus local implementations of the
// REST calls (scenarios, metrics, experiments) used by the UI.

import type { CommandResult, ConnectionState, Transport } from "../api/socket";
import type { ExperimentResult, ExperimentStatus, MetricsMsg, ScenarioSummary, ServerMsg } from "../types/protocol";
import type { FromExperimentWorker, FromLiveWorker, ToExperimentWorker, ToLiveWorker } from "./protocol";
import { bundledScenarios, defaultScenarioId, scenarioText } from "./scenarios";

let live: Worker | null = null;
let nextId = 1;
const pendingReplies = new Map<string, (r: CommandResult) => void>();
const pendingMetrics = new Map<string, (json: string) => void>();
let messageListener: ((m: ServerMsg) => void) | null = null;

function liveWorker(): Worker {
  if (live) return live;
  live = new Worker(new URL("./live.worker.ts", import.meta.url), { type: "module" });
  return live;
}
const postLive = (m: ToLiveWorker) => liveWorker().postMessage(m);

export class LocalEngineTransport implements Transport {
  state: ConnectionState = "connecting";
  lastSeq = 0;
  gaps = 0;

  constructor(
    private onMessage: (m: ServerMsg) => void,
    private onState: (s: ConnectionState) => void,
  ) {}

  connect() {
    const w = liveWorker();
    messageListener = this.onMessage;
    w.onmessage = (e: MessageEvent<FromLiveWorker>) => {
      const msg = e.data;
      switch (msg.type) {
        case "ready":
          this.setState("open");
          break;
        case "fatal":
          console.error("Simulation engine failed to start:", msg.error);
          this.setState("closed");
          break;
        case "message": {
          const m = JSON.parse(msg.json) as ServerMsg;
          if (m.type === "snapshot") {
            if (this.lastSeq && m.seq > this.lastSeq + 1) this.gaps += m.seq - this.lastSeq - 1;
            this.lastSeq = m.seq;
          }
          messageListener?.(m);
          break;
        }
        case "reply":
          pendingReplies.get(msg.id)?.(msg.result);
          pendingReplies.delete(msg.id);
          break;
        case "metrics":
          pendingMetrics.get(msg.id)?.(msg.json);
          pendingMetrics.delete(msg.id);
          break;
      }
    };
    w.onerror = (e) => {
      console.error("Simulation worker error:", e.message);
      this.setState("closed");
    };
    const scenarios = Object.fromEntries(bundledScenarios.map((s) => [s.id, s.text]));
    postLive({ type: "init", scenarios, initial: defaultScenarioId });
  }

  close() {
    live?.terminate();
    live = null;
    this.setState("closed");
  }

  send(cmd: Record<string, unknown>): Promise<CommandResult> {
    if (this.state !== "open") return Promise.resolve({ ok: false, error: "The simulation engine is still starting." });
    const id = `c${nextId++}`;
    return new Promise((resolve) => {
      pendingReplies.set(id, resolve);
      postLive({ type: "command", id, cmd });
    });
  }

  resync() {
    postLive({ type: "resync" });
  }

  private setState(s: ConnectionState) {
    this.state = s;
    this.onState(s);
  }
}

// ---------------------------------------------------------------- experiments

interface Job {
  status: ExperimentStatus;
  result?: ExperimentResult;
  csv?: string;
  csvUrl?: string;
  error?: string;
}

let expWorker: Worker | null = null;
const jobs = new Map<string, Job>();
let jobSeq = 1;

function experimentWorker(): Worker {
  if (expWorker) return expWorker;
  expWorker = new Worker(new URL("./experiment.worker.ts", import.meta.url), { type: "module" });
  expWorker.onmessage = (e: MessageEvent<FromExperimentWorker>) => {
    const msg = e.data;
    const job = jobs.get(msg.id);
    if (!job) return;
    if (msg.type === "progress") {
      job.status = { ...job.status, state: "running", progress: { done: msg.done, total: msg.total } };
    } else if (msg.type === "done") {
      job.result = JSON.parse(msg.json) as ExperimentResult;
      job.csv = msg.csv;
      job.status = { ...job.status, state: "done", wall_seconds: msg.wallSeconds };
    } else if (msg.type === "cancelled") {
      job.status = { ...job.status, state: "cancelled" };
    } else if (msg.type === "failed") {
      job.error = msg.error;
      job.status = { ...job.status, state: "failed" };
    }
  };
  return expWorker;
}

export const localApi = {
  async scenarios(): Promise<ScenarioSummary[]> {
    return bundledScenarios.map(({ id, name, description, grid }) => ({ id, name, description, grid }));
  },

  metrics(): Promise<MetricsMsg> {
    const id = `m${nextId++}`;
    return new Promise((resolve) => {
      pendingMetrics.set(id, (json) => resolve(JSON.parse(json) as MetricsMsg));
      postLive({ type: "metrics", id });
    });
  },

  async startExperiment(body: unknown): Promise<{ id: string }> {
    const req = body as { scenario?: string; controllers?: { type: string }[]; seed_count?: number; duration?: number };
    const text = req.scenario ? scenarioText(req.scenario) : undefined;
    if (!text) throw new Error("Unknown scenario.");
    const id = `exp-${jobSeq++}`;
    const seeds = req.seed_count ?? 5;
    jobs.set(id, {
      status: {
        id,
        state: "queued",
        scenario: req.scenario!,
        controllers: (req.controllers ?? []).map((c) => c.type),
        seeds,
        duration: req.duration ?? 0,
        progress: { done: 0, total: seeds * (req.controllers?.length ?? 0) },
        wall_seconds: 0,
      },
    });
    const msg: ToExperimentWorker = { type: "run", id, request: JSON.stringify(body), scenario: text };
    experimentWorker().postMessage(msg);
    return { id };
  },

  async experiment(id: string): Promise<ExperimentStatus> {
    const job = jobs.get(id);
    if (!job) throw new Error("Unknown experiment.");
    if (job.status.state === "failed") throw new Error(job.error ?? "The experiment failed.");
    return job.status;
  },

  async experimentResults(id: string): Promise<ExperimentResult> {
    const job = jobs.get(id);
    if (!job?.result) throw new Error("Results are not ready yet.");
    return job.result;
  },

  async cancelExperiment(id: string): Promise<unknown> {
    experimentWorker().postMessage({ type: "cancel", id } satisfies ToExperimentWorker);
    return { id };
  },

  csvUrl(id: string): string {
    const job = jobs.get(id);
    if (!job?.csv) return "#";
    job.csvUrl ??= URL.createObjectURL(new Blob([job.csv], { type: "text/csv" }));
    return job.csvUrl;
  },
};
