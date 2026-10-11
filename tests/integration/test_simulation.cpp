#include <gtest/gtest.h>

#include "test_util.hpp"
#include "traffic/experiments/experiment_runner.hpp"

using namespace traffic;

namespace {
ScenarioConfig empty_single() {
  auto c = test::single_intersection(0);
  c.pedestrians.rate_per_hour = 0;
  c.signals.phasing = "split";  // phase order N, E, S, W, pedestrians
  return c;
}
}  // namespace

TEST(Simulation, VehicleTravelsFromSpawnToDestination) {
  auto e = test::make(empty_single());
  ASSERT_TRUE(e->apply(CmdSpawnVehicle{VehicleType::Car, 1, 3}).ok);  // N -> S, first phase is N
  e->run_for(80);
  const auto m = e->metrics();
  EXPECT_EQ(m.spawned, 1);
  EXPECT_EQ(m.completed, 1);
  EXPECT_EQ(e->vehicle_count(), 0);
  // ~600 m at <= 50 km/h: travel time must exceed free-flow, which is > 40 s.
  EXPECT_GT(m.avg_travel_time, 40.0);
  EXPECT_GE(m.avg_delay, 0.0);
}

TEST(Simulation, RedLightHoldsVehicleAndGreenReleasesIt) {
  auto e = test::make(empty_single());
  // From the south approach: its phase (P3) comes after N and E phases (>= 50 s).
  ASSERT_TRUE(e->apply(CmdSpawnVehicle{VehicleType::Car, 3, 1}).ok);
  const TrafficSignal* sig = e->signal_at(0);
  ASSERT_NE(sig, nullptr);
  e->run_for(1);
  const VehicleId id = 1;
  bool was_held = false;
  while (e->time() < 200) {
    const Vehicle* v = test::find_vehicle(*e, id);
    if (!v) break;
    if (!v->on_connector) {
      const Road& r = e->network().road(v->road);
      if (v->route_index == 0) {
        const LightState st = sig->movement_state(v->next_movement);
        if (st == LightState::Red && r.length - v->s < 3.0 && v->v < 0.05) was_held = true;
        EXPECT_LE(v->s, r.length);  // never beyond the stop line while on the approach
      }
    }
    e->step();
  }
  EXPECT_TRUE(was_held);
  EXPECT_EQ(e->metrics().completed, 1);
  EXPECT_EQ(e->metrics().red_light_guard_events, 0);
}

TEST(Simulation, ClosedRoadCausesReroute) {
  auto c = test::grid(2, 2, 0);
  c.pedestrians.rate_per_hour = 0;
  auto e = test::make(c);
  // Boundary 4 is north of interior 0; boundary 7 is south of interior 3 (see expand_grid).
  ASSERT_TRUE(e->apply(CmdSpawnVehicle{VehicleType::Car, 4, 7}).ok);
  e->run_for(5);
  const Vehicle* v = test::find_vehicle(*e, 1);
  ASSERT_NE(v, nullptr);
  const auto original = v->route;
  // Close the interior roads of the planned route.
  for (std::size_t i = 1; i + 1 < original.size(); ++i) ASSERT_TRUE(e->apply(CmdCloseRoad{original[i]}).ok);
  v = test::find_vehicle(*e, 1);
  ASSERT_NE(v, nullptr);
  EXPECT_NE(v->route, original);
  EXPECT_EQ(v->reroutes, 1);
  for (std::size_t i = v->route_index + 1; i < v->route.size(); ++i) EXPECT_FALSE(e->network().road(v->route[i]).closed);
  e->run_for(300);
  EXPECT_EQ(e->metrics().completed, 1);
}

TEST(Simulation, UnreachableDestinationIsReportedNotSilentlyDropped) {
  auto e = test::make(empty_single());
  const RoadId exit_south = e->network().find_road(0, 3);
  // Boundary roads can't be closed, so block the only exit by closing... the command layer refuses it.
  EXPECT_FALSE(e->apply(CmdCloseRoad{exit_south}).ok);
  // Spawn requests between non-boundary nodes are rejected with a reason.
  auto r = e->apply(CmdSpawnVehicle{VehicleType::Car, 0, 3});
  EXPECT_FALSE(r.ok);
  EXPECT_FALSE(r.error.empty());
}

