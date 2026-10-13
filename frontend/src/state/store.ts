// Client state. Frames arrive at up to ~60 Hz; the canvas reads them directly every
// animation frame, while React components are notified at a throttled rate so panels
// don't re-render for every moving vehicle.

import { useSyncExternalStore } from "react";
import type { CommandResult, ConnectionState, Transport } from "../api/socket";
import { createTransport } from "../api/transport";
import type { MetricsMsg, NetworkMsg, ServerMsg, SnapshotMsg, StatusMsg } from "../types/protocol";

export interface Frame {
  snap: SnapshotMsg;
  receivedAt: number;
}

export interface Toast {
  id: number;
  kind: "ok" | "error";
  text: string;
}

class SimStore {
  network: NetworkMsg | null = null;
  prev: Frame | null = null;
  curr: Frame | null = null;
  status: StatusMsg | null = null;
  metrics: MetricsMsg | null = null;
  connection: ConnectionState = "connecting";
  toasts: Toast[] = [];
  /** Server time between frames, used to size the interpolation window. */
  frameInterval = 50;
  socket: Transport;

  private version = 0;
  private listeners = new Set<() => void>();
  private pendingNotify = false;
  private toastId = 1;

  constructor() {
    this.socket = createTransport((m) => this.handle(m), (s) => {
      this.connection = s;
      this.notify(true);
    });
  }

  subscribe = (fn: () => void) => {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  };
  getVersion = () => this.version;

  private handle(msg: ServerMsg) {
    switch (msg.type) {
      case "network":
        this.network = msg;
        this.prev = null;
        this.curr = null;
        this.notify(true);
        break;
      case "snapshot": {
        const now = performance.now();
        if (this.curr) {
          if (msg.step < this.curr.snap.step) this.prev = null;  // reset/seek: don't blend backwards
          else this.prev = this.curr;
          const dt = now - this.curr.receivedAt;
          if (dt > 5 && dt < 1000) this.frameInterval = this.frameInterval * 0.9 + dt * 0.1;
        }
        this.curr = { snap: msg, receivedAt: now };
        this.notify(false);
        break;
      }
      case "status":
        this.status = msg;
        this.notify(true);
        break;
      case "error":
        this.toast("error", msg.error ?? "The server reported an error.");
        break;
      default:
        break;
    }
  }

  /** Throttled notifications for frames; immediate for structural changes. */
  notify(immediate: boolean) {
    if (immediate) {
      this.version++;
      this.listeners.forEach((l) => l());
      return;
    }
    if (this.pendingNotify) return;
    this.pendingNotify = true;
    window.setTimeout(() => {
      this.pendingNotify = false;
      this.version++;
      this.listeners.forEach((l) => l());
    }, 250);
  }

  async command(cmd: Record<string, unknown>, success?: string): Promise<CommandResult> {
    const r = await this.socket.send(cmd);
    if (!r.ok) this.toast("error", r.error ?? "Command rejected.");
    else if (success) this.toast("ok", success);
    return r;
  }

  toast(kind: Toast["kind"], text: string) {
    const t = { id: this.toastId++, kind, text };
    this.toasts = [...this.toasts.slice(-3), t];
    this.notify(true);
    window.setTimeout(() => {
      this.toasts = this.toasts.filter((x) => x.id !== t.id);
      this.notify(true);
    }, kind === "error" ? 6000 : 2500);
  }
}

export const store = new SimStore();

/** Re-renders the calling component when the store changes (throttled for frames). */
export function useSim(): SimStore {
  useSyncExternalStore(store.subscribe, store.getVersion);
  return store;
}
