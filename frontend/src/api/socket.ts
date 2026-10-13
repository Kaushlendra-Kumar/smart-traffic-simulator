// WebSocket client: reconnects with backoff, tracks command acknowledgements by id,
// and asks for a full resync after reconnecting. Snapshot sequence gaps are expected
// (the server coalesces frames for slow clients) and are only counted.

import { PROTOCOL_VERSION, type AckMsg, type ServerMsg } from "../types/protocol";

export type ConnectionState = "connecting" | "open" | "closed";

export interface CommandResult {
  ok: boolean;
  error?: string;
  code?: string;
}

type Listener = (msg: ServerMsg) => void;

/** What the store needs from a connection to a simulation (server or in-browser engine). */
export interface Transport {
  state: ConnectionState;
  lastSeq: number;
  gaps: number;
  connect(): void;
  close(): void;
  send(cmd: Record<string, unknown>): Promise<CommandResult>;
  resync(): void;
}

export class SimSocket implements Transport {
  private ws: WebSocket | null = null;
  private nextId = 1;
  private pending = new Map<string, (r: CommandResult) => void>();
  private retry = 0;
  private closedByUser = false;
  state: ConnectionState = "connecting";
  lastSeq = 0;
  gaps = 0;

  constructor(
    private url: string,
    private onMessage: Listener,
    private onState: (s: ConnectionState) => void,
  ) {}

  connect() {
    this.closedByUser = false;
    this.setState("connecting");
    const ws = new WebSocket(this.url);
    this.ws = ws;
    ws.onopen = () => {
      this.retry = 0;
      this.setState("open");
    };
    ws.onmessage = (ev) => {
      let msg: ServerMsg;
      try {
        msg = JSON.parse(ev.data as string);
      } catch {
        return;
      }
      if (msg.type === "snapshot") {
        if (this.lastSeq && msg.seq > this.lastSeq + 1) this.gaps += msg.seq - this.lastSeq - 1;
        this.lastSeq = msg.seq;
      }
      if ((msg.type === "ack" || msg.type === "error") && (msg as AckMsg).id) {
        const a = msg as AckMsg;
        const resolve = this.pending.get(String(a.id));
        if (resolve) {
          this.pending.delete(String(a.id));
          resolve({ ok: a.type === "ack", error: a.error, code: a.code });
          return;
        }
      }
      this.onMessage(msg);
    };
    ws.onclose = () => {
      this.ws = null;
      for (const resolve of this.pending.values()) resolve({ ok: false, error: "Connection lost before the server answered." });
      this.pending.clear();
      this.setState("closed");
      if (!this.closedByUser) {
        const delay = Math.min(8000, 400 * 2 ** this.retry++);
        window.setTimeout(() => this.connect(), delay);
      }
    };
  }

  close() {
    this.closedByUser = true;
    this.ws?.close();
  }

  /** Sends a command and resolves with the server's ack/error. */
  send(cmd: Record<string, unknown>): Promise<CommandResult> {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN)
      return Promise.resolve({ ok: false, error: "Not connected to the simulation server." });
    const id = `c${this.nextId++}`;
    return new Promise((resolve) => {
      this.pending.set(id, resolve);
      this.ws!.send(JSON.stringify({ v: PROTOCOL_VERSION, type: "command", id, ...cmd }));
      window.setTimeout(() => {
        if (this.pending.delete(id)) resolve({ ok: false, error: "The server did not answer within 10 s." });
      }, 10000);
    });
  }

  resync() {
    this.ws?.send(JSON.stringify({ v: PROTOCOL_VERSION, type: "resync" }));
  }

  private setState(s: ConnectionState) {
    this.state = s;
    this.onState(s);
  }
}

export { socketUrl as defaultSocketUrl } from "./backend";
