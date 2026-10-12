# Progress log

## Completed

- **Engine**: road graph with geometric movements and crosswalks; edge-based Dijkstra with
  closure rerouting; IDM vehicles with ballistic integration; junction entry (signal,
  dilemma-zone commitment, downstream space, crosswalk and conflict occupancy); lane
  discipline, mandatory and blockage lane changes; pedestrians with recall; three
  controllers; metrics with warm-up, time-weighted queues and delay including unfinished
  trips; deterministic replay; paired parallel experiments.
- **Infrastructure**: strict scenario JSON (unknown keys reported with paths), command
  parser, snapshot/network/metrics serialisation, coordinator thread with accumulator pacing,
  experiment JSON/CSV.
- **Server**: HTTP/1.1 + RFC 6455 on standalone Asio; REST v1; WebSocket v1 with acks,
  resync, latest-wins telemetry, bounded control queues, capped socket buffers; scenario
  CRUD; asynchronous experiments; telemetry; static frontend.
- **Frontend**: canvas renderer (markings, signal heads, served paths, queues, incidents,
  vehicles, pedestrians, labels), live statistics, demand/signal/controller/incident panels,
  experiments dashboard, live charts.
- **Quality**: 124 C++ tests and 5 frontend tests; ASan + UBSan and TSan runs clean;
  benchmarks and a callgrind-guided optimisation (2.2× at 10k vehicles).

## Issues found during development and how they were resolved

| Issue | Resolution |
| --- | --- |
| Generated phases became split phasing; pedestrians only got a scramble phase | Colour through movements first; assign crosswalks before adding right turns |
| Head-of-line blocking by left-turners under protected phasing on 2-lane roads collapsed throughput | Phasing strategy option (`auto` = split unless every approach has dedicated turn lanes); lane-based detection for controllers |
| Merge race: two committed vehicles from conflicting movements reached one lane together | Occupancy check includes committed vehicles still approaching; hold at connector end instead of overlapping |
| Lane change into the gap just ahead of a vehicle leaving a junction | Treat feeder-connector vehicles as followers; no lane changes within 20 m after a junction |
| Left-turner changed into a blocked lane behind the obstacle and got stuck | Never change into a lane blocked ahead |
| Adaptive control 19% worse than fixed-time with pedestrians | Pedestrian-only phases compete only when no vehicle phase has demand; starvation guard serves them |
| Acks could arrive before the state they acknowledge | Publish frame/status before running command callbacks |
| Slow-client coalescing never triggered on loopback | Cap WebSocket kernel send buffer |
| Scenario ids lost (read after `std::move`) | Copy the id first; duplicates reported |
| Routes recomputed every step for waiting trips (23% of runtime) | Cache planned routes |

## Outstanding / next steps

1. Build and run the Docker image in CI (the development sandbox couldn't reach Docker Hub).
2. Binary (or delta) snapshot encoding for very large networks.
3. Discretionary lane changes (MOBIL) and yielding to emergency vehicles.
4. Coordinated fixed-time plans (offsets / green waves) as a stronger baseline on grids.
5. A learned controller behind the existing `SignalController` interface, evaluated with the
   same paired protocol.
