#pragma once
// SimulationCoordinator owns the one simulation thread. It is the only code that
// advances simulation time or mutates the engine. Other threads interact with it
// exclusively through:
//   * submit(): a bounded, validated command queue (MPSC, FIFO, never blocks);
//   * immutable published artefacts (frames, network/metrics/replay JSON) exchanged
//     through atomic shared_ptr swaps — readers can hold an old frame safely while the
//     simulation thread publishes a new one.
// Real-time pacing uses an accumulator: wall time * speed is converted into whole
// fixed steps, so slow consumers can delay *when* steps run but never change *what*
// they compute.

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "traffic/core/simulation_engine.hpp"
#include "traffic/infrastructure/command_parser.hpp"
#include "traffic/infrastructure/command_queue.hpp"

namespace traffic {

struct PublishedFrame {
  std::uint64_t seq = 0;
  double sim_time = 0.0;
  std::shared_ptr<const std::string> json;
  std::size_t bytes = 0;
  double serialize_us = 0.0;
};

struct CoordinatorStatus {
  std::string state = "ready";  // ready | running | paused
  double speed = 1.0;
  double sim_time = 0.0;
  std::uint64_t step = 0;
  std::string scenario_id;
  std::string scenario_name;
  std::uint64_t seed = 0;
  std::string controller;
  bool lagging = false;
  std::size_t queue_depth = 0;
  std::size_t queue_capacity = 0;
  std::uint64_t frames_published = 0;
  std::size_t last_frame_bytes = 0;
  double avg_serialize_us = 0.0;
  double avg_step_us = 0.0;
  double realtime_factor = 0.0;  // simulated seconds per wall second (measured)
};

using CommandCallback = std::function<void(const CommandResult&)>;

class SimulationCoordinator {
 public:
  struct Options {
    double publish_hz = 20.0;
    std::size_t queue_capacity = 256;
    double metrics_interval = 1.0;  // wall seconds between metrics/series republishing
  };

  explicit SimulationCoordinator(Options options);
  ~SimulationCoordinator();
  SimulationCoordinator(const SimulationCoordinator&) = delete;
  SimulationCoordinator& operator=(const SimulationCoordinator&) = delete;

  /// Builds the first engine. Must be called before start().
  std::vector<std::string> load_initial(const ScenarioConfig& scenario);

  using FrameListener = std::function<void(std::shared_ptr<const PublishedFrame>)>;
  using JsonListener = std::function<void(std::shared_ptr<const std::string>)>;
  /// Listeners run on the simulation thread and must not block. Set before start().
  void set_listeners(FrameListener on_frame, JsonListener on_network, JsonListener on_status);

  void start();
  void stop();

  /// Enqueues a command; returns false (and does not enqueue) if the queue is full.
  /// `scenario` is required for Load commands. `done` runs on the simulation thread.
  bool submit(ParsedCommand command, std::shared_ptr<const ScenarioConfig> scenario, CommandCallback done);

  [[nodiscard]] std::shared_ptr<const PublishedFrame> latest_frame() const { return frame_.load(); }
  [[nodiscard]] std::shared_ptr<const std::string> network_json() const { return network_.load(); }
  [[nodiscard]] std::shared_ptr<const std::string> metrics_json() const { return metrics_.load(); }
  [[nodiscard]] std::shared_ptr<const std::string> replay_json() const { return replay_.load(); }
  [[nodiscard]] std::shared_ptr<const std::string> status_json() const { return status_json_.load(); }
  [[nodiscard]] CoordinatorStatus status() const;

 private:
  struct Envelope {
    ParsedCommand command;
    std::shared_ptr<const ScenarioConfig> scenario;
    CommandCallback done;
  };
  enum class State { Ready, Running, Paused };

  void loop();
  CommandResult handle(Envelope& env);
  CommandResult handle_control(const ControlCommand& c, const std::shared_ptr<const ScenarioConfig>& scenario);
  std::vector<std::string> rebuild(const ScenarioConfig& base, std::uint64_t seed, bool apply_settings);
  CommandResult seek(double time);
  void remember_setting(const EngineCommand& c);
  void publish_frame();
  void publish_network();
  void publish_metrics();
  void publish_replay();
  void publish_status();

  Options opt_;
  CommandQueue<Envelope> queue_;
  std::thread thread_;
  std::atomic<bool> stop_{false};

  // ---- owned by the simulation thread (or by the caller before start())
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
  double accumulator_ = 0.0;
  bool lagging_ = false;
  std::uint64_t seq_ = 0;
  std::uint64_t replay_size_ = 0;
  double serialize_avg_us_ = 0.0;
  double step_avg_us_ = 0.0;
  double realtime_factor_ = 0.0;

  // ---- published (thread-safe)
  std::atomic<std::shared_ptr<const PublishedFrame>> frame_;
  std::atomic<std::shared_ptr<const std::string>> network_;
  std::atomic<std::shared_ptr<const std::string>> metrics_;
  std::atomic<std::shared_ptr<const std::string>> replay_;
  std::atomic<std::shared_ptr<const std::string>> status_json_;
  mutable std::mutex status_mu_;
  CoordinatorStatus status_;

  FrameListener on_frame_;
  JsonListener on_network_;
  JsonListener on_status_;
};

}  // namespace traffic
