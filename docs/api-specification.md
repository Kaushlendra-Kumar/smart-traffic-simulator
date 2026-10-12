# API specification (v1)

Base URL: the server root (default `http://localhost:8080`). All bodies are JSON
(`Content-Type: application/json`) unless noted. Request bodies must use
`Content-Length` (chunked uploads get 411) and be at most 1 MB (413).

## Errors

```json
{ "error": { "code": "validation_failed", "message": "first problem", "details": ["...", "..."] } }
```

| Status | Meaning |
| --- | --- |
| 400 | Malformed JSON or invalid command (`invalid_command`) |
| 404 | Unknown route, scenario or experiment |
| 405 | Method not allowed on that route |
| 409 | Valid request rejected in the current state (`rejected`), or experiment results not ready |
| 411 / 413 / 431 | Chunked body / body too large / headers too large |
| 422 | Scenario or experiment specification failed validation (all errors listed) |
| 503 | Command queue full (`busy`) or too many experiments queued — retry |

Command endpoints answer `{"ok": true, "status": {...}}` on success, where `status` is the
coordinator status **after** the command took effect, or
`{"ok": false, "code": "...", "error": "..."}`.

## Endpoints

| Method | Path | Body | Result |
| --- | --- | --- | --- |
| GET | `/api/v1/health` | — | `{status, version, protocol, uptime_s, simulation, scenario}` |
| GET | `/api/v1/scenarios` | — | `[{id, name, description, grid}]` |
| POST | `/api/v1/scenarios` | scenario JSON | 201 + scenario; 409 if the id exists; 422 if invalid |
| GET | `/api/v1/scenarios/{id}` | — | scenario JSON |
| PUT | `/api/v1/scenarios/{id}` | scenario JSON (same id) | 200 + scenario |
| POST | `/api/v1/simulation/start` | — | start running (from ready) |
| POST | `/api/v1/simulation/pause` | — | pause |
| POST | `/api/v1/simulation/resume` | — | resume |
| POST | `/api/v1/simulation/reset` | `{seed?}` | rebuild from the scenario; keeps controller, demand and signal-plan settings, clears incidents |
| POST | `/api/v1/simulation/step` | `{count?: 1..100000}` | advance N steps while paused (409 while running) |
| POST | `/api/v1/simulation/speed` | `{speed: 0.1..50}` | simulated seconds per wall second |
| POST | `/api/v1/simulation/seek` | `{time}` | deterministic replay to `time` (seconds), then pause |
| POST | `/api/v1/simulation/load` | `{scenario, seed?}` | load a scenario from the library |
| POST | `/api/v1/simulation/commands` | any command (below) | apply it |
| GET | `/api/v1/simulation/status` | — | status message (see WebSocket) |
| GET | `/api/v1/simulation/snapshot` | — | latest snapshot message |
| GET | `/api/v1/simulation/network` | — | static network message |
| GET | `/api/v1/simulation/replay` | — | `{scenario, seed, dt, current_step, state_hash, commands:[{step, command}]}` |
| GET | `/api/v1/metrics` | — | `{summary, intersections, series}` |
| GET | `/api/v1/telemetry` | — | server and simulation performance counters, per-client stats |
| POST | `/api/v1/experiments` | see below | 202 `{id, state}` |
| GET | `/api/v1/experiments` | — | list of experiment statuses |
| GET | `/api/v1/experiments/{id}` | — | `{id, state, progress:{done,total}, ...}` |
| GET | `/api/v1/experiments/{id}/results` | — | full results (409 until done) |
| GET | `/api/v1/experiments/{id}/results.csv` | — | CSV, one row per run |
| DELETE | `/api/v1/experiments/{id}` | — | cancel |

Any other `GET` path is served from the built frontend (`frontend/dist`) with a
single-page-app fallback; path traversal outside that directory returns 404.

The proposed endpoints from the brief are implemented as listed; additions are `load`,
`speed`, `seek`, `commands`, `snapshot`, `network`, `replay`, `telemetry`, CSV export and
experiment cancellation. There is no `DELETE /scenarios` (scenario files are kept).

### Commands

Sent as `{"cmd": "...", ...}` to `/api/v1/simulation/commands` or inside a WebSocket
`command` message. Every field is validated; out-of-range values are rejected.