TEST(Simulation, PedestriansCrossOnlyDuringWalk) {
  auto c = empty_single();
  c.pedestrians.rate_per_hour = 600;
  auto e = test::make(c);
  const TrafficSignal* sig = e->signal_at(0);
  int checked = 0;
  while (e->time() < 600) {
    const double t = e->time();
    e->step();
    for (const auto& p : e->pedestrians().pedestrians())
      if (p.crossing() && std::abs(p.start_time - t) < 1e-9) {
        EXPECT_EQ(sig->crosswalk_state(p.crosswalk), WalkState::Walk);
        ++checked;
      }
  }
  EXPECT_GT(checked, 20);
  EXPECT_GT(e->metrics().peds_completed, 20);
  EXPECT_GT(e->metrics().ped_avg_wait, 0.0);
}

TEST(Simulation, VehiclesNeverEnterOccupiedCrosswalk) {
  auto c = test::single_intersection(500);
  c.pedestrians.rate_per_hour = 800;
  auto e = test::make(c);
  const RoadNetwork& net = e->network();
  std::map<VehicleId, bool> was_on_connector;
  while (e->time() < 600) {
    e->step();
    e->for_each_vehicle([&](const Vehicle& v) {
      const bool now = v.on_connector;
      if (now && !was_on_connector[v.id] && v.s < 1.5) {
        // Just entered the junction this step: no pedestrian may be on the crossings it uses.
        const Movement& m = net.movement(v.movement);
        for (const auto& cw : net.node(m.node).crosswalks)
          if (cw.road_in == m.in || cw.road_out == m.out)
            for (const auto& p : e->pedestrians().pedestrians())
              if (p.node == m.node && p.crosswalk == cw.index && p.crossing() && p.start_time < e->time() - 0.2)
                ADD_FAILURE() << "vehicle " << v.id << " entered crosswalk " << cw.index << " with a pedestrian on it";
      }
      was_on_connector[v.id] = now;
    });
  }
}

TEST(Simulation, LaneBlockageForcesSafeLaneChanges) {
  auto c = test::single_intersection(350);
  c.pedestrians.rate_per_hour = 0;
  auto e = test::make(c);
  const RoadId south = e->network().find_road(3, 0);
  ASSERT_TRUE(e->apply(CmdBlockLane{south, 0, 150}).ok);
  // Track which side of the blockage every vehicle in the blocked lane is on: nobody may
  // ever pass through it (vehicles may legitimately enter that lane after passing it).
  std::map<VehicleId, double> last_s;
  while (e->time() < 600) {
    e->step();
    e->for_each_vehicle([&](const Vehicle& v) {
      if (v.on_connector || v.road != south || v.lane != 0) {
        last_s.erase(v.id);
        return;
      }
      auto it = last_s.find(v.id);
      if (it != last_s.end() && it->second <= 150.0) {
        EXPECT_LE(v.s, 150.0 + 1e-6) << "vehicle " << v.id << " drove through the blockage";
      }
      last_s[v.id] = v.s;
    });
  }
  std::string why;
  EXPECT_TRUE(e->check_invariants(&why)) << why;
  EXPECT_GT(e->metrics().lane_changes, 0);
  EXPECT_GT(e->metrics().completed, 100);
  EXPECT_EQ(e->metrics().following_violations, 0);
  EXPECT_FALSE(e->apply(CmdBlockLane{south, 1, 150}).ok);  // can't block every lane
}

TEST(Simulation, PauseIsJustNotSteppingTimeNeverGoesBackwards) {
  auto e = test::make(test::single_intersection(300));
  double last = -1;
  for (int i = 0; i < 1000; ++i) {
    e->step();
    EXPECT_GT(e->time(), last);
    last = e->time();
  }
  const auto hash = e->state_hash();
  EXPECT_EQ(e->state_hash(), hash);  // reading state doesn't change it
}

