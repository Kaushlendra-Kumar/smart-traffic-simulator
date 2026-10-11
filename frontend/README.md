# Smart Traffic Lab — frontend

React + TypeScript + Vite. Renders the simulation on an HTML canvas and talks to the
C++ server over REST (`/api/v1`) and WebSocket (`/ws`). It never computes vehicle
physics or signal state; it only draws authoritative snapshots, interpolating
positions between the two most recent frames for smooth motion.

```bash
npm install
npm run dev        # http://localhost:5173, proxies /api and /ws to localhost:8080
npm test           # unit tests (vitest)
npm run build      # production bundle in dist/, served by traffic_server
```

Set `TRAFFIC_BACKEND=http://host:port` to point the dev server at another backend.

| Path | Purpose |
| --- | --- |
| `src/api/socket.ts` | WebSocket client: reconnect with backoff, command acks by id, resync |
| `src/state/store.ts` | External store; frames bypass React, panels re-render at ≤ 4 Hz |
| `src/state/interpolate.ts` | Render-only interpolation between snapshots |
| `src/canvas/renderer.ts` | Layered canvas renderer (roads, markings, signals, queues, incidents, vehicles, pedestrians, labels) |
| `src/components/` | Top bar, live statistics, control panels, experiments dashboard, charts |
