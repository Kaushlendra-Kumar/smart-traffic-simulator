#pragma once
// SimulationSession: the single-threaded "live simulation" logic shared by the native
// server (driven by SimulationCoordinator's thread) and the browser build (driven by a
// Web Worker running the engine compiled to WebAssembly).
//
// It owns the engine and the scenario it was built from, applies validated commands,
// remembers settings that survive "reset" (controller, demand, vehicle types, signal
// plans — not incidents), and serialises network/snapshot/metrics/replay JSON.
// It knows nothing about threads, clocks or sockets: callers decide when to step.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "traffic/core/simulation_engine.hpp"
#include "traffic/infrastructure/command_parser.hpp"

namespace traffic {

class SimulationSession {
 public:
  enum class State { Ready, Running, Paused };

  struct Outcome {
    CommandResult result;
    bool network_changed = false;  // geometry/phases changed: clients need a new network message
  };

  /// Builds the first engine. Returns validation errors (empty on success).
  std::vector<std::string> load(const ScenarioConfig& scenario);

  /// Applies a parsed command. `scenario` is required for Load commands.
  Outcome handle(const ParsedCommand& command, const std::shared_ptr<const ScenarioConfig>& scenario);

  /// Advances `steps` fixed steps (used while running; pacing is the caller's job).
  void advance(long steps);

  [[nodiscard]] State state() const { return state_; }
  [[nodiscard]] const char* state_name() const;
  [[nodiscard]] double speed() const { return speed_; }
  [[nodiscard]] const ScenarioConfig& base() const { return base_; }
  [[nodiscard]] SimulationEngine& engine() { return *engine_; }
  [[nodiscard]] const SimulationEngine& engine() const { return *engine_; }
  [[nodiscard]] bool loaded() const { return engine_ != nullptr; }

  [[nodiscard]] std::string network_json() const;
  [[nodiscard]] std::string snapshot_json(std::uint64_t seq) const;
  [[nodiscard]] std::string metrics_json() const;
  [[nodiscard]] std::string replay_json() const;

 private:
  CommandResult handle_control(const ControlCommand& c, const std::shared_ptr<const ScenarioConfig>& scenario,
                               bool& network_changed);
  std::vector<std::string> rebuild(const ScenarioConfig& base, std::uint64_t seed, bool apply_settings);
  CommandResult seek(double time);
  void remember_setting(const EngineCommand& c);

  std::unique_ptr<SimulationEngine> engine_;
  ScenarioConfig base_;
  struct Settings {
    std::optional<ControllerConfig> controller;
    std::optional<double> demand, pedestrian_demand;
    std::map<VehicleType, bool> types;
    std::map<NodeId, std::vector<double>> greens;
  } settings_;
  State state_ = State::Ready;
  double speed_ = 1.0;
};

}  // namespace traffic
