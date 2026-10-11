#include "traffic/infrastructure/simulation_coordinator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include <nlohmann/json.hpp>

#include "traffic/infrastructure/scenario_io.hpp"
#include "traffic/infrastructure/snapshot_json.hpp"

namespace traffic {

using Clock = std::chrono::steady_clock;
using nlohmann::json;

namespace {
template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
const char* state_name(int s) {
  switch (s) {
    case 0: return "ready";
    case 1: return "running";
    case 2: return "paused";
  }
  return "ready";
}
}  // namespace

SimulationCoordinator::SimulationCoordinator(Options options) : opt_(options), queue_(options.queue_capacity) {}

SimulationCoordinator::~SimulationCoordinator() { stop(); }

std::vector<std::string> SimulationCoordinator::load_initial(const ScenarioConfig& scenario) {
  base_ = scenario;
  auto errors = rebuild(base_, base_.seed, false);
  if (errors.empty()) {
    publish_network();
    publish_frame();
    publish_metrics();
    publish_replay();
    publish_status();
  }
  return errors;
}

void SimulationCoordinator::set_listeners(FrameListener on_frame, JsonListener on_network, JsonListener on_status) {
  on_frame_ = std::move(on_frame);
  on_network_ = std::move(on_network);
  on_status_ = std::move(on_status);
}

void SimulationCoordinator::start() {
  if (thread_.joinable()) return;
  stop_ = false;
  thread_ = std::thread([this] { loop(); });
}

void SimulationCoordinator::stop() {
  stop_ = true;
  queue_.wake();
  if (thread_.joinable()) thread_.join();
}

bool SimulationCoordinator::submit(ParsedCommand command, std::shared_ptr<const ScenarioConfig> scenario,
                                   CommandCallback done) {
  return queue_.push(Envelope{std::move(command), std::move(scenario), std::move(done)});
}

CoordinatorStatus SimulationCoordinator::status() const {
  std::lock_guard lock(status_mu_);
  CoordinatorStatus s = status_;
  s.queue_depth = queue_.size();
  return s;
}

// ------------------------------------------------------------------ simulation thread

void SimulationCoordinator::loop() {
  auto last = Clock::now();
  auto next_publish = last;
  auto next_metrics = last;
  auto rate_window_start = last;
  double rate_window_sim = 0.0;
  std::vector<Envelope> batch;
  std::vector<CommandResult> results;
  const auto frame_period = std::chrono::duration<double>(1.0 / std::max(opt_.publish_hz, 1.0));

  while (!stop_) {
    batch.clear();
    queue_.drain(batch);
    bool dirty = false;
    results.clear();
    for (auto& env : batch) {
      results.push_back(handle(env));
      dirty = true;
    }
    if (dirty) {
      // Publish before acknowledging: once a client sees the ack, the frame/status it
      // reads already reflect the command.
      publish_frame();
      publish_metrics();
      publish_status();
      if (engine_->command_log().size() != replay_size_) publish_replay();
      for (std::size_t i = 0; i < batch.size(); ++i)
        if (batch[i].done) batch[i].done(results[i]);
    }

    const auto now = Clock::now();
    if (state_ == State::Running) {
      const double wall = std::chrono::duration<double>(now - last).count();
      accumulator_ += wall * speed_;
      const double dt = engine_->dt();
      auto steps = static_cast<long>(std::floor(accumulator_ / dt));
      // Never try to catch up more than ~250 ms of wall time in one go: if the engine
      // can't keep up we run slower than requested (flagged as lagging) instead of
      // spiralling. Physics are unaffected because every step uses the same dt.
      const long cap = std::max(1L, static_cast<long>(std::ceil(0.25 * speed_ / dt)));
      lagging_ = steps > cap;
      if (lagging_) {
        steps = cap;
        accumulator_ = 0.0;
      } else {
        accumulator_ -= static_cast<double>(steps) * dt;
      }
      const auto t0 = Clock::now();
      for (long i = 0; i < steps; ++i) engine_->step();
      if (steps > 0) {
        const double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / static_cast<double>(steps);
        step_avg_us_ = step_avg_us_ == 0.0 ? us : 0.9 * step_avg_us_ + 0.1 * us;
        rate_window_sim += static_cast<double>(steps) * dt;
      }
    }
    last = now;
    const double window = std::chrono::duration<double>(now - rate_window_start).count();
    if (window >= 1.0) {
      realtime_factor_ = rate_window_sim / window;
      rate_window_sim = 0.0;
      rate_window_start = now;
    }

    if (state_ == State::Running && now >= next_publish) {
      publish_frame();
      next_publish = now + std::chrono::duration_cast<Clock::duration>(frame_period);
    }
    if (now >= next_metrics) {
      publish_metrics();
      publish_status();
      if (engine_->command_log().size() != replay_size_) publish_replay();
      next_metrics = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(opt_.metrics_interval));
    }
    const auto wake = state_ == State::Running ? std::min(next_publish, next_metrics) : next_metrics;
    queue_.wait_until(wake);
  }
}