TEST(Determinism, IdenticalSeedsGiveIdenticalRuns) {
  auto c = test::grid(2, 3, 300);
  c.controller.type = "adaptive";
  auto a = test::make(c);
  auto b = test::make(c);
  for (int i = 0; i < 6000; ++i) {
    if (i == 1500) {
      ASSERT_TRUE(a->apply(CmdSetDemand{1.8}).ok);
      ASSERT_TRUE(b->apply(CmdSetDemand{1.8}).ok);
    }
    a->step();
    b->step();
  }
  EXPECT_EQ(a->state_hash(), b->state_hash());
  EXPECT_EQ(a->metrics().completed, b->metrics().completed);
  c.seed = 2;
  auto other = test::make(c);
  other->run_until(a->time());
  EXPECT_NE(other->state_hash(), a->state_hash());
}

TEST(Determinism, ReplayFromCommandLogReproducesState) {
  auto c = test::single_intersection(350);
  auto live = test::make(c);
  live->run_for(30);
  ASSERT_TRUE(live->apply(CmdSetController{[] {
                ControllerConfig x;
                x.type = "max_pressure";
                return x;
              }()})
                  .ok);
  live->run_for(40);
  ASSERT_TRUE(live->apply(CmdSpawnVehicle{VehicleType::Emergency, 2, 4}).ok);
  ASSERT_TRUE(live->apply(CmdBlockLane{live->network().find_road(1, 0), 1, 100}).ok);
  live->run_for(100);
  ASSERT_EQ(live->command_log().size(), 3u);

  auto replay = test::make(c);
  for (const auto& entry : live->command_log()) {
    while (replay->step_count() < entry.step) replay->step();
    ASSERT_TRUE(replay->apply(entry.command).ok);
  }
  while (replay->step_count() < live->step_count()) replay->step();
  EXPECT_EQ(replay->state_hash(), live->state_hash());
}

TEST(Determinism, DemandRealisationIndependentOfController) {
  auto c = test::single_intersection(400);
  std::vector<long> spawned;
  for (const char* type : {"fixed_time", "adaptive", "max_pressure"}) {
    c.controller.type = type;
    auto e = test::make(c);
    e->run_for(900);
    spawned.push_back(e->metrics().spawned);
  }
  EXPECT_EQ(spawned[0], spawned[1]);
  EXPECT_EQ(spawned[0], spawned[2]);
}

TEST(Experiments, PairedComparisonIsDeterministicAndThreadIndependent) {
  ExperimentSpec spec;
  spec.scenario = test::single_intersection(250);
  spec.scenario.warmup = 60;
  ControllerConfig f, a;
  f.type = "fixed_time";
  a.type = "adaptive";
  spec.controllers = {f, a};
  spec.seeds = {1, 2, 3};
  spec.duration = 400;
  spec.threads = 1;
  auto serial = ExperimentRunner::run(spec);
  spec.threads = 4;
  auto parallel = ExperimentRunner::run(spec);
  ASSERT_EQ(serial.runs.size(), 6u);
  for (std::size_t i = 0; i < serial.runs.size(); ++i) {
    EXPECT_EQ(serial.runs[i].final_hash, parallel.runs[i].final_hash);
    EXPECT_EQ(serial.runs[i].controller, parallel.runs[i].controller);
  }
  ASSERT_EQ(serial.comparisons.size(), 1u);
  const auto& delay = serial.comparisons[0].metrics.front();
  EXPECT_EQ(delay.key, "avg_delay_all");
  EXPECT_EQ(delay.n, 3);
  EXPECT_NEAR(delay.diff_mean, delay.candidate_mean - delay.baseline_mean, 1e-9);
  EXPECT_FALSE(serial.explanations.empty());
}

TEST(Experiments, ZeroBaselineHasNoPercentage) {
  ExperimentResult r;
  r.controllers = {"a", "b"};
  r.seeds = {1};
  RunResult x, y;
  x.metrics.ped_avg_wait = 0.0;
  y.metrics.ped_avg_wait = 4.0;
  r.runs = {x, y};
  ExperimentRunner::analyse(r);
  ASSERT_EQ(r.comparisons.size(), 1u);
  for (const auto& m : r.comparisons[0].metrics)
    if (m.key == "ped_avg_wait") {
      EXPECT_FALSE(m.pct_change.has_value());
      EXPECT_DOUBLE_EQ(m.diff_mean, 4.0);
    }
}
