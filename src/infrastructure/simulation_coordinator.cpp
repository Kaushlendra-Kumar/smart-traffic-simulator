#include "traffic/infrastructure/simulation_coordinator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include <nlohmann/json.hpp>

#include "traffic/infrastructure/snapshot_json.hpp"

namespace traffic {

using Clock = std::chrono::steady_clock;
using nlohmann::json;

SimulationCoordinator::SimulationCoordinator(Options options) : opt_(options), queue_(options.queue_capacity) {}

SimulationCoordinator::~SimulationCoordinator() { stop(); }

std::vector<std::string> SimulationCoordinator::load_initial(const ScenarioConfig& scenario) {
  auto errors = session_.load(scenario);
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
    bool network_changed = false;
    results.clear();
    for (auto& env : batch) {
      const auto previous = session_.state();
      auto outcome = session_.handle(env.command, env.scenario);
      if (session_.state() == SimulationSession::State::Running && previous != SimulationSession::State::Running)
        accumulator_ = 0.0;
      if (outcome.network_changed) {
        network_changed = true;
        accumulator_ = 0.0;
        replay_size_ = static_cast<std::uint64_t>(-1);
      }
      results.push_back(outcome.result);
      dirty = true;
    }
    if (dirty) {
      // Publish before acknowledging: once a client sees the ack, the frame/status it
      // reads already reflect the command.
      if (network_changed) publish_network();
      publish_frame();
      publish_metrics();
      publish_status();
      if (session_.engine().command_log().size() != replay_size_) publish_replay();
      for (std::size_t i = 0; i < batch.size(); ++i)
        if (batch[i].done) batch[i].done(results[i]);
    }

    const auto now = Clock::now();
    const bool running = session_.state() == SimulationSession::State::Running;
    if (running) {
      const double wall = std::chrono::duration<double>(now - last).count();
      const double speed = session_.speed();
      accumulator_ += wall * speed;
      const double dt = session_.engine().dt();
      auto steps = static_cast<long>(std::floor(accumulator_ / dt));
      // Never try to catch up more than ~250 ms of wall time in one go: if the engine
      // can't keep up we run slower than requested (flagged as lagging) instead of
      // spiralling. Physics are unaffected because every step uses the same dt.
      const long cap = std::max(1L, static_cast<long>(std::ceil(0.25 * speed / dt)));
      lagging_ = steps > cap;
      if (lagging_) {
        steps = cap;
        accumulator_ = 0.0;
      } else {
        accumulator_ -= static_cast<double>(steps) * dt;
      }
      const auto t0 = Clock::now();
      session_.advance(steps);
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

    if (running && now >= next_publish) {
      publish_frame();
      next_publish = now + std::chrono::duration_cast<Clock::duration>(frame_period);
    }
    if (now >= next_metrics) {
      publish_metrics();
      publish_status();
      if (session_.engine().command_log().size() != replay_size_) publish_replay();
      next_metrics = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(opt_.metrics_interval));
    }
    const auto wake = running ? std::min(next_publish, next_metrics) : next_metrics;
    queue_.wait_until(wake);
  }
}

void SimulationCoordinator::publish_frame() {
  const auto t0 = Clock::now();
  auto frame = std::make_shared<PublishedFrame>();
  frame->seq = ++seq_;
  frame->sim_time = session_.engine().time();
  frame->json = std::make_shared<const std::string>(session_.snapshot_json(frame->seq));
  frame->bytes = frame->json->size();
  frame->serialize_us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
  serialize_avg_us_ = serialize_avg_us_ == 0.0 ? frame->serialize_us : 0.9 * serialize_avg_us_ + 0.1 * frame->serialize_us;
  std::shared_ptr<const PublishedFrame> published = frame;
  frame_.store(published);
  if (on_frame_) on_frame_(published);
}

void SimulationCoordinator::publish_network() {
  auto s = std::make_shared<const std::string>(session_.network_json());
  network_.store(s);
  if (on_network_) on_network_(s);
}

void SimulationCoordinator::publish_metrics() {
  metrics_.store(std::make_shared<const std::string>(session_.metrics_json()));
}

void SimulationCoordinator::publish_replay() {
  replay_.store(std::make_shared<const std::string>(session_.replay_json()));
  replay_size_ = session_.engine().command_log().size();
}

void SimulationCoordinator::publish_status() {
  CoordinatorStatus s;
  const SimulationEngine& engine = session_.engine();
  s.state = session_.state_name();
  s.speed = session_.speed();
  s.sim_time = engine.time();
  s.step = engine.step_count();
  s.scenario_id = session_.base().id;
  s.scenario_name = session_.base().name;
  s.seed = engine.scenario().seed;
  s.controller = engine.controller_config().display_name();
  s.lagging = lagging_;
  s.queue_depth = queue_.size();
  s.queue_capacity = queue_.capacity();
  s.frames_published = seq_;
  if (auto f = frame_.load()) s.last_frame_bytes = f->bytes;
  s.avg_serialize_us = serialize_avg_us_;
  s.avg_step_us = step_avg_us_;
  s.realtime_factor = session_.state() == SimulationSession::State::Running ? realtime_factor_ : 0.0;
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
