// traffic_server — REST + WebSocket front-end for the simulation engine.
//
//   traffic_server [--config config/default.json] [--port N] [--host H]
//
// Environment overrides (for container platforms such as Render): PORT, HOST,
// EXPERIMENT_THREADS. Command-line flags take precedence over the environment, which
// takes precedence over the config file.
//                  [--scenario-dir DIR] [--static-dir DIR] [--scenario ID]

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>

#include <asio.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "api/experiment_manager.hpp"
#include "api/scenario_manager.hpp"
#include "api/server.hpp"
#include "traffic/infrastructure/simulation_coordinator.hpp"

#include <filesystem>

using namespace traffic;
using nlohmann::json;

int main(int argc, char** argv) {
  std::map<std::string, std::string> flags;
  for (int i = 1; i + 1 < argc; i += 2) flags[std::string(argv[i]).substr(2)] = argv[i + 1];
  if (argc % 2 == 0) {
    std::cerr << "usage: traffic_server [--config FILE] [--port N] [--host H] [--scenario-dir DIR] [--static-dir DIR] "
                 "[--scenario ID]\n";
    return 2;
  }

  json cfg = json::object();
  const std::string config_path = flags.contains("config") ? flags["config"] : "config/default.json";
  if (std::ifstream in(config_path); in) {
    cfg = json::parse(in, nullptr, false);
    if (cfg.is_discarded()) {
      spdlog::error("config event=invalid_json path={}", config_path);
      return 1;
    }
  } else if (flags.contains("config")) {
    spdlog::error("config event=missing path={}", config_path);
    return 1;
  }
  auto get = [&](const char* flag, const char* key, std::string fallback) {
    if (flags.contains(flag)) return flags[flag];
    if (cfg.contains(key) && cfg[key].is_string()) return cfg[key].get<std::string>();
    return fallback;
  };

  api::ServerConfig server_cfg;
  server_cfg.host = get("host", "host", "0.0.0.0");
  const char* env_port = std::getenv("PORT");
  const char* env_host = std::getenv("HOST");
  if (!flags.contains("host") && env_host && *env_host) server_cfg.host = env_host;
  server_cfg.port = static_cast<std::uint16_t>(flags.contains("port")       ? std::stoi(flags["port"])
                                               : env_port && *env_port ? std::stoi(env_port)
                                                                       : cfg.value("port", 8080));
  server_cfg.static_dir = get("static-dir", "static_dir", "frontend/dist");
  if (!std::filesystem::exists(server_cfg.static_dir)) {
    spdlog::warn("server event=no_frontend static_dir=\"{}\" (run `npm run build` in frontend/ or use the Vite dev server)",
                 server_cfg.static_dir);
    server_cfg.static_dir.clear();
  }
  const std::string scenario_dir = get("scenario-dir", "scenario_dir", "config/scenarios");
  const std::string default_scenario = get("scenario", "default_scenario", "single-intersection");

  api::ScenarioManager scenarios(scenario_dir);
  for (const auto& problem : scenarios.load_all()) spdlog::warn("scenario event=skipped detail=\"{}\"", problem);
  auto initial = scenarios.get(default_scenario);
  if (!initial) {
    auto all = scenarios.list();
    if (all.empty()) {
      spdlog::error("scenario event=none_loaded dir={}", scenario_dir);
      return 1;
    }
    spdlog::warn("scenario event=default_missing id={} using={}", default_scenario, all.front().id);
    initial = scenarios.get(all.front().id);
  }

  SimulationCoordinator::Options copt;
  copt.publish_hz = cfg.value("publish_hz", 20.0);
  copt.queue_capacity = cfg.value("command_queue_capacity", 256);
  SimulationCoordinator coordinator(copt);
  if (auto errors = coordinator.load_initial(*initial); !errors.empty()) {
    spdlog::error("scenario event=load_failed id={} error=\"{}\"", initial->id, errors.front());
    return 1;
  }
  int experiment_threads = cfg.value("experiment_threads", 0);
  if (const char* env_threads = std::getenv("EXPERIMENT_THREADS"); env_threads && *env_threads)
    experiment_threads = std::max(0, std::atoi(env_threads));
  api::ExperimentManager experiments(static_cast<std::size_t>(cfg.value("max_experiments", 20)), experiment_threads);
  api::Server server(server_cfg, coordinator, scenarios, experiments);
  coordinator.set_listeners([&](auto f) { server.on_frame(std::move(f)); }, [&](auto n) { server.on_network(std::move(n)); },
                            [&](auto s) { server.on_status(std::move(s)); });
  try {
    server.start();
  } catch (const std::exception& e) {
    spdlog::error("server event=bind_failed error=\"{}\"", e.what());
    return 1;
  }
  coordinator.start();
  spdlog::info("simulation event=ready scenario={} seed={}", initial->id, initial->seed);

  // Block until SIGINT/SIGTERM, then shut down in dependency order.
  asio::io_context signals_io;
  asio::signal_set signals(signals_io, SIGINT, SIGTERM);
  signals.async_wait([](std::error_code, int sig) { spdlog::info("server event=shutdown signal={}", sig); });
  signals_io.run();
  coordinator.stop();
  server.stop();
  return 0;
}
