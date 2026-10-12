# Algorithms

## Routing

Route planning runs **Dijkstra over roads, not nodes** (an edge-based graph). A search
state is "on road r"; successors are the outgoing movements of `r`. Because successors come
from movements, turn restrictions and missing turns are respected automatically and
U-turns are impossible. Ties break by road id, so routes are deterministic.

| Mode (`routing` in a scenario) | Edge cost | Use |
| --- | --- | --- |
| `free_flow_time` (default) | length / speed limit + connector length / turn speed | Realistic shortest-time routes; independent of controller, so experiments stay paired |
| `distance` | length | Shortest path; can choose slow streets |
| `congestion_aware` | length / clamp(mean current speed on the road, 2 m/s, limit) | Reacts to queues; makes routes depend on controller behaviour, so experiments are no longer perfectly paired |

Trade-off: distance routing can send traffic down a slow 20 km/h street; free-flow time
fixes that but ignores congestion; congestion-aware routing spreads load but couples route
choice to signal control and can oscillate when many vehicles react to the same
information. A unit test builds a network where the distance-shortest and time-shortest
routes differ.

**Rerouting**: closing a road reroutes every vehicle whose *remaining* route uses it
(vehicles already in a junction keep their committed exit). If the destination becomes
unreachable, the vehicle diverts to the cheapest reachable exit. Trips that can't be routed
at all are counted as `unserved`.

Complexity: O((R + M) log R) per query for R roads and M movements. Validation uses the
same search to check that every source can reach at least one destination; a BFS helper
(`nodes_reaching`) answers plain reachability questions.

## Phase generation

Conflicts are computed from geometry:

- two movements from **different approaches** conflict if they **merge** into the same
  outgoing road, or if any pair of their connector polylines **cross** (segment
  intersection test on 16-segment Bézier samples);
- a **crosswalk** conflicts with every movement entering or leaving through the arm it
  crosses;
- movements from the same approach never conflict with each other (lane discipline keeps
  them apart).

When a scenario doesn't list phases explicitly, phases are generated:

- **Split phasing** (`phasing: "split"`): one phase per approach serving all of its
  movements. Immune to head-of-line blocking in shared lanes — a left-turner waiting at the
  front of a shared lane can never block through traffic that has green. Common in Indian
  cities. Chosen automatically when any approach has fewer than three lanes.
- **Protected phasing** (`phasing: "protected"`): greedy **graph colouring** of the conflict
  graph over through and left movements, ordered through-first then by conflict degree
  (ties by id), which yields NEMA-like groups (opposing throughs together, opposing
  protected lefts together). Chosen automatically when every approach has dedicated turn
  lanes (≥ 3 lanes).

Then, for both strategies:

