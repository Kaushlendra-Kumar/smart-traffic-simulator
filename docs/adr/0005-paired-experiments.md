# ADR 0005: Paired experiments through independent RNG streams

**Status**: accepted

## Context

Comparing controllers with independent random demand mixes controller effects with demand
noise. The brief asks for identical demand realisations where practical.

## Decision

Randomness is split into streams seeded from the scenario seed: demand arrivals, vehicle
attributes and pedestrians. The demand stream consumes exactly one draw per source per step
whatever happens, and trip attributes are drawn whether or not a vehicle type is enabled.
Routing defaults to free-flow travel time, which doesn't depend on controller behaviour.
For each seed every controller therefore sees the same arrivals, origins, destinations,
classes and driver parameters; statistics are computed on paired differences.

## Consequences

- Much narrower confidence intervals for the same number of seeds; a test asserts equal
  trip counts across controllers.
- `congestion_aware` routing intentionally breaks perfect pairing (routes react to
  controller-induced queues); this is documented.
- The engine uses its own PRNG and double conversion, because
  `std::uniform_real_distribution` is implementation-defined.
