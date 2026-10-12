# ADR 0002: One authoritative simulation thread with a fixed timestep

**Status**: accepted

## Context

Vehicles interact through shared lanes, merges, junctions and crosswalks within every step.
Results must be deterministic for fair experiments and must not depend on rendering or
network speed.

## Decision

Exactly one thread mutates the live engine. It advances in fixed 0.1 s steps; an
accumulator converts wall time × speed into whole steps, catching up at most 250 ms per
wake-up. Accelerations are computed in a read-only pass before integration so the result
doesn't depend on update order.

## Consequences

- Determinism and simple reasoning: no locks inside the engine.
- Throughput is bounded by one core: measured ~5 M vehicle-updates/s, i.e. a
  10,000-vehicle grid at ~50× real time — ample for the lab.
- If the engine can't keep up at a requested speed it runs slower (flagged as `lagging`)
  instead of changing dt.
- Parallelism is applied where it is safe and free: independent experiment runs.
