// Where the C++ server lives.
//
// * Same origin (default): the server also serves this frontend, or the Vite dev
//   server proxies /api and /ws to it.
// * Separate origin: set VITE_BACKEND_URL at build time, e.g.
//   VITE_BACKEND_URL=https://smart-traffic-simulator.onrender.com
//   (used when the frontend is hosted on Vercel and the server on Render).

const configured = (import.meta.env.VITE_BACKEND_URL as string | undefined)?.trim().replace(/\/+$/, "") ?? "";

/** True when the frontend talks to a server on another host. */
export const remoteBackend = configured.length > 0;

/** Absolute or relative URL for a REST path such as "/api/v1/health". */
export function apiUrl(path: string): string {
  return `${configured}${path}`;
}

/** WebSocket URL of the live simulation stream. */
export function socketUrl(): string {
  if (remoteBackend) return `${configured.replace(/^http/, "ws")}/ws`;
  const proto = window.location.protocol === "https:" ? "wss" : "ws";
  return `${proto}://${window.location.host}/ws`;
}
