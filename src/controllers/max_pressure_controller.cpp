#include <cstdio>

#include "traffic/controllers/signal_controller.hpp"

namespace traffic {

void MaxPressureController::initialize(const IntersectionConfig&) { next_decision_ = 0.0; }

SignalDecision MaxPressureController::decide(const IntersectionObservation& obs, double t) {
  SignalDecision d;
  const int n = static_cast<int>(obs.phases->size());
  const int cur = obs.current_phase;
  d.next_phase = (cur + 1) % n;
  if (n < 2) return d;

  auto pressure = [&](int i) {
    const auto& p = obs.phase_obs[static_cast<std::size_t>(i)];
    if (!(*obs.phases)[static_cast<std::size_t>(i)].serves_vehicles())
      return cfg_.w_pedestrian * (p.pedestrians_waiting + p.oldest_pedestrian_wait / 30.0);
    return p.pressure + (p.pedestrians_waiting > 0 ? cfg_.w_pedestrian * p.pedestrians_waiting : 0.0);
  };
  bool vehicle_rival = false;
  for (int i = 0; i < n; ++i)
    if (i != cur && (*obs.phases)[static_cast<std::size_t>(i)].serves_vehicles() && obs.has_demand(i)) vehicle_rival = true;
  int best = -1;
  double best_p = 0.0;
  for (int i = 0; i < n; ++i) {
    if (i == cur || !obs.has_demand(i)) continue;
    if (vehicle_rival && !(*obs.phases)[static_cast<std::size_t>(i)].serves_vehicles()) continue;  // see adaptive
    const double p = pressure(i);
    if (best < 0 || p > best_p) { best = i; best_p = p; }
  }
  if (best >= 0) d.next_phase = best;
  if (obs.stage != SignalStage::Green || best < 0) return d;
  if (obs.time_in_stage < (*obs.phases)[static_cast<std::size_t>(cur)].min_green) return d;

  char buf[160];
  for (int i = 0; i < n; ++i) {  // starvation guard
    if (i == cur) continue;
    const auto& p = obs.phase_obs[static_cast<std::size_t>(i)];
    if (p.oldest_wait >= cfg_.max_wait || p.oldest_pedestrian_wait >= cfg_.max_pedestrian_wait) {
      d.switch_phase = true;
      d.next_phase = i;
      std::snprintf(buf, sizeof buf, "starvation guard: phase %d", i + 1);
      reason_ = buf;
      return d;
    }
  }
  if (t < next_decision_) return d;
  next_decision_ = t + cfg_.decision_interval;
  const double cur_p = pressure(cur);
  if (best_p > cur_p) {
    d.switch_phase = true;
    std::snprintf(buf, sizeof buf, "pressure %.1f (phase %d) > %.1f (current)", best_p, best + 1, cur_p);
    reason_ = buf;
  }
  return d;
}

}  // namespace traffic
