#include <gtest/gtest.h>

#include <set>

#include "test_util.hpp"
#include "traffic/core/rng.hpp"
#include "traffic/network/network_builder.hpp"
#include "traffic/signals/traffic_signal.hpp"

using namespace traffic;

namespace {
struct Fixture {
  RoadNetwork net;
  ConflictModel cm;
  explicit Fixture(int lanes = 2) {
    auto res = build_network(test::single_intersection(0, lanes).network);
    net = std::move(res.network);
    cm = ConflictModel(net, 0);
  }
  MovementId mv(NodeId from, NodeId to) const { return net.find_movement(net.find_road(from, 0), net.find_road(0, to)); }
  PhasePlan plan(const std::string& phasing) const {
    SignalTimingSpec t;
    t.phasing = phasing;
    return build_phase_plan(net, 0, cm, t, PedestrianSpec{}, nullptr);
  }
};
}  // namespace

TEST(ConflictModel, GeometricConflicts) {
  Fixture f;
  // Opposing through movements are compatible; crossing through movements conflict.
  EXPECT_FALSE(f.cm.conflicts(f.mv(1, 3), f.mv(3, 1)));
  EXPECT_TRUE(f.cm.conflicts(f.mv(1, 3), f.mv(2, 4)));
  // A left turn conflicts with the opposing through movement.
  EXPECT_TRUE(f.cm.conflicts(f.mv(1, 2), f.mv(3, 1)));
  // Opposing left turns pass each other without crossing.
  EXPECT_FALSE(f.cm.conflicts(f.mv(1, 2), f.mv(3, 4)));
  // Merges into the same exit conflict (N right -> W and S left -> W).
  EXPECT_TRUE(f.cm.conflicts(f.mv(1, 4), f.mv(3, 4)));
  // Movements from the same approach never conflict with each other.
  EXPECT_FALSE(f.cm.conflicts(f.mv(1, 2), f.mv(1, 3)));
}

TEST(ConflictModel, CrosswalkConflicts) {
  Fixture f;
  const auto& node = f.net.node(0);
  int north_cw = -1;
  for (const auto& c : node.crosswalks)
    if (c.arm_node == 1) north_cw = c.index;
  ASSERT_GE(north_cw, 0);
  EXPECT_TRUE(f.cm.crosswalk_conflicts(north_cw, f.mv(1, 3)));   // leaves via the north arm
  EXPECT_TRUE(f.cm.crosswalk_conflicts(north_cw, f.mv(3, 1)));   // arrives on the north arm
  EXPECT_FALSE(f.cm.crosswalk_conflicts(north_cw, f.mv(2, 4)));  // east-west through
}

class PhasePlanTest : public ::testing::TestWithParam<std::pair<int, std::string>> {};

TEST_P(PhasePlanTest, GeneratedPlansAreConflictFreeAndComplete) {
  const auto [lanes, phasing] = GetParam();
  Fixture f(lanes);
  auto plan = f.plan(phasing);
  ASSERT_TRUE(plan.errors.empty()) << plan.errors.front();
  EXPECT_TRUE(validate_phase_plan(f.net, 0, f.cm, plan.phases).empty());
  std::set<MovementId> served;
  std::set<int> walks;
  for (const auto& p : plan.phases) {
    served.insert(p.movements.begin(), p.movements.end());
    walks.insert(p.crosswalks.begin(), p.crosswalks.end());
    if (!p.crosswalks.empty()) {
      EXPECT_GE(p.min_green, p.walk_time + p.ped_clearance);
    }
  }
  EXPECT_EQ(served.size(), f.cm.movements().size());
  EXPECT_EQ(walks.size(), f.net.node(0).crosswalks.size());  // every crosswalk gets WALK somewhere
}

INSTANTIATE_TEST_SUITE_P(Strategies, PhasePlanTest,
                         ::testing::Values(std::pair{2, std::string("split")}, std::pair{2, std::string("protected")},
                                           std::pair{3, std::string("protected")}, std::pair{1, std::string("auto")},
                                           std::pair{3, std::string("auto")}));

TEST(PhasePlan, SplitPhasingServesOneApproachPerPhase) {
  Fixture f(2);
  auto plan = f.plan("split");
  int vehicle_phases = 0;
  for (const auto& p : plan.phases) {
    if (!p.serves_vehicles()) continue;
    ++vehicle_phases;
  }
  EXPECT_EQ(vehicle_phases, 4);
}

TEST(PhasePlan, ExplicitConflictingPhaseIsRejected) {
  Fixture f;
  IntersectionSignalSpec spec;
  spec.node = 0;
  PhaseSpec bad;
  bad.movements = {{1, 3}, {2, 4}};  // crossing through movements
  spec.phases = {bad};
  auto plan = build_phase_plan(f.net, 0, f.cm, SignalTimingSpec{}, PedestrianSpec{}, &spec);
  ASSERT_FALSE(plan.errors.empty());
  bool mentions_conflict = false, mentions_unserved = false;
  for (const auto& e : plan.errors) {
    mentions_conflict |= e.find("conflicting") != std::string::npos;
    mentions_unserved |= e.find("never served") != std::string::npos;
  }
  EXPECT_TRUE(mentions_conflict);
  EXPECT_TRUE(mentions_unserved);
}