1. each crosswalk joins every phase it is compatible with (before right turns are added,
   so right turns don't crowd out parallel walks);
2. **maximal augmentation**: every movement compatible with a phase's movements and
   crosswalks is added to it (this is where right turns go);
3. crosswalks compatible with no phase get an exclusive **pedestrian phase**;
4. timing is applied (min green ≥ walk + clearance where a phase has crosswalks);
5. the plan is validated: no conflicting pair in any phase, every movement served.

Explicit phase lists from scenarios go through the same validation; violations are errors.

Complexity: O(M²·P²) for the conflict matrix (M movements, P polyline points), O(M²·K) for
colouring and augmentation (K phases). It runs once per scenario load.

## Fixed-time controller

Cycles phases in order, switching when the current green reaches its configured duration
(clamped to [min, max] green). Supports per-intersection green overrides and time-of-day
plans (`plans: [{start, greens}]`). Pedestrian-only phases have a fixed length. This is the
control group: it serves every phase every cycle whether or not anyone is waiting.

## Adaptive controller (heuristic)

Observations per phase (aggregated from movements, using lane-based detection: a stopped
queue counts towards the movement of its lane's *front* vehicle, because that's the
movement that has to be served for the queue to move):

- `queued` (v < 2 m/s), `approaching` (moving, within 150 m), `arriving_soon` (would reach the
  stop line within the gap-out time), `wait_sum`, `oldest_wait`, waiting pedestrians and
  their oldest wait, emergency vehicle within 200 m.

Score of a phase:

```
score = w_queue·queued + w_wait·wait_sum/60 + w_oldest·oldest_wait/30 + w_arrivals·approaching
      + (pedestrians waiting ? w_pedestrian·(peds + oldest_ped_wait/30) : 0)
      + (emergency_priority and emergency ? 1000 : 0)
```

Decision at every step once the current green has run its minimum:

1. **Emergency**: if an emergency vehicle waits for another phase and not the current one,
   switch to it.
2. **Starvation guard**: if any other phase's oldest vehicle has waited ≥ `max_wait` (90 s)
   or oldest pedestrian ≥ `max_pedestrian_wait` (90 s), switch to the worst such phase.
3. **Gap-out**: if the current phase has no queue and nobody arriving within
   `gap_out_seconds` (3 s), switch to the best-scoring phase with demand.
4. **Hysteresis**: otherwise switch only if the best rival scores more than
   `current · (1 + hysteresis_ratio) + hysteresis_abs` (defaults 0.25 and 2).
5. Otherwise hold. With no competing demand the signal **rests in green**.

Pedestrian-only phases stop every approach for walk + clearance (~23 s), so they compete
only when no vehicle phase has demand; otherwise the starvation guard serves them. This
rule was added after an experiment showed the first version serving the scramble phase so
often that it was 19% *worse* than fixed-time; without pedestrians the same controller was
55% better. Max-green is enforced by the signal safety layer whenever other phases have
demand.

**Limitations**: it's a greedy, local heuristic. It doesn't coordinate neighbouring
intersections (no green waves), doesn't optimise cycle length, and near saturation its
extra switching costs more lost time than skipping phases saves — the measured results in
the README show it is *not* better than fixed-time in the rush-hour and wide-arterial
scenarios. It's not an optimal controller.

## Max-pressure controller

Every `decision_interval` seconds (after min green) serve the phase maximising

```
pressure(p) = Σ_{m ∈ p} ( upstream(m) − occupancy(out(m)) / movements_into(out(m)) )
```

where `upstream(m)` counts queued and approaching vehicles for the movement and the
downstream term divides the vehicles on the outgoing road among the movements that feed
it. Max-pressure (Varaiya 2013) is throughput-optimal in a store-and-forward network model
under assumptions not met exactly here (finite links, lost time). It uses the same
starvation guard and pedestrian-phase rule as the adaptive controller.

## Experiment statistics

For each seed, every controller sees the identical demand realisation (separate RNG
streams; see [ADR 0005](adr/0005-paired-experiments.md)). For each metric:

- per controller: mean over seeds and 95% CI half-width `t(0.975, n−1) · s / √n`;
- per candidate vs baseline: the **paired** differences (candidate − baseline per seed),
  their mean and CI, the percentage change (omitted when the baseline mean is ~0), and the
  number of seeds where the candidate was strictly better;
- a difference is called distinguishable from noise only when |mean| > CI half-width.

Explanations are generated from the data: the delay verdict with CI, switching frequency
and clearance share, pedestrian waits, and the phase whose share of green time differs
most from its share of arriving vehicles under the baseline, compared with the
candidates' allocations.

## Alternatives considered

- **Reinforcement learning** (e.g. DQN per intersection): not implemented. It would need an
  offline training pipeline and careful evaluation against the same paired protocol; the
  controller interface (`IntersectionObservation` → `SignalDecision`) is designed so a
  learned policy could be dropped in without touching the engine.
- **Rolling-horizon optimisation**: would need an arrival prediction model; deferred.
- **SCOOT/SCATS-style coordination**: needs cycle/offset optimisation across intersections;
  deferred.
