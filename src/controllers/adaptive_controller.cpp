#include <cmath>
#include <cstdio>

#include "traffic/controllers/signal_controller.hpp"

namespace traffic {

void AdaptiveSignalController::initialize(const IntersectionConfig&) {}

double AdaptiveSignalController::score(const IntersectionObservation& obs, int phase) const {
  const auto& p = obs.phase_obs[static_cast<std::size_t>(phase)];
  double s = cfg_.w_queue * p.queued + cfg_.w_wait * p.wait_sum / 60.0 + cfg_.w_oldest * p.oldest_wait / 30.0 +
             cfg_.w_arrivals * p.approaching;
  if (p.pedestrians_waiting > 0)
    s += cfg_.w_pedestrian * (p.pedestrians_waiting + p.oldest_pedestrian_wait / 30.0);
  if (cfg_.emergency_priority && p.emergency) s += 1000.0;
  return s;
}

SignalDecision AdaptiveSignalController::decide(const IntersectionObservation& obs, double) {
  SignalDecision d;
  const int n = static_cast<int>(obs.phases->size());
  const int cur = obs.current_phase;
  d.next_phase = (cur + 1) % n;
  if (n < 2) return d;

  // Pick the most valuable rival phase now so the safety layer has a target even when
  // max-green forces a change.
  // Pedestrian-only phases stop every approach for walk + clearance, so they compete
  // only when no vehicle phase has demand; otherwise the starvation guard serves them.
  bool vehicle_rival = false;
  for (int i = 0; i < n; ++i)
    if (i != cur && (*obs.phases)[static_cast<std::size_t>(i)].serves_vehicles() && obs.has_demand(i)) vehicle_rival = true;
  int best = -1;
  double best_score = 0.0;
  for (int i = 0; i < n; ++i) {
    if (i == cur || !obs.has_demand(i)) continue;
    if (vehicle_rival && !(*obs.phases)[static_cast<std::size_t>(i)].serves_vehicles()) continue;
    const double s = score(obs, i);
    if (best < 0 || s > best_score) { best = i; best_score = s; }
  }
  if (best >= 0) d.next_phase = best;
  if (obs.stage != SignalStage::Green || best < 0) return d;  // rest in green if nobody else waits
  const auto& phase = (*obs.phases)[static_cast<std::size_t>(cur)];
  if (obs.time_in_stage < phase.min_green) return d;

  char buf[160];
  // 1) Emergency pre-emption.
  const auto& cur_obs = obs.phase_obs[static_cast<std::size_t>(cur)];
  if (cfg_.emergency_priority && !cur_obs.emergency) {
    for (int i = 0; i < n; ++i)
      if (i != cur && obs.phase_obs[static_cast<std::size_t>(i)].emergency) {
        d.switch_phase = true;
        d.next_phase = i;
        std::snprintf(buf, sizeof buf, "emergency vehicle waiting for phase %d", i + 1);
        reason_ = buf;
        return d;
      }
  }
  // 2) Starvation guard: serve the phase whose oldest vehicle/pedestrian waited too long.
  int starving = -1;
  double worst = 0.0;
  for (int i = 0; i < n; ++i) {
    if (i == cur) continue;
    const auto& p = obs.phase_obs[static_cast<std::size_t>(i)];
    const double over = std::max(p.oldest_wait - cfg_.max_wait, p.oldest_pedestrian_wait - cfg_.max_pedestrian_wait);
    if (over >= 0 && (starving < 0 || over > worst)) { starving = i; worst = over; }
  }
  if (starving >= 0) {
    d.switch_phase = true;
    d.next_phase = starving;
    std::snprintf(buf, sizeof buf, "starvation guard: phase %d waited beyond its limit", starving + 1);
    reason_ = buf;
    return d;
  }
  // 3) Gap-out: the current phase has nothing left to discharge.
  const bool current_busy = cur_obs.queued > 0 || cur_obs.arriving_soon > 0;
  if (!current_busy) {
    d.switch_phase = true;
    std::snprintf(buf, sizeof buf, "gap-out: phase %d empty, serving phase %d (score %.1f)", cur + 1, best + 1, best_score);
    reason_ = buf;
    return d;
  }
  // 4) Hysteresis: only switch if a rival is clearly more valuable.
  const double cur_score = score(obs, cur);
  if (best_score > cur_score * (1.0 + cfg_.hysteresis_ratio) + cfg_.hysteresis_abs) {
    d.switch_phase = true;
    std::snprintf(buf, sizeof buf, "phase %d score %.1f beats current %.1f", best + 1, best_score, cur_score);
    reason_ = buf;
  }
  return d;
}

}  // namespace traffic