TEST(TrafficSignal, MinGreenYellowAllRedSequence) {
  Fixture f;
  auto plan = f.plan("split");
  TrafficSignal sig(f.net, 0, plan.phases);
  const auto& p0 = plan.phases[0];
  const double dt = 0.1;
  SignalDecision want{true, 1};
  // Requests before min-green are ignored.
  for (int i = 0; i < static_cast<int>((p0.min_green - 1) / dt); ++i) sig.step(dt, want, true);
  EXPECT_EQ(sig.stage(), SignalStage::Green);
  EXPECT_EQ(sig.current_phase(), 0);
  while (sig.stage() == SignalStage::Green) sig.step(dt, want, true);
  EXPECT_EQ(sig.stage(), SignalStage::Yellow);
  for (MovementId m : p0.movements)
    if (!plan.phases[1].contains(m)) {
      EXPECT_EQ(sig.movement_state(m), LightState::Yellow);
    }
  int yellow_steps = 0;
  while (sig.stage() == SignalStage::Yellow) {
    sig.step(dt, want, true);
    ++yellow_steps;
  }
  EXPECT_NEAR(yellow_steps * dt, p0.yellow, dt + 1e-9);
  EXPECT_EQ(sig.stage(), SignalStage::AllRed);
  while (sig.stage() == SignalStage::AllRed) sig.step(dt, SignalDecision{}, true);
  EXPECT_EQ(sig.current_phase(), 1);
  EXPECT_EQ(sig.switches(), 1);
}

TEST(TrafficSignal, MaxGreenForcesChangeOnlyWithCompetingDemand) {
  Fixture f;
  auto plan = f.plan("split");
  TrafficSignal sig(f.net, 0, plan.phases);
  const double max_g = plan.phases[0].max_green;
  for (int i = 0; i < static_cast<int>((max_g + 30) / 0.1); ++i) sig.step(0.1, SignalDecision{false, 2}, false);
  EXPECT_EQ(sig.stage(), SignalStage::Green);  // rests in green: nobody else waiting
  EXPECT_EQ(sig.current_phase(), 0);
  sig.step(0.1, SignalDecision{false, 2}, true);
  EXPECT_EQ(sig.stage(), SignalStage::Yellow);
  EXPECT_EQ(sig.next_phase(), 2);
}

TEST(TrafficSignal, RandomDecisionsNeverViolateSafetyInvariants) {
  // Property test: whatever a (possibly adversarial) controller asks, the safety layer
  // never shows conflicting greens or WALK across a moving stream.
  for (const std::string phasing : {"split", "protected"}) {
    Fixture f(3);
    auto plan = f.plan(phasing);
    TrafficSignal sig(f.net, 0, plan.phases);
    Rng rng(99);
    for (int i = 0; i < 50000; ++i) {
      SignalDecision d{rng.uniform() < 0.3, static_cast<int>(rng.uniform() * (sig.phase_count() + 2)) - 1};
      if (rng.uniform() < 0.05) sig.request_walk(static_cast<int>(rng.uniform() * 4));
      sig.step(0.1, d, rng.uniform() < 0.5);
      std::string why;
      ASSERT_TRUE(sig.invariants_hold(&why)) << phasing << " step " << i << ": " << why;
    }
    EXPECT_GT(sig.switches(), 100);
  }
}

TEST(TrafficSignal, PedestrianRecallDefersPhaseChange) {
  Fixture f;
  auto plan = f.plan("protected");
  int phase_with_walk = -1;
  for (std::size_t i = 0; i < plan.phases.size(); ++i)
    if (!plan.phases[i].crosswalks.empty() && plan.phases[i].serves_vehicles()) phase_with_walk = static_cast<int>(i);
  ASSERT_GE(phase_with_walk, 0);
  std::rotate(plan.phases.begin(), plan.phases.begin() + phase_with_walk, plan.phases.end());
  TrafficSignal sig(f.net, 0, plan.phases);
  const int cw = plan.phases[0].crosswalks.front();
  EXPECT_EQ(sig.crosswalk_state(cw), WalkState::Walk);
  const double ped_total = plan.phases[0].walk_time + plan.phases[0].ped_clearance;
  for (int i = 0; i < static_cast<int>((ped_total + 2) / 0.1); ++i) sig.step(0.1, SignalDecision{}, false);
  EXPECT_EQ(sig.crosswalk_state(cw), WalkState::DontWalk);
  sig.request_walk(cw);
  EXPECT_EQ(sig.crosswalk_state(cw), WalkState::Walk);
  sig.step(0.1, SignalDecision{true, 1}, true);  // change request during the recalled walk
  EXPECT_EQ(sig.stage(), SignalStage::Green);   // deferred until walk + clearance complete
  for (int i = 0; i < static_cast<int>((ped_total + 1) / 0.1); ++i) sig.step(0.1, SignalDecision{true, 1}, true);
  EXPECT_NE(sig.stage(), SignalStage::Green);
}

TEST(TrafficSignal, PhaseTimingEditsAreValidated) {
  Fixture f;
  auto plan = f.plan("split");
  TrafficSignal sig(f.net, 0, plan.phases);
  std::string err;
  EXPECT_FALSE(sig.set_phase_timing(0, 30, 20, &err));
  EXPECT_FALSE(sig.set_phase_timing(42, 5, 20, &err));
  EXPECT_TRUE(sig.set_phase_timing(0, 10, 45, &err)) << err;
  EXPECT_DOUBLE_EQ(sig.phases()[0].max_green, 45);
  const int ped = sig.phase_count() - 1;  // scramble phase needs walk + clearance
  EXPECT_FALSE(sig.set_phase_timing(ped, 5, 60, &err));
}
