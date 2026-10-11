#pragma once
// Pluggable signal-control policies. A controller sees an IntersectionObservation
// (sensor-like aggregates, never engine internals) and returns a SignalDecision.
// The TrafficSignal safety layer enforces min-green, yellow, all-red and conflict
// freedom regardless of what the controller asks for.

#include <memory>
#include <string>
#include <vector>

#include "traffic/core/config.hpp"
#include "traffic/signals/traffic_signal.hpp"

namespace traffic {

struct PhaseObservation {
  int queued = 0;            // vehicles with speed < 2 m/s waiting for a movement of this phase
  int approaching = 0;       // moving vehicles within 150 m heading for this phase
  int arriving_soon = 0;     // moving vehicles that would reach the stop line within the gap-out window
  double wait_sum = 0.0;     // total queued time of queued vehicles (s)
  double oldest_wait = 0.0;  // s
  int pedestrians_waiting = 0;
  double oldest_pedestrian_wait = 0.0;
  bool emergency = false;    // an emergency vehicle is approaching/queued for this phase
  double pressure = 0.0;     // sum over movements of (upstream count - downstream count)
};

struct IntersectionObservation {
  NodeId node = kInvalidNode;
  double time = 0.0;
  int current_phase = 0;
  SignalStage stage = SignalStage::Green;
  double time_in_stage = 0.0;
  const std::vector<SignalPhase>* phases = nullptr;
  std::vector<PhaseObservation> phase_obs;

  [[nodiscard]] bool has_demand(int phase) const;
};

struct IntersectionConfig {
  NodeId node = kInvalidNode;
  std::vector<SignalPhase> phases;
};

class SignalController {
 public:
  virtual ~SignalController() = default;
  virtual void initialize(const IntersectionConfig& config) = 0;
  virtual SignalDecision decide(const IntersectionObservation& obs, double simulation_time) = 0;
  [[nodiscard]] virtual std::string name() const = 0;
  /// Short human-readable reason for the most recent switch (for the UI).
  [[nodiscard]] virtual std::string last_reason() const { return {}; }
};

/// Cycles phases in order with configured greens; supports time-of-day plans.
class FixedTimeController final : public SignalController {
 public:
  explicit FixedTimeController(ControllerConfig cfg) : cfg_(std::move(cfg)) {}
  void initialize(const IntersectionConfig& config) override;
  SignalDecision decide(const IntersectionObservation& obs, double t) override;
  [[nodiscard]] std::string name() const override { return "fixed_time"; }
  [[nodiscard]] std::string last_reason() const override { return reason_; }
  void set_greens(std::vector<double> greens) { greens_ = std::move(greens); plans_.clear(); }
  [[nodiscard]] double green_for(int phase, double t) const;

 private:
  ControllerConfig cfg_;
  NodeId node_ = kInvalidNode;
  std::vector<double> greens_;
  std::vector<std::pair<double, std::vector<double>>> plans_;
  std::string reason_;
};

/// Transparent demand-responsive heuristic: scores phases by queue, accumulated wait,
/// oldest wait, arrivals and pedestrians; holds the current phase while it is still
/// discharging (gap-out) unless a rival beats it by a hysteresis margin; prevents
/// starvation with a max-wait override; optional emergency priority.
class AdaptiveSignalController final : public SignalController {
 public:
  explicit AdaptiveSignalController(ControllerConfig cfg) : cfg_(std::move(cfg)) {}
  void initialize(const IntersectionConfig& config) override;
  SignalDecision decide(const IntersectionObservation& obs, double t) override;
  [[nodiscard]] std::string name() const override { return "adaptive"; }
  [[nodiscard]] std::string last_reason() const override { return reason_; }
  [[nodiscard]] double score(const IntersectionObservation& obs, int phase) const;

 private:
  ControllerConfig cfg_;
  std::string reason_;
};

/// Max-pressure (Varaiya 2013): at fixed decision intervals serve the phase with the
/// largest sum of (upstream queue - downstream occupancy). Includes the same
/// starvation guard as the adaptive controller.
class MaxPressureController final : public SignalController {
 public:
  explicit MaxPressureController(ControllerConfig cfg) : cfg_(std::move(cfg)) {}
  void initialize(const IntersectionConfig& config) override;
  SignalDecision decide(const IntersectionObservation& obs, double t) override;
  [[nodiscard]] std::string name() const override { return "max_pressure"; }
  [[nodiscard]] std::string last_reason() const override { return reason_; }

 private:
  ControllerConfig cfg_;
  double next_decision_ = 0.0;
  std::string reason_;
};

/// Creates a controller from config; returns nullptr and sets `error` for unknown types.
std::unique_ptr<SignalController> make_controller(const ControllerConfig& cfg, std::string* error = nullptr);
std::vector<std::string> validate_controller_config(const ControllerConfig& cfg);

}  // namespace traffic
