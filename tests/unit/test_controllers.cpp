#include <gtest/gtest.h>

#include "traffic/controllers/signal_controller.hpp"

using namespace traffic;

namespace {

std::vector<SignalPhase> phases(int n, bool with_ped_phase = false) {
  std::vector<SignalPhase> out;
  for (int i = 0; i < n; ++i) {
    SignalPhase p;
    p.name = "P" + std::to_string(i + 1);
    p.movements = {i};
    p.min_green = 7;
    p.max_green = 60;
    p.green = 20;
    out.push_back(p);
  }
  if (with_ped_phase) {
    SignalPhase p;
    p.name = "ped";
    p.crosswalks = {0};
    p.min_green = p.max_green = p.green = 20;
    p.yellow = 0;
    out.push_back(p);
  }
  return out;
}

IntersectionObservation obs(const std::vector<SignalPhase>& ph, int current, double t_in_green) {
  IntersectionObservation o;
  o.node = 0;
  o.current_phase = current;
  o.stage = SignalStage::Green;
  o.time_in_stage = t_in_green;
  o.phases = &ph;
  o.phase_obs.resize(ph.size());
  return o;
}

}  // namespace

TEST(FixedTime, SwitchesExactlyAfterConfiguredGreenAndCycles) {
  auto ph = phases(3);
  ControllerConfig cfg;
  cfg.greens = {10, 15, 25};
  FixedTimeController c(cfg);
  c.initialize({0, ph});
  EXPECT_FALSE(c.decide(obs(ph, 0, 9.9), 0).switch_phase);
  auto d = c.decide(obs(ph, 0, 10.0), 0);
  EXPECT_TRUE(d.switch_phase);
  EXPECT_EQ(d.next_phase, 1);
  EXPECT_FALSE(c.decide(obs(ph, 1, 14.0), 0).switch_phase);
  EXPECT_TRUE(c.decide(obs(ph, 1, 15.0), 0).switch_phase);
  EXPECT_EQ(c.decide(obs(ph, 2, 25.0), 0).next_phase, 0);
}

TEST(FixedTime, GreensClampedToPhaseLimitsAndPlansByTime) {
  auto ph = phases(2);
  ControllerConfig cfg;
  cfg.greens = {3, 500};  // outside [min_green, max_green]
  cfg.plans = {{600, {30, 10}}};
  FixedTimeController c(cfg);
  c.initialize({0, ph});
  EXPECT_DOUBLE_EQ(c.green_for(0, 0), 7);
  EXPECT_DOUBLE_EQ(c.green_for(1, 0), 60);
  EXPECT_DOUBLE_EQ(c.green_for(0, 700), 30);
  EXPECT_DOUBLE_EQ(c.green_for(1, 700), 10);
}

TEST(Adaptive, RestsInGreenWithoutCompetingDemand) {
  auto ph = phases(3);
  AdaptiveSignalController c(ControllerConfig{});
  auto o = obs(ph, 0, 30);
  EXPECT_FALSE(c.decide(o, 0).switch_phase);
}

TEST(Adaptive, GapsOutToBusiestPhaseAndSkipsEmptyOnes) {
  auto ph = phases(4);
  AdaptiveSignalController c(ControllerConfig{});
  auto o = obs(ph, 0, 10);
  o.phase_obs[2].queued = 6;  // phase 1 (index 1) empty, phase 2 busy
  o.phase_obs[3].queued = 2;
  auto d = c.decide(o, 0);
  EXPECT_TRUE(d.switch_phase);
  EXPECT_EQ(d.next_phase, 2);
}

TEST(Adaptive, RespectsMinGreen) {
  auto ph = phases(2);
  AdaptiveSignalController c(ControllerConfig{});
  auto o = obs(ph, 0, 3);
  o.phase_obs[1].queued = 20;
  EXPECT_FALSE(c.decide(o, 0).switch_phase);
}

