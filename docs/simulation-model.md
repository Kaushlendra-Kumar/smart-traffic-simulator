# Simulation model

## Coordinate system and units

World coordinates are metres with **x pointing east and y pointing north**; speeds are
m/s; time is seconds of simulation time. The renderer applies its own pan/zoom transform
(and flips y for the screen); nothing in the view affects the simulation.

Traffic drives on the **right**: each direction of a two-way road occupies the half of the
carriageway to the right of its travel direction. Lane 0 is the inner lane (next to the
centre line).

## Road graph

- **Nodes** are intersections or boundary endpoints. Ids are dense (0..N−1). A node with a
  single neighbour is a boundary node where vehicles enter and leave. Nodes with three or
  more arms are signalised (unsignalised priority junctions are not modelled and are
  rejected at load time); two-arm nodes are simple bends.
- **Roads** are directed edges with 1–5 lanes, a speed limit and a reverse road if two-way.
  Each road is trimmed by the junction-box radius at both ends, so `length` is the drivable
  length between stop lines.
- **Movements** connect an incoming road to an outgoing road through a node (no U-turns).
  Turns are classified from the angle between directions (straight within ±35°). Each
  movement has one **connector** per incoming lane: a quadratic Bézier from the end of the
  lane to the start of the target lane. Left turns bend around the node centre so that
  opposing left turns pass each other without crossing.
- **Crosswalks** cross each arm of a signalised node just inside the junction box.
- Grids are generated from `rows × cols` with boundary stubs; interior node `(r, c)` has id
  `r·cols + c`.

Validation (all reported, never silently fixed): duplicate/unknown/non-dense node ids,
duplicate roads, self-loops, roads too short after trimming, lane counts, non-positive
speed limits, dead ends, isolated nodes, unsignalised multi-arm nodes, boundary nodes that
cannot reach any destination, and (for signals) conflicting or unserved movements.

### Lane discipline

| Lanes | Inner lane | Middle lanes | Outer lane |
| --- | --- | --- | --- |
| 1 | everything | — | — |
| 2 | left, straight | — | straight, right |
| 3+ | left (and straight if no left exists) | straight | right (and straight if no right exists) |

Vehicles choose a permitted lane when they enter a road (the one with the most space) and
perform a **mandatory lane change** if a connector delivered them to a lane that doesn't
permit their next turn.

## Vehicle dynamics

Longitudinal motion uses the **Intelligent Driver Model** (Treiber, Hennecke & Helbing 2000):

```
a = a_max · [ 1 − (v / v0)^4 − (s* / s)^2 ]
s* = s0 + max(0, v·T + v·Δv / (2·√(a_max·b)))
```

| Class | a_max | b | T | s0 | length | desired speed |
| --- | --- | --- | --- | --- | --- | --- |
| Car | 1.5 | 2.0 | 1.4 s (±15%) | 2.0 m | 4.5 m | limit × (0.9–1.1) |
| Bus | 1.0 | 1.5 | 1.6 s (±15%) | 2.5 m | 12 m | limit × 0.85 × (0.9–1.1) |
| Emergency | 2.0 | 2.5 | 1.1 s (±15%) | 2.0 m | 6 m | limit × 1.15 × (0.9–1.1) |

Acceleration is clamped to [−9, a_max] m/s². Integration uses the **ballistic scheme**
(exact for constant acceleration within a step, never produces negative speed):
`v' = max(0, v + a·dt)`, `x' = x + v·dt + ½a·dt²` (or the stopping distance if the vehicle
stops within the step).

The "leader" for the gap `s` is, in priority order of proximity:

1. the vehicle ahead in the same lane;
2. a lane blockage (stationary obstacle);
3. at the front of the lane: if the vehicle **may enter** the junction, the nearest vehicle
   ahead on any connector feeding the same target lane, or the tail of the target lane
   (all compared by distance to the start of the target lane, so merges are handled);
   otherwise a virtual stopped obstacle just past the stop line.

