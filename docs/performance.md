# Performance

All numbers below were **measured**, not targeted in advance.

## Environment

| Item | Value |
| --- | --- |
| Machine | Cloud VM, 2 vCPU Intel Xeon @ 2.10 GHz, 7 GB RAM, shared host (expect ±15% noise) |
| OS | Ubuntu 24.04, Linux 6.18 |
| Compiler | GCC 13.3.0, `-O3` (CMake `Release`), no fast-math |
| Benchmark | Google Benchmark 1.8, `benchmarks/bench_engine.cpp` |
| Scenario | Generated square grid (block 250 m, 2 lanes, 50 km/h), adaptive control, pedestrians 60/h per intersection, seed 12345, dt 0.1 s |

## Method

`BM_EngineStep/N` builds a grid sized for N vehicles (3×3, 6×6, 10×10, 14×14), fills it
with real demand until N vehicles are in the network, then stops new arrivals and times 600
consecutive steps. The reported `vehicles` counter is the **mean population during the timed
steps** (it drains slowly as trips complete). Earlier versions of the harness kept demand
on and let the population drift far above N; that was fixed before the measurements below.

```bash
cmake --preset release && cmake --build --preset release
./build/release/benchmarks/traffic_benchmarks --benchmark_counters_tabular=true
```

## Results

### Simulation step

| Target | Mean vehicles | Intersections | Time per step | Steps/s | Simulated s per wall s | Vehicle updates/s |
| --- | --- | --- | --- | --- | --- | --- |
| 100 | 105 | 9 | 24 µs | 41,000 | 4,100× | 4.3 M |
| 1,000 | 1,008 | 36 | 135 µs | 7,400 | 740× | 7.5 M |
| 5,000 | 5,171 | 100 | 737 µs | 1,370 | 137× | 7.1 M |
| 10,000 | 10,142 | 196 | 2.0 ms | 500 | 50× | 5.1 M |

A single simulation thread runs a 10,000-vehicle, 196-intersection network at about 50×
real time. Step latency is stable (Google Benchmark reports CPU ≈ wall time).

### Snapshot build + JSON serialisation

| Vehicles | Time | Frame size |
| --- | --- | --- |
| ~100 | 100 µs | 6 KB |
| ~1,000 | 0.65 ms | 45 KB |
| ~5,300 | 3.5 ms | 230 KB |
| ~10,300 | 7.8 ms | 450 KB |

At the default 20 Hz publish rate, 10k vehicles cost ~16% of a core for serialisation and
~9 MB/s per client; large networks should lower `publish_hz` (or a binary encoding would be
the next step). The default single-intersection scenario produces ~1.7 KB frames in
~80 µs (from `/api/v1/telemetry`).

### Routing (Dijkstra over roads)

| Network | Roads | Time per query |
| --- | --- | --- |
| 3×3 grid | 48 | 0.7 µs |
| 14×14 grid | 840 | 51 µs |

### Experiments

Experiments run on a worker pool of independent engines. On the 2-vCPU machine a 10-seed,
3-controller comparison takes 0.4–1.1 s for single-intersection scenarios (30 runs of 900–1800
simulated seconds each) and 4.4 s for the 3×3 grid with 1,500 s runs.

## Profiling

```bash
valgrind --tool=callgrind --callgrind-out-file=cg.out \
  ./build/release/traffic_cli run scenario.json --duration 150
callgrind_annotate cg.out | head -40
```

(`perf record -g ./build/release/traffic_cli run ...` works equally where perf is
available.)

### Bottlenecks found and fixed

Profile of a congested 10×10 grid (1,943 vehicles in the network), before optimisation:

| Function | Share of instructions | Cause |
| --- | --- | --- |
| `shortest_route` | 23.0% | Trips waiting to enter a congested source recomputed their route **every step** |
| `SimulationEngine::update_signals` | 20.7% | For every movement of every phase it re-counted vehicles on the outgoing road and re-scanned the node's movements to count feeders; oldest-pedestrian wait scanned all pedestrians per crosswalk |
| `compute_accelerations` | 6.9% | (expected core work) |
| `integrate_and_transition` | 6.0% | (expected core work) |
| `Polyline::intersects` | 5.3% | One-off conflict-model construction at load (not per step) |

Changes:

1. Routes are planned once when a trip is requested and cached in the backlog; they are
   recomputed only if a road on them has been closed.
2. `update_signals` computes per-step aggregates once (vehicles per road, oldest waiting
   pedestrian per crosswalk) and uses a static table of how many movements discharge into
   each road.

Before and after (same machine, same populations):

| Mean vehicles | Before | After | Speed-up |
| --- | --- | --- | --- |
| ~105 | 19.6 µs | 24.4 µs | none (within noise on this shared host) |
| ~1,008 | 162 µs | 135 µs | 1.2× |
| ~5,171 | 1,583 µs | 737 µs | 2.1× |
| ~10,142 | 4,448 µs | 2,000 µs | 2.2× |

All 124 tests, including determinism and invariant tests, pass after the change.

## What has not been optimised (and why)

- Vehicles are an array of structs. Structure-of-arrays would help vectorise the IDM pass,
  but the profile shows the remaining time spread across junction logic and list
  maintenance, not the arithmetic.
- Lane lists are `std::vector` with erase-at-front on lane exits; lanes are short enough
  (< 60 vehicles) that this hasn't shown up in profiles.
- No parallelism inside a step — see [system-design.md](system-design.md#scaling-strategy-and-trade-offs).