CommandResult SimulationCoordinator::handle(Envelope& env) {
  return std::visit(overloaded{
                        [&](const ControlCommand& c) { return handle_control(c, env.scenario); },
                        [&](const EngineCommand& c) {
                          CommandResult r = engine_->apply(c);
                          if (r.ok) remember_setting(c);
                          return r;
                        },
                    },
                    env.command);
}

void SimulationCoordinator::remember_setting(const EngineCommand& c) {
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

CommandResult SimulationCoordinator::handle_control(const ControlCommand& c,
                                                    const std::shared_ptr<const ScenarioConfig>& scenario) {
  using K = ControlCommand::Kind;
  switch (c.kind) {
    case K::Start:
    case K::Resume:
      state_ = State::Running;
      accumulator_ = 0.0;
      return CommandResult::success();
    case K::Pause:
      if (state_ == State::Running) state_ = State::Paused;
      return CommandResult::success();
    case K::Step:
      if (state_ == State::Running) return CommandResult::fail("pause the simulation before single-stepping");
      for (int i = 0; i < c.steps; ++i) engine_->step();
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
      publish_network();
      return CommandResult::success();
    }
    case K::Load: {
      if (!scenario) return CommandResult::fail("unknown scenario '" + c.scenario_id + "'");
      ScenarioConfig previous = base_;
      base_ = *scenario;
      settings_ = Settings{};
      auto errors = rebuild(base_, c.seed.value_or(base_.seed), false);
      if (!errors.empty()) {
        base_ = previous;
        rebuild(base_, base_.seed, false);
        return CommandResult::fail(errors.front());
      }
      state_ = State::Ready;
      publish_network();
      return CommandResult::success();
    }
    case K::Seek:
      return seek(c.time);
  }
  return CommandResult::fail("unhandled command");
}