Vehicles anticipate turns by capping their desired speed at
`√(v_turn² + 2·b·d)` where `d` is the distance to the stop line (left turns 9 m/s, right
turns 7 m/s).

### Junction entry

A front vehicle may enter its movement if all of these hold:

- the signal shows green (or the vehicle is already **committed**);
- the target lane has room for it (counting vehicles already on connectors heading there),
  which prevents blocking the box;
- no pedestrian is on a crosswalk the movement crosses;
- no vehicle is on, or committed to, a **conflicting** movement (vehicles still clearing the
  junction from the previous phase have priority).

**Commitment** (dilemma-zone rule): on green, a vehicle commits once it is within its
comfortable stopping distance plus a margin; on yellow it commits only if it can no longer
stop comfortably (`v²/2b > d − 0.5`). Committed vehicles proceed even if the light turns red.
As a last-resort guard, an uncommitted vehicle is never allowed past the stop line; if the
guard ever triggers it is counted (`red_light_guard_events`) — it is 0 across all shipped
scenarios and controllers.

### Lane changes

Lane changes happen for two reasons: the current lane doesn't permit the next turn
(mandatory), or a blockage lies ahead within 80 m. A change is accepted only if:

- the new leader gap is at least `max(s0, 0.5·v)`;
- the new follower's gap is at least `max(s0, 0.5·v_f)` and the IDM deceleration it would
  need is milder than 4 m/s²;
- vehicles about to leave a junction connector into the target lane are also treated as
  followers;
- the target lane is not blocked ahead;
- the vehicle is not within 20 m after a junction or 3 m before a stop line, and has not
  changed lanes in the last 3 s.

The change is instantaneous in the lane lists (so following logic stays exact) while the
rendered position slides across over ~2 s (`lateral_offset`, render-only). If a mandatory
change can't be completed and the vehicle is stopped near the line, it re-plans its route
using a turn permitted from its current lane (`forced_reroutes`).

## Signals

Phases list movements that receive green and crosswalks that receive WALK. The
`TrafficSignal` state machine has three stages — Green, Yellow, All-red — and enforces:

- minimum green before any change; maximum green when other phases have demand;
- yellow (3.5 s default, 0 for pedestrian-only phases) then all-red (1.5 s);
- movements shared by the current and next phase stay green through the change;
- WALK for the walk time, then flashing don't-walk for `crosswalk length / 1.0 m/s`;
  phases with crosswalks have `min_green ≥ walk + clearance`;
- **pedestrian recall**: if pedestrians arrive at a crosswalk of a resting green phase, WALK
  is shown again (if it fits before max green) and the phase change is deferred until the
  clearance ends.

