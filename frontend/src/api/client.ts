import { apiUrl, engineMode } from "./backend";
import type { ExperimentResult, ExperimentStatus, MetricsMsg, ScenarioSummary } from "../types/protocol";

async function request<T>(method: string, path: string, body?: unknown): Promise<T> {
  const res = await fetch(apiUrl(path), {
    method,
    headers: body ? { "Content-Type": "application/json" } : undefined,
    body: body ? JSON.stringify(body) : undefined,
  });
  const text = await res.text();
  const data = text ? JSON.parse(text) : null;
  if (!res.ok) {
    const message = data?.error?.message ?? data?.error ?? `Request failed (${res.status})`;
    throw new Error(typeof message === "string" ? message : JSON.stringify(message));
  }
  return data as T;
}

type LocalApi = typeof import("../engine/local").localApi;
let local: LocalApi | null = null;
const loadLocal = (): Promise<LocalApi> =>
  import("../engine/local").then((m) => {
    local = m.localApi;
    return local;
  });
const wasm = engineMode === "wasm";

export const api = {
  scenarios: () => (wasm ? loadLocal().then((l) => l.scenarios()) : request<ScenarioSummary[]>("GET", "/api/v1/scenarios")),
  metrics: () => (wasm ? loadLocal().then((l) => l.metrics()) : request<MetricsMsg>("GET", "/api/v1/metrics")),
  startExperiment: (body: unknown) =>
    wasm ? loadLocal().then((l) => l.startExperiment(body)) : request<{ id: string }>("POST", "/api/v1/experiments", body),
  experiment: (id: string) =>
    wasm ? loadLocal().then((l) => l.experiment(id)) : request<ExperimentStatus>("GET", `/api/v1/experiments/${id}`),
  experimentResults: (id: string) =>
    wasm
      ? loadLocal().then((l) => l.experimentResults(id))
      : request<ExperimentResult>("GET", `/api/v1/experiments/${id}/results`),
  cancelExperiment: (id: string) =>
    wasm ? loadLocal().then((l) => l.cancelExperiment(id)) : request<unknown>("DELETE", `/api/v1/experiments/${id}`),
  csvUrl: (id: string) => (wasm ? (local?.csvUrl(id) ?? "#") : apiUrl(`/api/v1/experiments/${id}/results.csv`)),
};
