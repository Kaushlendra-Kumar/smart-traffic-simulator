# System design

## Requirements

### Functional

- Simulate signalised road networks (single junctions to grids) with cars, buses,
  emergency vehicles and pedestrians; lanes, turns, queues, closures and lane blockages.
- Interactive control: start, pause, resume, reset, single-step, speed, seek, scenario
  load; demand, signal plans, controller choice and parameters, incidents.
- Fixed-time, adaptive and max-pressure controllers behind one interface.
- Repeatable experiments across controllers and seeds with statistics, explanations and
  CSV export.
- Live visualisation in a browser; REST for management; WebSocket for live state.

### Non-functional

| Property | Target and how it is met |
| --- | --- |
| Determinism | Same scenario + seed + command log ⇒ identical state hash. Fixed dt, integer step clock, seeded RNG streams, ordered containers, single writer (see [simulation-model.md](simulation-model.md#determinism)) |
| Safety invariants | No conflicting greens, no red-light running, no overlaps; enforced in code and asserted by property tests across all scenarios and controllers |
| Responsiveness | Commands applied within one coordinator wake-up (≤ 50 ms at 20 Hz publishing); slow clients don't affect others |
| Bounded memory | Bounded command queue, one pending frame per client, bounded control queue, capped kernel send buffers, bounded experiment retention |
| Testability | Engine has no I/O; 124 C++ tests (unit, integration, property, end-to-end server) plus frontend unit tests |
| Observability | Structured `key=value` logs (spdlog); `/api/v1/telemetry` (step cost, real-time factor, frame size, serialisation time, broadcast time, per-client lag and coalescing, queue depth) |

## API contracts

See [api-specification.md](api-specification.md). Summary:

- REST under `/api/v1`: health, scenarios (CRUD with validation), simulation control,
  generic commands, status/snapshot/network/replay, metrics, telemetry, experiments
  (create, poll, results JSON/CSV, cancel).
- WebSocket at `/ws`, protocol version 1: server sends `hello`, `network`, `status`,
  `snapshot` (with `seq`), `ack`/`error`; client sends `command` (with `id`), `resync`,
  `ping`.

## Threading model

| Thread | Count | Does | Never does |
| --- | --- | --- | --- |
| Simulation | 1 | Drains commands, steps the engine, builds and serialises snapshots, publishes artefacts | Socket I/O, blocking waits other than its timed condition-variable wait |
| I/O (Asio) | 1 | Accepts connections, parses HTTP/WS, routes REST, writes frames | Touches engine objects |
| Experiment worker | 1 + pool | Runs queued experiments; each run owns a private engine | Touches the live engine |
| Main | 1 | Startup, signal handling, ordered shutdown | — |

Details and the synchronisation of each hand-off are in [concurrency.md](concurrency.md).

## Snapshot delivery and backpressure

1. The simulation thread publishes at a configurable rate (`publish_hz`, default 20),
   independent of the 10 Hz simulation step and of the speed multiplier.
2. Each frame is serialised **once** and shared by all clients as
   `shared_ptr<const std::string>`.
3. Per WebSocket client:
   - telemetry is **latest-wins**: one frame in flight, at most one pending; a newer frame
     replaces the pending one and increments `frames_coalesced`;
   - control messages (acks, errors, network) use a FIFO capped at
     `ws_control_queue_limit` (256). Overflow disconnects the client with close code 1008 —
     commands' results are never silently dropped;
   - the socket's kernel send buffer is capped (128 KB) so stale frames don't pile up in
     the kernel, where user-space coalescing couldn't replace them.
4. Commands from clients go through the bounded command queue; when full the client gets
   an immediate `busy` error (HTTP 503) rather than a dropped command.

Measured (test `SlowClientIsCoalescedNotBuffered`): a client that stops reading for 2.5 s
while the grid runs at 50× has dozens of frames coalesced, a control queue far below its
limit, and on resuming receives the current state rather than a backlog.

## Failure handling

| Failure | Behaviour |
| --- | --- |
| Malformed JSON, unknown command, out-of-range values | 400 / WS `error` with a specific message; nothing applied |
| Command invalid in the current state (step while running, unknown road, last open lane) | 409 / WS `error` with the engine's reason |
| Unknown scenario/experiment | 404 |
| Invalid scenario upload | 422 with every validation error and its JSON path |
| Oversized request / message | 413 for HTTP bodies over 1 MB; WS close 1009 over 64 KB |
| Protocol violations (unmasked client frame, reserved bits, bad control frame) | WS close 1002 |
| Unsupported protocol version | WS `error` `unsupported_version` |
| Client disconnects | Session removed; pending acks dropped with it |
| Engine can't keep up with the requested speed | Runs as fast as it can, never more than 250 ms of catch-up per wake-up; `lagging` flag set; physics unaffected |
| Scenario file broken at startup | Skipped with a warning; server starts with the others |
| Closure makes a destination unreachable | Vehicles divert to the cheapest reachable exit; new trips without a route are counted as `unserved` |

## Performance considerations

- Vehicles live in a contiguous slot array; lanes and connectors hold slot indices ordered
  front-first, so leader lookup is O(1) and per-step work is O(vehicles + roads).
- Accelerations are computed in a read-only pass and integrated afterwards (order
  independent).
- Routes for waiting trips are cached; signal observations use per-step aggregates.
- Snapshots carry only render data (positions rounded to 1 cm); static geometry is sent
  once per scenario.

Measurements and the optimisation history are in [performance.md](performance.md).

## Scaling strategy and trade-offs

- **Experiments scale out trivially**: each run is an isolated engine with no shared
  state, so the runner uses a worker pool and results are bit-identical regardless of
  thread count (tested).
- **One live simulation is harder to parallelise**: vehicles interact through shared
  lanes, merges and junctions within a single step. Parallel updates would need spatial
  partitioning with halo exchange or a two-phase update with conflict resolution at
  partition boundaries, and determinism would require a fixed reduction order. The
  measured single-thread throughput (~5 M vehicle-updates/s) makes this unnecessary for
  networks of up to ~10k vehicles.
- **Next steps if needed**, in order of expected payoff: structure-of-arrays for the hot
  vehicle fields; batching junction checks per intersection; partitioning the network by
  intersection clusters with one thread each; multiple independent simulation workers per
  process for multi-user deployments.
- No claim of linear scaling is made; none has been measured.
