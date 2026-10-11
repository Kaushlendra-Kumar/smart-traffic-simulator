#include <algorithm>

#include "traffic/controllers/signal_controller.hpp"

namespace traffic {

bool IntersectionObservation::has_demand(int phase) const {
  const auto& p = phase_obs[static_cast<std::size_t>(phase)];
  return p.queued > 0 || p.approaching > 0 || p.pedestrians_waiting > 0;
}

void FixedTimeController::initialize(const IntersectionConfig& config) {
  node_ = config.node;
  const std::size_t n = config.phases.size();
  auto fit = [&](const std::vector<double>& g) {
    std::vector<double> out(n);
    for (std::size_t i = 0; i < n; ++i)
      out[i] = std::clamp(i < g.size() ? g[i] : config.phases[i].green, config.phases[i].min_green,
                          config.phases[i].max_green);
    // Pedestrian-only phases have a fixed length.
    for (std::size_t i = 0; i < n; ++i)
      if (!config.phases[i].serves_vehicles()) out[i] = config.phases[i].green;
    return out;
  };
  auto node_it = cfg_.node_greens.find(node_);
  if (node_it != cfg_.node_greens.end()) greens_ = fit(node_it->second);
  else if (cfg_.greens.size() == n) greens_ = fit(cfg_.greens);
  else greens_ = fit({});
  plans_.clear();
  for (const auto& [start, g] : cfg_.plans)
    if (g.size() == n) plans_.push_back({start, fit(g)});
  std::sort(plans_.begin(), plans_.end());
}

double FixedTimeController::green_for(int phase, double t) const {
  const std::vector<double>* g = &greens_;
  for (const auto& [start, plan] : plans_)
    if (t >= start) g = &plan;
  return (*g)[static_cast<std::size_t>(phase)];
}

SignalDecision FixedTimeController::decide(const IntersectionObservation& obs, double t) {
  SignalDecision d;
  const int n = static_cast<int>(obs.phases->size());
  d.next_phase = (obs.current_phase + 1) % n;
  if (obs.stage == SignalStage::Green && obs.time_in_stage >= green_for(obs.current_phase, t) - 1e-9) {
    d.switch_phase = true;
    reason_ = "fixed green of " + std::to_string(static_cast<int>(green_for(obs.current_phase, t))) + " s elapsed";
  }
  return d;
}

}  // namespace traffic
