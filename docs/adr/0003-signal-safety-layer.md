# ADR 0003: Controllers propose, the signal state machine enforces safety

**Status**: accepted

## Context

Several control algorithms (and future learned policies) must be swappable. A bug in any
of them must never produce conflicting greens or cut pedestrian clearance short.

## Decision

Controllers receive a sensor-like `IntersectionObservation` and return a `SignalDecision`
(switch or not, preferred next phase). `TrafficSignal` owns timing and enforces minimum
green, yellow, all-red, pedestrian walk and clearance, and max green; phases are validated
against an explicit geometric conflict model at load time. Vehicles additionally yield to
traffic still clearing on conflicting movements.

## Consequences

- Conflict freedom holds for any controller; a property test feeds 50,000 random decisions
  and checks the invariant after every step.
- Controllers are small and focused on policy.
- Some policies (e.g. truly variable yellow) would need the safety layer to expose more
  parameters.
