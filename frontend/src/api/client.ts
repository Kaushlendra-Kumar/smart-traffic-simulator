import type { ExperimentResult, ExperimentStatus, MetricsMsg, ScenarioSummary } from "../types/protocol";

async function request<T>(method: string, path: string, body?: unknown): Promise<T> {
  const res = await fetch(path, {
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

export const api = {
  scenarios: () => request<ScenarioSummary[]>("GET", "/api/v1/scenarios"),
  metrics: () => request<MetricsMsg>("GET", "/api/v1/metrics"),
  startExperiment: (body: unknown) => request<{ id: string }>("POST", "/api/v1/experiments", body),
  experiment: (id: string) => request<ExperimentStatus>("GET", `/api/v1/experiments/${id}`),
  experimentResults: (id: string) => request<ExperimentResult>("GET", `/api/v1/experiments/${id}/results`),
  cancelExperiment: (id: string) => request<unknown>("DELETE", `/api/v1/experiments/${id}`),
  csvUrl: (id: string) => `/api/v1/experiments/${id}/results.csv`,
};
