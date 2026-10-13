#include "traffic/infrastructure/simulation_session.hpp"

#include <cmath>

#include <nlohmann/json.hpp>

#include "traffic/infrastructure/scenario_io.hpp"
#include "traffic/infrastructure/snapshot_json.hpp"

namespace traffic {

using nlohmann::json;

namespace {
template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
// Explicit deduction guide: older compilers (e.g. Clang 15) lack aggregate CTAD.
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;
}  // namespace

std::vector<std::string> SimulationSession::load(const ScenarioConfig& scenario) {
  base_ = scenario;
  settings_ = Settings{};
  state_ = State::Ready;
  return rebuild(base_, base_.seed, false);
}

const char* SimulationSession::state_name() const {
  switch (state_) {
    case State::Ready: return "ready";
    case State::Running: return "running";
    case State::Paused: return "paused";
  }
  return "ready";
}

void SimulationSession::advance(long steps) {
  for (long i = 0; i < steps; ++i) engine_->step();
}

SimulationSession::Outcome SimulationSession::handle(const ParsedCommand& command,
                                                     const std::shared_ptr<const ScenarioConfig>& scenario) {
  Outcome out;
  out.result = std::visit(overloaded{
                              [&](const ControlCommand& c) { return handle_control(c, scenario, out.network_changed); },
                              [&](const EngineCommand& c) {
                                CommandResult r = engine_->apply(c);
                                if (r.ok) remember_setting(c);
                                return r;
                              },
                          },
                          command);
  return out;
}

void SimulationSession::remember_setting(const EngineCommand& c) {
  // Settings survive "reset" (they are re-applied as step-0 commands); incidents do not.
  std::visit(overloaded{
                 [&](const CmdSetController& x) { settings_.controller = x.config; },
                 [&](const CmdSetDemand& x) { settings_.demand = x.multiplier; },
                 [&](const CmdSetPedestrianDemand& x) { settings_.pedestrian_demand = x.multiplier; },
                 [&](const CmdSetVehicleType& x) { settings_.types[x.type] = x.enabled; },
                 [&](const CmdSetSignalPlan& x) { settings_.greens[x.node] = x.greens; },
                 [](const auto&) {},
             },
             c);
}

CommandResult SimulationSession::handle_control(const ControlCommand& c,
                                                const std::shared_ptr<const ScenarioConfig>& scenario,
                                                bool& network_changed) {
  using K = ControlCommand::Kind;
  switch (c.kind) {
    case K::Start:
    case K::Resume:
      state_ = State::Running;
      return CommandResult::success();
    case K::Pause:
      if (state_ == State::Running) state_ = State::Paused;
      return CommandResult::success();
    case K::Step:
      if (state_ == State::Running) return CommandResult::fail("pause the simulation before single-stepping");
      advance(c.steps);
      state_ = State::Paused;
      return CommandResult::success();
    case K::SetSpeed:
      if (!(c.speed >= 0.1 && c.speed <= 50.0)) return CommandResult::fail("speed must be in [0.1, 50]");
      speed_ = c.speed;
      return CommandResult::success();
    case K::Reset: {
      auto errors = rebuild(base_, c.seed.value_or(engine_->scenario().seed), true);
      if (!errors.empty()) return CommandResult::fail(errors.front());
      state_ = State::Ready;
      network_changed = true;
      return CommandResult::success();
    }
    case K::Load: {
      if (!scenario) return CommandResult::fail("unknown scenario '" + c.scenario_id + "'");
      ScenarioConfig previous = base_;
      Settings previous_settings = settings_;
      base_ = *scenario;
      settings_ = Settings{};
      auto errors = rebuild(base_, c.seed.value_or(base_.seed), false);
      if (!errors.empty()) {
        base_ = previous;
        settings_ = previous_settings;
        rebuild(base_, base_.seed, true);
        return CommandResult::fail(errors.front());
      }
      state_ = State::Ready;
      network_changed = true;
      return CommandResult::success();
    }
    case K::Seek: {
      CommandResult r = seek(c.time);
      if (r.ok) network_changed = true;  // phases may have been edited before the seek point
      return r;
    }
  }
  return CommandResult::fail("unhandled command");
}

std::vector<std::string> SimulationSession::rebuild(const ScenarioConfig& base, std::uint64_t seed,
                                                    bool apply_settings) {
  ScenarioConfig cfg = base;
  cfg.seed = seed;
  auto created = SimulationEngine::create(cfg);
  if (!created.engine) return created.errors;
  engine_ = std::move(created.engine);
  if (apply_settings) {
    // Re-applied through the engine so they are part of the replayable command log.
    if (settings_.controller) engine_->apply(CmdSetController{*settings_.controller});
    if (settings_.demand) engine_->apply(CmdSetDemand{*settings_.demand});
    if (settings_.pedestrian_demand) engine_->apply(CmdSetPedestrianDemand{*settings_.pedestrian_demand});
    for (auto [type, enabled] : settings_.types) engine_->apply(CmdSetVehicleType{type, enabled});
    for (const auto& [node, greens] : settings_.greens) engine_->apply(CmdSetSignalPlan{node, greens});
  }
  return {};
}

CommandResult SimulationSession::seek(double time) {
  // Deterministic replay: rebuild from the same scenario and seed, re-apply the
  // logged commands at their original steps, then run to the target time.
  const std::vector<LoggedCommand> log = engine_->command_log();
  const auto target = static_cast<std::uint64_t>(std::llround(time / engine_->dt()));
  ScenarioConfig cfg = base_;
  cfg.seed = engine_->scenario().seed;
  auto created = SimulationEngine::create(cfg);
  if (!created.engine) return CommandResult::fail(created.errors.front());
  auto fresh = std::move(created.engine);
  for (const auto& entry : log) {
    if (entry.step > target) break;
    while (fresh->step_count() < entry.step) fresh->step();
    fresh->apply(entry.command);
  }
  while (fresh->step_count() < target) fresh->step();
  engine_ = std::move(fresh);
  state_ = State::Paused;
  return CommandResult::success();
}

std::string SimulationSession::network_json() const { return io::network_to_json(*engine_).dump(); }

std::string SimulationSession::snapshot_json(std::uint64_t seq) const {
  return io::snapshot_to_json(engine_->snapshot(), seq);
}

std::string SimulationSession::metrics_json() const {
  json j{{"v", io::kProtocolVersion},
         {"type", "metrics"},
         {"t", engine_->time()},
         {"summary", io::metrics_to_json(engine_->metrics())},
         {"intersections", io::intersections_to_json(engine_->intersection_stats())},
         {"series", io::series_to_json(engine_->series())}};
  return j.dump();
}

std::string SimulationSession::replay_json() const {
  json cmds = json::array();
  for (const auto& e : engine_->command_log())
    cmds.push_back({{"step", e.step}, {"command", engine_command_to_json(e.command)}});
  json j{{"scenario", io::scenario_to_json(base_)},
         {"seed", engine_->scenario().seed},
         {"dt", engine_->dt()},
         {"current_step", engine_->step_count()},
         {"state_hash", std::to_string(engine_->state_hash())},
         {"commands", cmds}};
  return j.dump();
}

}  // namespace traffic
