// traffic_cli — run the engine headless (no networking involved).
//
//   traffic_cli validate <scenario.json>
//   traffic_cli run      <scenario.json> [--controller TYPE] [--seed N] [--duration S] [--json]
//   traffic_cli compare  <scenario.json> [--controllers a,b,c] [--seeds N] [--base-seed S]
//                        [--duration S] [--threads T] [--csv FILE] [--json FILE]

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

#include "traffic/core/simulation_engine.hpp"
#include "traffic/experiments/experiment_runner.hpp"
#include "traffic/infrastructure/experiment_io.hpp"
#include "traffic/infrastructure/scenario_io.hpp"
#include "traffic/infrastructure/snapshot_json.hpp"

using namespace traffic;

namespace {

int usage() {
  std::cerr << "usage:\n"
               "  traffic_cli validate <scenario.json>\n"
               "  traffic_cli run <scenario.json> [--controller fixed_time|adaptive|max_pressure] [--seed N]\n"
               "                  [--duration S] [--json]\n"
               "  traffic_cli compare <scenario.json> [--controllers fixed_time,adaptive,max_pressure]\n"
               "                  [--seeds N] [--base-seed S] [--duration S] [--threads T] [--csv FILE] [--json FILE]\n";
  return 2;
}

std::map<std::string, std::string> parse_flags(int argc, char** argv, int from) {
  std::map<std::string, std::string> flags;
  for (int i = from; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("--", 0) != 0) continue;
    if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) flags[a.substr(2)] = argv[++i];
    else flags[a.substr(2)] = "1";
  }
  return flags;
}

