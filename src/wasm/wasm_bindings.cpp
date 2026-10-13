// WebAssembly entry points (Emscripten embind). The browser runs the same engine,
// session logic and JSON protocol as the native server, inside a Web Worker:
//
//   LiveSim        — one live simulation (SimulationSession); JS paces the steps.
//   ExperimentJob  — a paired multi-seed experiment executed one run at a time so the
//                    worker can report progress between runs.
//
// All data crosses the boundary as JSON strings in protocol v1 (see
// docs/api-specification.md), so the frontend code path is identical to server mode.

#include <emscripten/bind.h>

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "traffic/experiments/experiment_runner.hpp"
#include "traffic/infrastructure/command_parser.hpp"
#include "traffic/infrastructure/experiment_io.hpp"
#include "traffic/infrastructure/scenario_io.hpp"
#include "traffic/infrastructure/simulation_session.hpp"

using nlohmann::json;
using namespace traffic;

namespace {

std::string errors_json(const std::vector<std::string>& errors) { return json(errors).dump(); }

class LiveSim {
 public:
  /// Returns a JSON array of errors (empty on success).
  std::string load(const std::string& scenario_text) {
    auto parsed = io::scenario_from_string(scenario_text);
    if (!parsed.scenario) return errors_json(parsed.errors);
    return errors_json(session_.load(*parsed.scenario));
  }

  /// Applies a command (protocol JSON). `scenario_text` is required for load_scenario.
  /// Returns {"ok":bool, "code"?, "error"?, "network_changed":bool}.
  std::string command(const std::string& command_text, const std::string& scenario_text) {
    json cmd = json::parse(command_text, nullptr, false);
    if (cmd.is_discarded()) return json{{"ok", false}, {"code", "bad_json"}, {"error", "command is not valid JSON"}}.dump();
    auto parsed = parse_command(cmd);
    if (!parsed.command)
      return json{{"ok", false}, {"code", "invalid_command"}, {"error", parsed.error}}.dump();
    std::shared_ptr<const ScenarioConfig> scenario;
    if (!scenario_text.empty()) {
      auto ps = io::scenario_from_string(scenario_text);
      if (!ps.scenario)
        return json{{"ok", false}, {"code", "invalid_scenario"}, {"error", ps.errors.front()}}.dump();
      scenario = std::make_shared<const ScenarioConfig>(std::move(*ps.scenario));
    }
    auto outcome = session_.handle(*parsed.command, scenario);
    json out{{"ok", outcome.result.ok}, {"network_changed", outcome.network_changed}};
    if (!outcome.result.ok) {
      out["code"] = "rejected";
      out["error"] = outcome.result.error;
    }
    return out.dump();
  }

  void advance(int steps) { session_.advance(steps); }
  std::string network() const { return session_.network_json(); }
  std::string snapshot(double seq) const { return session_.snapshot_json(static_cast<std::uint64_t>(seq)); }
  std::string metrics() const { return session_.metrics_json(); }
  std::string replay() const { return session_.replay_json(); }

  std::string state() const { return session_.state_name(); }
  double speed() const { return session_.speed(); }
  double dt() const { return session_.engine().dt(); }
  double time() const { return session_.engine().time(); }
  double step() const { return static_cast<double>(session_.engine().step_count()); }
  std::string scenarioId() const { return session_.base().id; }
  std::string scenarioName() const { return session_.base().name; }
  double seed() const { return static_cast<double>(session_.engine().scenario().seed); }
  std::string controller() const { return session_.engine().controller_config().display_name(); }

 private:
  SimulationSession session_;
};

class ExperimentJob {
 public:
  /// `request_text` is the REST experiment body (controllers, seeds/seed_count, duration).
  /// Returns a JSON array of validation errors (empty on success).
  std::string start(const std::string& request_text, const std::string& scenario_text, const std::string& id) {
    auto ps = io::scenario_from_string(scenario_text);
    if (!ps.scenario) return errors_json(ps.errors);
    json req = json::parse(request_text, nullptr, false);
    if (req.is_discarded()) return errors_json({"experiment request is not valid JSON"});
    req.erase("scenario");
    req.erase("threads");  // the browser build runs one run at a time
    std::vector<std::string> errors;
    auto spec = io::experiment_spec_from_json(req, *ps.scenario, errors);
    if (!spec) return errors_json(errors);
    spec_ = std::move(*spec);
    spec_.id = id;
    result_ = ExperimentResult{};
    result_.id = id;
    result_.scenario_id = spec_.scenario.id;
    result_.duration = spec_.duration;
    result_.seeds = spec_.seeds;
    for (const auto& c : spec_.controllers) result_.controllers.push_back(c.display_name());
    result_.runs.clear();
    next_ = 0;
    return "[]";
  }

  int total() const { return static_cast<int>(spec_.controllers.size() * spec_.seeds.size()); }
  int done() const { return static_cast<int>(next_); }
  bool finished() const { return static_cast<int>(next_) >= total(); }

  /// Executes the next run (same ordering as ExperimentRunner: controller-major).
  void runNext() {
    if (finished()) return;
    const std::size_t ci = next_ / spec_.seeds.size();
    const std::size_t si = next_ % spec_.seeds.size();
    RunResult r = ExperimentRunner::run_single(spec_.scenario, spec_.controllers[ci], spec_.seeds[si], spec_.duration,
                                               spec_.keep_series);
    r.controller_index = ci;
    result_.wall_seconds += r.wall_seconds;
    result_.runs.push_back(std::move(r));
    ++next_;
    if (finished()) ExperimentRunner::analyse(result_);
  }

  std::string resultJson() const { return io::experiment_result_to_json(result_, true).dump(); }
  std::string resultCsv() const { return io::experiment_result_to_csv(result_, spec_); }

 private:
  ExperimentSpec spec_;
  ExperimentResult result_;
  std::size_t next_ = 0;
};

/// Validates a scenario without building a session: returns a JSON array of errors.
std::string validateScenario(const std::string& scenario_text) {
  auto ps = io::scenario_from_string(scenario_text);
  if (!ps.scenario) return errors_json(ps.errors);
  auto created = SimulationEngine::create(*ps.scenario);
  return errors_json(created.errors);
}

}  // namespace

EMSCRIPTEN_BINDINGS(traffic_engine) {
  emscripten::class_<LiveSim>("LiveSim")
      .constructor<>()
      .function("load", &LiveSim::load)
      .function("command", &LiveSim::command)
      .function("advance", &LiveSim::advance)
      .function("network", &LiveSim::network)
      .function("snapshot", &LiveSim::snapshot)
      .function("metrics", &LiveSim::metrics)
      .function("replay", &LiveSim::replay)
      .function("state", &LiveSim::state)
      .function("speed", &LiveSim::speed)
      .function("dt", &LiveSim::dt)
      .function("time", &LiveSim::time)
      .function("step", &LiveSim::step)
      .function("scenarioId", &LiveSim::scenarioId)
      .function("scenarioName", &LiveSim::scenarioName)
      .function("seed", &LiveSim::seed)
      .function("controller", &LiveSim::controller);
  emscripten::class_<ExperimentJob>("ExperimentJob")
      .constructor<>()
      .function("start", &ExperimentJob::start)
      .function("total", &ExperimentJob::total)
      .function("done", &ExperimentJob::done)
      .function("finished", &ExperimentJob::finished)
      .function("runNext", &ExperimentJob::runNext)
      .function("resultJson", &ExperimentJob::resultJson)
      .function("resultCsv", &ExperimentJob::resultCsv);
  emscripten::function("validateScenario", &validateScenario);
}