| `cmd` | Fields | Notes |
| --- | --- | --- |
| `start`, `pause`, `resume` | — | |
| `reset` | `seed?` | |
| `step` | `count?` | only while paused |
| `set_speed` | `speed` | |
| `seek` | `time` | |
| `load_scenario` | `scenario`, `seed?` | |
| `set_demand` | `multiplier` 0..5 | vehicles |
| `set_pedestrian_demand` | `multiplier` 0..10 | |
| `set_vehicle_type` | `type` bus\|emergency, `enabled` | disabled types become cars |
| `close_road`, `open_road` | `road` | entry/exit roads can't be closed |
| `block_lane` | `road`, `lane`, `position` | ≥ 40 m from the lane start; one lane must stay open |
| `clear_lane` | `road`, `lane` | |
| `set_controller` | `controller` (object, below) | |
| `set_signal_plan` | `node`, `greens[]` | one fixed-time green per phase, within [min, max] |
| `set_phase_timing` | `node`, `phase`, `min_green`, `max_green` | min must cover pedestrian walk + clearance |
| `set_speed_limit` | `road`, `kmh` 10..130 | |
| `spawn_vehicle` | `type`, `origin`, `destination` | boundary nodes with a route |

Controller object:

```json
{ "type": "adaptive", "w_queue": 1, "w_wait": 1, "w_oldest": 1.5, "w_arrivals": 0.5,
  "w_pedestrian": 1, "hysteresis_ratio": 0.25, "hysteresis_abs": 2, "gap_out_seconds": 3,
  "max_wait": 90, "max_pedestrian_wait": 90, "emergency_priority": true,
  "decision_interval": 5, "greens": [20, 20], "plans": [{"start": 900, "greens": [30, 15]}],
  "node_greens": {"4": [25, 15, 20]}, "label": "optional display name" }
```

### Experiments

```json
POST /api/v1/experiments
{ "scenario": "uneven-demand",
  "controllers": [{"type": "fixed_time"}, {"type": "adaptive", "hysteresis_ratio": 0.5}],
  "seed_count": 10, "base_seed": 1,      // or "seeds": [3, 5, 8]
  "duration": 900, "threads": 0 }        // 0 = all cores
```

The first controller is the baseline. Results contain `aggregates` (mean, ci95, n per
metric and controller), `comparisons` (paired differences per candidate), `explanations`,
`metric_definitions` (label, unit, direction), and every run's metrics, per-intersection
phase statistics, time series and final state hash. CSV columns carry units in their
names (e.g. `avg_delay_all_s`, `throughput_vph_veh_per_h`) and include the scenario id,
controller configuration (JSON) and seed.

## WebSocket protocol (v1)

Endpoint `/ws` (RFC 6455, `Sec-WebSocket-Version: 13`, text frames, JSON). Every message
has `"v": 1` and `"type"`. Client messages are limited to 64 KB (close 1009); unmasked
client frames close the connection (1002).

### Server → client

| type | When | Content |
| --- | --- | --- |
| `hello` | on connect | `{server, protocol}` |
| `network` | on connect, scenario load/reset, `resync` | static geometry: nodes (with crosswalks, arms, movement ids), roads (lane offsets, endpoints), movements (connector paths), signal phases, controller, bounds |
| `status` | about once per second and after commands | state, speed, sim time, step, scenario, seed, controller, lagging, queue depth, frame bytes, serialisation and step cost, real-time factor |
| `snapshot` | at `publish_hz` while running, after commands | `seq`, `step`, `t`, `veh` = `[[id, x, y, heading, speed, type, length]]`, `ped` = `[[id, x, y, crossing]]`, `sig` = `[{node, phase, next, stage, t, mv, cw, why}]`, `q` = `[[road, queued]]`, `closed`, `blk`, `m` (metrics), demand multipliers, controller |
| `ack` | after a command was applied | `{id, ok: true}` |
| `error` | command rejected or bad message | `{id, code, error}` — the connection stays open |
| `pong` | reply to `ping` | `{id}` |

`sig.mv` is one character per movement of the node in `network.nodes[i].movements` order
(`G`, `Y`, `R`); `sig.cw` one per crosswalk (`W` walk, `C` clearance, `D` don't walk).
Vehicle `type`: 0 car, 1 bus, 2 emergency. Positions are world metres rounded to 1 cm.

**Sequence numbers** increase strictly. Gaps are normal: a slow client's pending frame is
replaced by newer ones. A client should treat each snapshot as complete state, not a
delta.

**Ordering guarantee**: for a command, the `ack`/`error` is sent after the snapshot and
status reflecting it have been published to the client's queue, so state read after the
ack is never older than the command.

### Client → server

```json
{ "v": 1, "type": "command", "id": "c42", "cmd": "close_road", "road": 17 }
{ "v": 1, "type": "resync" }   // ask for network + status + latest snapshot again
{ "v": 1, "type": "ping", "id": "p1" }
```

### Reconnecting

A new connection always receives `hello`, `network`, the latest `status` and the latest
`snapshot`, so a reconnecting client needs no special handshake. Pending command
promises on the client are rejected when the socket closes; the client reconnects with
exponential backoff (0.4 s doubling to 8 s).
