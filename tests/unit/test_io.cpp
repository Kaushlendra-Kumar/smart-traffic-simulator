#include <gtest/gtest.h>

#include <filesystem>

#include "test_util.hpp"
#include "traffic/infrastructure/command_parser.hpp"
#include "traffic/infrastructure/experiment_io.hpp"
#include "traffic/infrastructure/scenario_io.hpp"
#include "traffic/infrastructure/snapshot_json.hpp"

using namespace traffic;
using nlohmann::json;

namespace {
bool contains(const std::vector<std::string>& errs, const std::string& needle) {
  for (const auto& e : errs)
    if (e.find(needle) != std::string::npos) return true;
  return false;
}
}  // namespace

TEST(ScenarioIo, RoundTripPreservesConfiguration) {
  ScenarioConfig s = test::single_intersection(410, 3);
  s.id = "round-trip";
  s.demand.profile = {{0, 0.5}, {600, 1.5}};
  s.demand.sources = {{1, 700, {{3, 2.0}, {2, 1.0}}}};
  s.controller.type = "adaptive";
  s.controller.node_greens[0] = {12, 14, 16, 18};
  s.events = {{120, "close_road", 1, 0, 0, 0, 1}};
  s.signals.phasing = "protected";
  const json j = io::scenario_to_json(s);
  auto parsed = io::scenario_from_json(j);
  ASSERT_TRUE(parsed.scenario.has_value()) << parsed.errors.front();
  EXPECT_EQ(io::scenario_to_json(*parsed.scenario), j);
  EXPECT_EQ(parsed.scenario->controller.node_greens.at(0).size(), 4u);
}

TEST(ScenarioIo, ReportsUnknownKeysWithPath) {
  json j = io::scenario_to_json(test::single_intersection());
  j["demand"]["sorces"] = json::array();
  auto parsed = io::scenario_from_json(j);
  EXPECT_FALSE(parsed.scenario.has_value());
  EXPECT_TRUE(contains(parsed.errors, "demand.sorces: unknown key"));
}

TEST(ScenarioIo, ReportsTypeAndRangeErrors) {
  json j = io::scenario_to_json(test::single_intersection());
  j["dt"] = "fast";
  j["network"]["roads"][0]["lanes"] = 9;
  j["controller"]["type"] = "magic";
  auto parsed = io::scenario_from_json(j);
  EXPECT_TRUE(contains(parsed.errors, "dt: expected a number"));
  EXPECT_TRUE(contains(parsed.errors, "network.roads[0].lanes: must be in [1, 5]"));
  EXPECT_TRUE(contains(parsed.errors, "controller.type"));
  EXPECT_FALSE(io::scenario_from_string("{not json").scenario.has_value());
}

TEST(ScenarioIo, AllShippedScenariosParseAndBuild) {
  int count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(TRAFFIC_SCENARIO_DIR)) {
    if (entry.path().extension() != ".json") continue;
    ++count;
    auto parsed = io::scenario_from_file(entry.path().string());
    ASSERT_TRUE(parsed.scenario.has_value()) << entry.path() << ": " << parsed.errors.front();
    auto created = SimulationEngine::create(*parsed.scenario);
    ASSERT_NE(created.engine, nullptr) << entry.path() << ": " << created.errors.front();
    EXPECT_EQ(parsed.scenario->id, entry.path().stem().string());
  }
  EXPECT_GE(count, 7);
}

TEST(CommandParser, ValidCommands) {
  auto r = parse_command(json{{"cmd", "set_demand"}, {"multiplier", 1.5}});
  ASSERT_TRUE(r.command.has_value()) << r.error;
  auto& ec = std::get<EngineCommand>(*r.command);
  EXPECT_DOUBLE_EQ(std::get<CmdSetDemand>(ec).multiplier, 1.5);

  r = parse_command(json{{"cmd", "step"}, {"count", 10}});
  ASSERT_TRUE(r.command.has_value());
  EXPECT_EQ(std::get<ControlCommand>(*r.command).steps, 10);

  r = parse_command(json{{"cmd", "set_controller"}, {"controller", {{"type", "max_pressure"}, {"decision_interval", 4}}}});
  ASSERT_TRUE(r.command.has_value()) << r.error;
  EXPECT_EQ(std::get<CmdSetController>(std::get<EngineCommand>(*r.command)).config.decision_interval, 4);
}

