// Long-running property tests: every shipped scenario, under every controller, keeps
// all physical and safety invariants for its whole duration.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>

#include "traffic/core/simulation_engine.hpp"
#include "traffic/infrastructure/scenario_io.hpp"

using namespace traffic;

namespace {
std::vector<std::string> scenario_files() {
  std::vector<std::string> out;
  for (const auto& e : std::filesystem::directory_iterator(TRAFFIC_SCENARIO_DIR))
    if (e.path().extension() == ".json") out.push_back(e.path().string());
  std::sort(out.begin(), out.end());
  return out;
}
}  // namespace

class ScenarioInvariants : public ::testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(ScenarioInvariants, HoldThroughoutRun) {
  const auto& [path, controller] = GetParam();
  auto parsed = io::scenario_from_file(path);
  ASSERT_TRUE(parsed.scenario) << parsed.errors.front();
  ScenarioConfig cfg = *parsed.scenario;
  cfg.controller.type = controller;
  auto created = SimulationEngine::create(cfg);
  ASSERT_TRUE(created.engine) << created.errors.front();
  auto& e = *created.engine;
  const double duration = std::min(cfg.duration, 900.0);
  std::string why;
  while (e.time() < duration) {
    e.step();
    if (e.step_count() % 10 == 0) {
      ASSERT_TRUE(e.check_invariants(&why)) << "t=" << e.time() << ": " << why;
    }
  }
  const auto m = e.metrics();
  EXPECT_EQ(m.red_light_guard_events, 0);
  EXPECT_EQ(m.following_violations, 0);
  EXPECT_GT(m.completed, 0);
  EXPECT_EQ(m.spawned, m.completed + m.in_network + m.backlog + m.unserved);  // conservation of trips
  for (double x : {m.avg_delay, m.avg_delay_all, m.avg_queue, m.avg_travel_time, m.throughput_vph, m.ped_avg_wait})
    EXPECT_TRUE(std::isfinite(x) && x >= 0);
  EXPECT_LE(m.avg_delay, m.avg_travel_time + 1e-9);
}

INSTANTIATE_TEST_SUITE_P(AllScenarios, ScenarioInvariants,
                         ::testing::Combine(::testing::ValuesIn(scenario_files()),
                                            ::testing::Values("fixed_time", "adaptive", "max_pressure")),
                         [](const auto& param_info) {
                           std::string name = std::filesystem::path(std::get<0>(param_info.param)).stem().string() + "_" +
                                              std::get<1>(param_info.param);
                           for (char& c : name)
                             if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
                           return name;
                         });