std::vector<std::string> SimulationCoordinator::rebuild(const ScenarioConfig& base, std::uint64_t seed,
                                                        bool apply_settings) {
  ScenarioConfig cfg = base;
  cfg.seed = seed;
  auto created = SimulationEngine::create(cfg);
  if (!created.engine) return created.errors;
  engine_ = std::move(created.engine);
  accumulator_ = 0.0;
  replay_size_ = static_cast<std::uint64_t>(-1);
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

CommandResult SimulationCoordinator::seek(double time) {
  // Deterministic replay: rebuild from the same scenario and seed, re-apply the
  // logged commands at their original steps, then run to the target time.
  const std::vector<LoggedCommand> log = engine_->command_log();
  const std::uint64_t target = static_cast<std::uint64_t>(std::llround(time / engine_->dt()));
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
  accumulator_ = 0.0;
  replay_size_ = static_cast<std::uint64_t>(-1);
  return CommandResult::success();
}

void SimulationCoordinator::publish_frame() {
  const auto t0 = Clock::now();
  const SimulationSnapshot snap = engine_->snapshot();
  auto frame = std::make_shared<PublishedFrame>();
  frame->seq = ++seq_;
  frame->sim_time = snap.time;
  frame->json = std::make_shared<const std::string>(io::snapshot_to_json(snap, frame->seq));
  frame->bytes = frame->json->size();
  frame->serialize_us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
  serialize_avg_us_ = serialize_avg_us_ == 0.0 ? frame->serialize_us : 0.9 * serialize_avg_us_ + 0.1 * frame->serialize_us;
  std::shared_ptr<const PublishedFrame> published = frame;
  frame_.store(published);
  if (on_frame_) on_frame_(published);
}

void SimulationCoordinator::publish_network() {
  auto j = io::network_to_json(*engine_);
  auto s = std::make_shared<const std::string>(j.dump());
  network_.store(s);
  if (on_network_) on_network_(s);
}

void SimulationCoordinator::publish_metrics() {
  json j{{"v", io::kProtocolVersion},
         {"type", "metrics"},
         {"t", engine_->time()},
         {"summary", io::metrics_to_json(engine_->metrics())},
         {"intersections", io::intersections_to_json(engine_->intersection_stats())},
         {"series", io::series_to_json(engine_->series())}};
  metrics_.store(std::make_shared<const std::string>(j.dump()));
}

void SimulationCoordinator::publish_replay() {
  json cmds = json::array();
  for (const auto& e : engine_->command_log()) cmds.push_back({{"step", e.step}, {"command", engine_command_to_json(e.command)}});
  json j{{"scenario", io::scenario_to_json(base_)},
         {"seed", engine_->scenario().seed},
         {"dt", engine_->dt()},
         {"current_step", engine_->step_count()},
         {"state_hash", std::to_string(engine_->state_hash())},
         {"commands", cmds}};
  replay_.store(std::make_shared<const std::string>(j.dump()));
  replay_size_ = engine_->command_log().size();
}

void SimulationCoordinator::publish_status() {
  CoordinatorStatus s;
  s.state = state_name(static_cast<int>(state_));
  s.speed = speed_;
  s.sim_time = engine_->time();
  s.step = engine_->step_count();
  s.scenario_id = base_.id;
  s.scenario_name = base_.name;
  s.seed = engine_->scenario().seed;
  s.controller = engine_->controller_config().display_name();
  s.lagging = lagging_;
  s.queue_depth = queue_.size();
  s.queue_capacity = queue_.capacity();
  s.frames_published = seq_;
  if (auto f = frame_.load()) s.last_frame_bytes = f->bytes;
  s.avg_serialize_us = serialize_avg_us_;
  s.avg_step_us = step_avg_us_;
  s.realtime_factor = state_ == State::Running ? realtime_factor_ : 0.0;
  {
    std::lock_guard lock(status_mu_);
    status_ = s;
  }
  json j{{"v", io::kProtocolVersion}, {"type", "status"},       {"state", s.state},
         {"speed", s.speed},          {"sim_time", s.sim_time}, {"step", s.step},
         {"scenario", s.scenario_id}, {"scenario_name", s.scenario_name},
         {"seed", s.seed},            {"controller", s.controller},
         {"lagging", s.lagging},      {"queue_depth", s.queue_depth},
         {"queue_capacity", s.queue_capacity},
         {"frames_published", s.frames_published},
         {"last_frame_bytes", s.last_frame_bytes},
         {"avg_serialize_us", std::round(s.avg_serialize_us)},
         {"avg_step_us", std::round(s.avg_step_us * 10) / 10},
         {"realtime_factor", std::round(s.realtime_factor * 100) / 100}};
  auto js = std::make_shared<const std::string>(j.dump());
  status_json_.store(js);
  if (on_status_) on_status_(js);
}

}  // namespace traffic
