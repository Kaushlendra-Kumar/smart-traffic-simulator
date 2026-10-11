#include "traffic/controllers/signal_controller.hpp"

namespace traffic {

std::vector<std::string> validate_controller_config(const ControllerConfig& c) {
  std::vector<std::string> e;
  if (c.type != "fixed_time" && c.type != "adaptive" && c.type != "max_pressure")
    e.push_back("unknown controller type '" + c.type + "' (expected fixed_time, adaptive or max_pressure)");
  for (double g : c.greens)
    if (!(g > 0 && g <= 300)) e.push_back("fixed-time greens must be in (0, 300] s");
  for (const auto& [start, g] : c.plans) {
    if (start < 0) e.push_back("plan start times must be >= 0");
    for (double x : g)
      if (!(x > 0 && x <= 300)) e.push_back("plan greens must be in (0, 300] s");
  }
  auto nonneg = [&](double v, const char* name) {
    if (!(v >= 0)) e.push_back(std::string(name) + " must be >= 0");
  };
  nonneg(c.w_queue, "w_queue");
  nonneg(c.w_wait, "w_wait");
  nonneg(c.w_oldest, "w_oldest");
  nonneg(c.w_arrivals, "w_arrivals");
  nonneg(c.w_pedestrian, "w_pedestrian");
  nonneg(c.hysteresis_ratio, "hysteresis_ratio");
  nonneg(c.hysteresis_abs, "hysteresis_abs");
  if (!(c.gap_out_seconds > 0 && c.gap_out_seconds <= 15)) e.push_back("gap_out_seconds must be in (0, 15]");
  if (!(c.max_wait >= 10)) e.push_back("max_wait must be >= 10 s");
  if (!(c.max_pedestrian_wait >= 10)) e.push_back("max_pedestrian_wait must be >= 10 s");
  if (!(c.decision_interval >= 1 && c.decision_interval <= 60)) e.push_back("decision_interval must be in [1, 60] s");
  return e;
}

std::unique_ptr<SignalController> make_controller(const ControllerConfig& cfg, std::string* error) {
  if (cfg.type == "fixed_time") return std::make_unique<FixedTimeController>(cfg);
  if (cfg.type == "adaptive") return std::make_unique<AdaptiveSignalController>(cfg);
  if (cfg.type == "max_pressure") return std::make_unique<MaxPressureController>(cfg);
  if (error) *error = "unknown controller type '" + cfg.type + "'";
  return nullptr;
}

}  // namespace traffic