Conflicts are derived from geometry (see [algorithms.md](algorithms.md#phase-generation)).
The invariant "no two conflicting movements are non-red, and no WALK crosses a non-red
movement" is checked by `TrafficSignal::invariants_hold` and property-tested with 50,000
random controller decisions.

## Pedestrians

Pedestrians arrive at each crosswalk of a signalised node as a Bernoulli process per step
(rate per intersection split evenly across crosswalks), wait at the kerb, start only
during WALK, and cross at a speed drawn from [1.0, 1.5] m/s. Vehicles never enter a
connector whose crosswalks are occupied. Pedestrian waiting time is measured from arrival
to the start of crossing.

This is a behavioural proxy for waiting time and signal compatibility. It does not model
pedestrian–vehicle interaction physics and must not be used as a pedestrian-safety analysis.

## Demand

Each boundary node with outgoing roads is a source with a rate (veh/h, overridable per
source) and a destination distribution (default: every other reachable boundary node
equally). A piecewise-linear **profile** scales demand over time (rush hours), and a runtime
multiplier scales it interactively. Arrivals are Bernoulli per step with probability
`rate × multiplier × dt / 3600` — a discretised Poisson process.

Routes are computed when a trip is requested (Dijkstra; see algorithms) and cached; if a
closure invalidates a cached route it is recomputed. Trips that cannot enter immediately
wait in a per-source backlog (a vertical queue) and their waiting time counts as delay.

## Update order

Within a step:

1. **Scheduled scenario events** (closures, blockages, demand changes) — so the step sees
   a consistent network.
2. **Demand and network entry** — new vehicles join before any dynamics this step.
3. **Pedestrian arrivals.**
4. **Signals** — controllers observe the state at the start of the step and propose;
   signals enforce timing. Lights are then fixed for the rest of the step.
5. **Pedestrians start/advance** — after lights change, so a pedestrian starts only on
   WALK, and *before* vehicles check crosswalk occupancy, so a vehicle never enters a
   crossing a pedestrian started on this step.
6. **Accelerations** for all vehicles from a read-only view of the step-start state (order
   independent).
7. **Integration and transitions** — connector exits first (a vehicle can't traverse two
   segments in one step), then lane exits; completions are recorded.
8. **Lane changes** — on the integrated state, in deterministic lane order.
9. **Bookkeeping and metrics**, then the clock advances (`t = step × dt`).

## Metrics

| Metric | Definition |
| --- | --- |
| Travel time | exit time − demand time (includes waiting to enter) |
| Free-flow time | Σ over traversed segments of length / desired free speed on that segment |
| Delay | travel time − free-flow time (clamped at 0) |
| Delay incl. unfinished | (Σ delay of completed trips + delay accrued so far by trips still in the network or waiting to enter) / number of those trips. Prevents a controller that strands vehicles from looking good |
| Stopped time / stops | time with v < 0.5 m/s; a stop starts when speed drops below 0.5 m/s after exceeding 2 m/s |
| Queue | vehicles below 2 m/s on roads entering signalised nodes; averages are **time-weighted** (∫q dt / T) |
| Max queue | largest single-approach queue observed |
| Throughput | measured completions per simulated hour; the time series uses a 60 s rolling window |
| Pedestrian wait | start of crossing − arrival |
| Clearance fraction | share of signal time in yellow or all-red |

Only trips (and pedestrians) whose demand time is after the **warm-up** count towards
averages. Percentiles use linear interpolation; p95 is reported only with ≥ 20 samples.
Averages are never averaged again without weights: experiment aggregates are means of
per-run values, one per seed, with Student-t confidence intervals.

Fuel consumption and emissions are **not** modelled.

## Determinism

A run is reproducible from: scenario configuration, seed, controller configuration,
timestep, and the ordered command log (each entry records the step before which it was
applied). The server's seek rebuilds an engine and replays the log; tests assert equal
state hashes.

What could break determinism and how it's avoided:

| Risk | Mitigation |
| --- | --- |
| `std::uniform_real_distribution` differs across standard libraries | Own xoshiro256** generator and bit-exact double conversion |
| One RNG shared by subsystems | Separate streams (demand, vehicle attributes, pedestrians); each step consumes a fixed number of draws per source regardless of outcome |
| Unordered iteration | Vectors indexed by id; `std::map` where ordering matters; no `unordered_*` containers in the engine |
| Concurrent mutation | One thread owns the engine; experiments use private engines |
| Floating-point accumulation in time | Time derived from the integer step count |
| Wall-clock dependence | None in the engine; the coordinator converts wall time into whole steps |
| Compiler/platform floating-point differences | Not eliminated: results are reproducible on the same build; across compilers or `-ffast-math` they may differ. The engine is not built with fast-math |

## Limitations

- No unsignalised priority junctions with 3+ arms, roundabouts, or left-hand traffic.
- Vehicles from the same approach don't interact inside the junction box; there is no
  explicit collision model inside junctions — conflicts are prevented by signals,
  yellow/all-red clearance and the occupancy rule above.
- No discretionary (speed-gain) lane changes; other drivers don't yield to emergency
  vehicles; no pedestrian jaywalking.
- Pedestrian-only phases are exclusive "scramble" phases when no vehicle phase is
  compatible with a crosswalk.
