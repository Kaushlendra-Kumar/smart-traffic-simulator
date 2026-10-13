// Messages between the main thread and the engine workers (browser build only).

export type ToLiveWorker =
  | { type: "init"; scenarios: Record<string, string>; initial: string }
  | { type: "command"; id: string; cmd: Record<string, unknown> }
  | { type: "resync" }
  | { type: "metrics"; id: string };

export type FromLiveWorker =
  | { type: "ready" }
  | { type: "fatal"; error: string }
  | { type: "message"; json: string } // a protocol-v1 server message, verbatim
  | { type: "reply"; id: string; result: { ok: boolean; error?: string; code?: string } }
  | { type: "metrics"; id: string; json: string };

export type ToExperimentWorker = { type: "run"; id: string; request: string; scenario: string } | { type: "cancel"; id: string };

export type FromExperimentWorker =
  | { type: "progress"; id: string; done: number; total: number }
  | { type: "done"; id: string; json: string; csv: string; wallSeconds: number }
  | { type: "cancelled"; id: string }
  | { type: "failed"; id: string; error: string };