TEST(Adaptive, ExtendsWhileDischargingUnlessRivalClearlyBetter) {
  auto ph = phases(2);
  AdaptiveSignalController c(ControllerConfig{});
  auto o = obs(ph, 0, 15);
  o.phase_obs[0].queued = 5;
  o.phase_obs[1].queued = 6;  // slightly more, but within hysteresis
  EXPECT_FALSE(c.decide(o, 0).switch_phase);
  o.phase_obs[1].queued = 20;
  EXPECT_TRUE(c.decide(o, 0).switch_phase);
}

TEST(Adaptive, StarvationGuardOverridesScores) {
  auto ph = phases(3);
  ControllerConfig cfg;
  cfg.max_wait = 60;
  AdaptiveSignalController c(cfg);
  auto o = obs(ph, 0, 20);
  o.phase_obs[0].queued = 15;
  o.phase_obs[1].queued = 10;
  o.phase_obs[2].queued = 1;
  o.phase_obs[2].oldest_wait = 75;
  auto d = c.decide(o, 0);
  EXPECT_TRUE(d.switch_phase);
  EXPECT_EQ(d.next_phase, 2);
  EXPECT_NE(c.last_reason().find("starvation"), std::string::npos);
}

TEST(Adaptive, EmergencyPriority) {
  auto ph = phases(3);
  AdaptiveSignalController c(ControllerConfig{});
  auto o = obs(ph, 0, 10);
  o.phase_obs[0].queued = 10;
  o.phase_obs[2].queued = 1;
  o.phase_obs[2].emergency = true;
  auto d = c.decide(o, 0);
  EXPECT_TRUE(d.switch_phase);
  EXPECT_EQ(d.next_phase, 2);

  ControllerConfig off;
  off.emergency_priority = false;
  AdaptiveSignalController c2(off);
  EXPECT_FALSE(c2.decide(o, 0).switch_phase);
}

TEST(Adaptive, PedestrianOnlyPhaseWaitsForStarvationWhenVehiclesCompete) {
  auto ph = phases(2, true);
  ControllerConfig cfg;
  cfg.max_pedestrian_wait = 90;
  AdaptiveSignalController c(cfg);
  auto o = obs(ph, 0, 10);  // current phase empty -> gap-out
  o.phase_obs[1].queued = 1;
  o.phase_obs[2].pedestrians_waiting = 8;
  o.phase_obs[2].oldest_pedestrian_wait = 40;
  EXPECT_EQ(c.decide(o, 0).next_phase, 1);
  o.phase_obs[2].oldest_pedestrian_wait = 95;
  EXPECT_EQ(c.decide(o, 0).next_phase, 2);
  o.phase_obs[1].queued = 0;  // no vehicles anywhere: pedestrians served immediately
  o.phase_obs[2].oldest_pedestrian_wait = 5;
  EXPECT_EQ(c.decide(o, 0).next_phase, 2);
}

TEST(MaxPressure, ServesHighestPressureAtDecisionPoints) {
  auto ph = phases(3);
  ControllerConfig cfg;
  cfg.decision_interval = 5;
  MaxPressureController c(cfg);
  c.initialize({0, ph});
  auto o = obs(ph, 0, 10);
  o.phase_obs[0].queued = 3;
  o.phase_obs[0].pressure = 3;
  o.phase_obs[1].queued = 9;
  o.phase_obs[1].pressure = 2;   // big queue but blocked downstream
  o.phase_obs[2].queued = 6;
  o.phase_obs[2].pressure = 6;
  auto d = c.decide(o, 100);
  EXPECT_TRUE(d.switch_phase);
  EXPECT_EQ(d.next_phase, 2);
  // Not another decision until the interval elapses.
  o.phase_obs[1].pressure = 50;
  EXPECT_FALSE(c.decide(o, 102).switch_phase);
  EXPECT_TRUE(c.decide(o, 105).switch_phase);
}

TEST(ControllerFactory, ValidatesConfig) {
  ControllerConfig bad;
  bad.type = "neural_net";
  EXPECT_FALSE(validate_controller_config(bad).empty());
  std::string err;
  EXPECT_EQ(make_controller(bad, &err), nullptr);
  EXPECT_FALSE(err.empty());
  ControllerConfig good;
  good.type = "adaptive";
  EXPECT_TRUE(validate_controller_config(good).empty());
  EXPECT_NE(make_controller(good), nullptr);
}
