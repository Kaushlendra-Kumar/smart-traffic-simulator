import { engineMode, socketUrl } from "./backend";
import { SimSocket, type CommandResult, type ConnectionState, type Transport } from "./socket";
import type { ServerMsg } from "../types/protocol";

/**
 * Picks the transport for this build. The in-browser engine is loaded on demand so the
 * server build never downloads the WebAssembly module.
 */
export function createTransport(onMessage: (m: ServerMsg) => void, onState: (s: ConnectionState) => void): Transport {
  if (engineMode === "server") return new SimSocket(socketUrl(), onMessage, onState);
  return new DeferredLocalTransport(onMessage, onState);
}

class DeferredLocalTransport implements Transport {
  state: ConnectionState = "connecting";
  private inner: Transport | null = null;
  private loading: Promise<Transport> | null = null;

  constructor(
    private onMessage: (m: ServerMsg) => void,
    private onState: (s: ConnectionState) => void,
  ) {}

  get lastSeq() {
    return this.inner?.lastSeq ?? 0;
  }
  get gaps() {
    return this.inner?.gaps ?? 0;
  }

  private load(): Promise<Transport> {
    this.loading ??= import("../engine/local").then(({ LocalEngineTransport }) => {
      this.inner = new LocalEngineTransport(this.onMessage, (s) => {
        this.state = s;
        this.onState(s);
      });
      return this.inner;
    });
    return this.loading;
  }

  connect() {
    this.onState("connecting");
    this.load()
      .then((t) => t.connect())
      .catch(() => {
        this.state = "closed";
        this.onState("closed");
      });
  }
  close() {
    this.inner?.close();
  }
  async send(cmd: Record<string, unknown>): Promise<CommandResult> {
    return (await this.load()).send(cmd);
  }
  resync() {
    this.inner?.resync();
  }
}