std::optional<ScenarioConfig> load(const std::string& path) {
  auto parsed = io::scenario_from_file(path);
  if (!parsed.scenario) {
    for (const auto& e : parsed.errors) std::cerr << "error: " << e << "\n";
    return std::nullopt;
  }
  return parsed.scenario;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return usage();
  const std::string mode = argv[1];
  auto scenario = load(argv[2]);
  if (!scenario) return 1;
  auto flags = parse_flags(argc, argv, 3);

  if (mode == "validate") {
    auto created = SimulationEngine::create(*scenario);
    if (!created.engine) {
      for (const auto& e : created.errors) std::cerr << "error: " << e << "\n";
      return 1;
    }
    const auto& eng = *created.engine;
    std::cout << "OK: " << scenario->id << " — " << eng.network().nodes.size() << " nodes, " << eng.network().roads.size()
              << " roads, " << eng.network().movements.size() << " movements, " << eng.signals().size()
              << " signalised intersections\n";
    for (const auto& s : eng.signals()) {
      std::cout << "  node " << s.node() << ":\n";
      for (const auto& p : s.phases())
        std::cout << "    " << p.name << "  (min " << p.min_green << " s, max " << p.max_green << " s, walk " << p.walk_time << " s)\n";
    }
    return 0;
  }

  if (mode == "run") {
    if (flags.contains("controller")) scenario->controller.type = flags["controller"];
    if (flags.contains("seed")) scenario->seed = std::stoull(flags["seed"]);
    const double duration = flags.contains("duration") ? std::stod(flags["duration"]) : scenario->duration;
    auto created = SimulationEngine::create(*scenario);
    if (!created.engine) {
      for (const auto& e : created.errors) std::cerr << "error: " << e << "\n";
      return 1;
    }
    auto& eng = *created.engine;
    eng.run_until(duration);
    const auto m = eng.metrics();
    if (flags.contains("json")) {
      nlohmann::json j{{"scenario", scenario->id}, {"controller", scenario->controller.type}, {"seed", scenario->seed},
                       {"metrics", io::metrics_to_json(m)}, {"state_hash", std::to_string(eng.state_hash())}};
      std::cout << j.dump(2) << "\n";
      return 0;
    }
    std::printf("scenario=%s controller=%s seed=%llu duration=%.0fs (warm-up %.0fs)\n", scenario->id.c_str(),
                scenario->controller.type.c_str(), static_cast<unsigned long long>(scenario->seed), duration, scenario->warmup);
    std::printf("  trips: requested %ld, completed %ld (measured %ld), in network %ld, waiting to enter %ld, unserved %ld\n",
                m.spawned, m.completed, m.measured_completed, m.in_network, m.backlog, m.unserved);
    std::printf("  travel time: mean %.1f s, median %.1f s, p95 %s\n", m.avg_travel_time, m.median_travel_time,
                m.p95_travel_time ? (std::to_string(static_cast<int>(*m.p95_travel_time)) + " s").c_str() : "n/a");
    std::printf("  delay: %.1f s/veh completed, %.1f s/veh incl. unfinished, %.2f veh-h total\n", m.avg_delay,
                m.avg_delay_all, m.total_delay_hours);
    std::printf("  queue: %.1f veh avg (time-weighted), %d max approach; throughput %.0f veh/h\n", m.avg_queue, m.max_queue,
                m.throughput_vph);
    std::printf("  stops %.2f/veh, stopped %.1f s/veh; pedestrians %ld crossed, wait %.1f s avg\n", m.avg_stops,
                m.avg_stopped_time, m.peds_completed, m.ped_avg_wait);
    std::printf("  signals: %ld phase changes, %.1f%% clearance; safety guards %ld, overlap guards %ld\n", m.phase_switches,
                100 * m.clearance_fraction, m.red_light_guard_events, m.following_violations);
    std::printf("  state hash %llu\n", static_cast<unsigned long long>(eng.state_hash()));
    return 0;
  }

  if (mode == "compare") {
    nlohmann::json req;
    req["controllers"] = nlohmann::json::array();
    std::stringstream ss(flags.contains("controllers") ? flags["controllers"] : "fixed_time,adaptive,max_pressure");
    std::string item;
    while (std::getline(ss, item, ',')) {
      nlohmann::json c = io::controller_to_json(scenario->controller);
      c["type"] = item;
      c.erase("label");
      req["controllers"].push_back(c);
    }
    req["seed_count"] = flags.contains("seeds") ? std::stoi(flags["seeds"]) : 5;
    if (flags.contains("base-seed")) req["base_seed"] = std::stoull(flags["base-seed"]);
    if (flags.contains("duration")) req["duration"] = std::stod(flags["duration"]);
    if (flags.contains("threads")) req["threads"] = std::stoi(flags["threads"]);
    std::vector<std::string> errors;
    auto spec = io::experiment_spec_from_json(req, *scenario, errors);
    if (!spec) {
      for (const auto& e : errors) std::cerr << "error: " << e << "\n";
      return 1;
    }
    spec->id = "cli";
    spec->keep_series = false;
    auto result = ExperimentRunner::run(*spec, [](int done, int total) {
      std::fprintf(stderr, "\r  runs %d/%d", done, total);
      if (done == total) std::fprintf(stderr, "\n");
    });
    std::printf("\n%-28s", "metric");
    for (const auto& c : result.controllers) std::printf("%22s", c.c_str());
    std::printf("\n");
    for (const auto& def : comparison_metrics()) {
      std::printf("%-28s", (def.key + " [" + def.unit + "]").c_str());
      for (const auto& a : result.aggregates)
        for (const auto& m : a.metrics)
          if (m.key == def.key) std::printf("%14.2f ± %-5.2f", m.mean, m.ci95);
      std::printf("\n");
    }
    std::printf("\n(± = 95%% confidence half-width over %zu seeds; wall time %.1f s)\n\n", result.seeds.size(),
                result.wall_seconds);
    for (const auto& e : result.explanations) std::printf("* %s\n", e.c_str());
    if (flags.contains("csv")) {
      std::ofstream(flags["csv"]) << io::experiment_result_to_csv(result, *spec);
      std::printf("\nwrote %s\n", flags["csv"].c_str());
    }
    if (flags.contains("json")) {
      std::ofstream(flags["json"]) << io::experiment_result_to_json(result, false).dump(2);
      std::printf("wrote %s\n", flags["json"].c_str());
    }
    return 0;
  }
  return usage();
}
