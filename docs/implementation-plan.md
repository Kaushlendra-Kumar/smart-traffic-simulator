# Implementation plan

Dependency order: engine core → controllers/metrics/experiments → JSON infrastructure and
coordinator → server → frontend → hardening. Each milestone ends with a clean build and a
passing test suite.

| # | Milestone | Acceptance criteria | Status |
| --- | --- | --- | --- |
| 0 | Repository, CMake (find_package + FetchContent fallback), presets, architecture | Configures and builds on GCC/Clang; documented boundaries | Done |
| 1 | Road graph, routing, vehicles, spawner, fixed-step loop, minimal signals | Headless run of a junction and a grid; routing and validation tests | Done |
| 2 | IDM following, junction entry rules, lane changes, pedestrians, scenario JSON, seeded RNG | Invariant tests (no overlaps, no red-light running, signals conflict-free) across scenarios | Done |
| 3 | Controller interface; fixed-time, adaptive, max-pressure; metrics; paired multi-seed experiments; CLI | Deterministic, thread-count-independent experiments with CIs and explanations | Done |
| 4 | REST + WebSocket server, validated commands, bounded queues, immutable snapshots | End-to-end server tests incl. reconnect and slow client | Done |
| 5 | React/TypeScript/Canvas UI with controls and live charts | Builds; renders live state; no runtime errors in a headless browser | Done |
| 6 | Experiment dashboard, CSV export, explanations, congestion overlay | Results with CIs and paired differences in the browser and as CSV | Done |
| 7 | Sanitizers, benchmarks, profiling, docs, Docker, CI | ASan/UBSan/TSan clean; measured benchmarks; docs match code | Done (Docker image not buildable in the dev sandbox — registry blocked) |