TEST(CommandParser, RejectsMalformedCommands) {
  EXPECT_FALSE(parse_command(json::array()).command.has_value());
  EXPECT_FALSE(parse_command(json{{"cmd", "launch_rockets"}}).command.has_value());
  EXPECT_FALSE(parse_command(json{{"cmd", "set_demand"}}).command.has_value());
  EXPECT_FALSE(parse_command(json{{"cmd", "set_demand"}, {"multiplier", 99}}).command.has_value());
  EXPECT_FALSE(parse_command(json{{"cmd", "set_speed"}, {"speed", "fast"}}).command.has_value());
  EXPECT_FALSE(parse_command(json{{"cmd", "close_road"}, {"road", -1}}).command.has_value());
  EXPECT_FALSE(parse_command(json{{"cmd", "spawn_vehicle"}, {"type", "tank"}, {"origin", 1}, {"destination", 3}}).command.has_value());
  EXPECT_FALSE(parse_command(json{{"cmd", "set_controller"}, {"controller", {{"type", "adaptive"}, {"w_queue", -1}}}}).command.has_value());
}

TEST(CommandParser, EngineCommandJsonRoundTrip) {
  const std::vector<EngineCommand> cmds{CmdSetDemand{2}, CmdBlockLane{3, 1, 40}, CmdSpawnVehicle{VehicleType::Bus, 1, 3},
                                        CmdSetSignalPlan{0, {10, 20}}, CmdSetVehicleType{VehicleType::Emergency, false}};
  for (const auto& c : cmds) {
    auto r = parse_command(engine_command_to_json(c));
    ASSERT_TRUE(r.command.has_value()) << r.error;
    EXPECT_EQ(command_name(std::get<EngineCommand>(*r.command)), command_name(c));
    EXPECT_EQ(engine_command_to_json(std::get<EngineCommand>(*r.command)), engine_command_to_json(c));
  }
}

TEST(SnapshotJson, CompactAndComplete) {
  auto e = test::make(test::single_intersection(400));
  e->run_for(120);
  const auto snap = e->snapshot();
  const json j = json::parse(io::snapshot_to_json(snap, 7));
  EXPECT_EQ(j["v"], io::kProtocolVersion);
  EXPECT_EQ(j["seq"], 7);
  EXPECT_EQ(j["veh"].size(), snap.vehicles.size());
  ASSERT_FALSE(j["veh"].empty());
  EXPECT_EQ(j["veh"][0].size(), 7u);
  EXPECT_EQ(j["sig"][0]["mv"].get<std::string>().size(), 12u);
  const json net = io::network_to_json(*e);
  EXPECT_EQ(net["roads"].size(), 8u);
  EXPECT_EQ(net["movements"].size(), 12u);
}

TEST(ExperimentIo, SpecValidationAndCsv) {
  auto scenario = test::single_intersection(200);
  scenario.warmup = 60;
  std::vector<std::string> errors;
  auto spec = io::experiment_spec_from_json(json{{"controllers", {{{"type", "fixed_time"}}, {{"type", "fixed_time"}}}},
                                                 {"seed_count", 2},
                                                 {"duration", 300}},
                                            scenario, errors);
  ASSERT_TRUE(spec.has_value()) << errors.front();
  EXPECT_NE(spec->controllers[0].display_name(), spec->controllers[1].display_name());  // labels made unique
  spec->id = "t";
  auto result = ExperimentRunner::run(*spec);
  const std::string csv = io::experiment_result_to_csv(result, *spec);
  EXPECT_NE(csv.find("avg_delay_all_s"), std::string::npos);
  EXPECT_NE(csv.find("throughput_vph_veh_per_h"), std::string::npos);
  EXPECT_NE(csv.find("controller_config_json"), std::string::npos);
  EXPECT_EQ(std::count(csv.begin(), csv.end(), '\n'), 1 + 4);

  errors.clear();
  EXPECT_FALSE(io::experiment_spec_from_json(json{{"controllers", json::array()}, {"duration", 5}, {"oops", 1}}, scenario, errors));
  EXPECT_TRUE(contains(errors, "oops: unknown key"));
  EXPECT_TRUE(contains(errors, "duration"));
}
