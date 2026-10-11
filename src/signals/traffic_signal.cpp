#include "traffic/signals/traffic_signal.hpp"

#include <algorithm>

namespace traffic {

TrafficSignal::TrafficSignal(const RoadNetwork& net, NodeId node, std::vector<SignalPhase> phases)
    : node_(node), conflicts_(net, node), phases_(std::move(phases)) {
  mstate_.assign(conflicts_.movements().size(), LightState::Red);
  cwstate_.assign(static_cast<std::size_t>(conflicts_.crosswalk_count()), WalkState::DontWalk);
  green_time_.assign(phases_.size(), 0.0);
  walk_start_.assign(cwstate_.size(), -1.0);
  if (!phases_.empty())
    for (int c : phases_[0].crosswalks) walk_start_[static_cast<std::size_t>(c)] = 0.0;
  refresh_states();
}

void TrafficSignal::begin_change(int target) {
  if (phases_.empty()) return;
  if (target < 0 || target >= phase_count() || target == current_) target = (current_ + 1) % phase_count();
  if (target == current_) return;  // single-phase signal: nothing to change to
  next_ = target;
  stage_ = phases_[static_cast<std::size_t>(current_)].yellow > 0 ? SignalStage::Yellow : SignalStage::AllRed;
  t_stage_ = 0.0;
  ++switches_;
}

void TrafficSignal::step(double dt, const SignalDecision& d, bool other_demand) {
  if (phases_.empty()) return;
  t_stage_ += dt;
  const SignalPhase& cur = phases_[static_cast<std::size_t>(current_)];
  switch (stage_) {
    case SignalStage::Green: {
      green_time_[static_cast<std::size_t>(current_)] += dt;
      const bool min_ok = t_stage_ >= cur.min_green - 1e-9 && !pedestrian_interval_active();
      if (min_ok && d.switch_phase && d.next_phase != current_) begin_change(d.next_phase);
      else if (min_ok && t_stage_ >= cur.max_green - 1e-9 && other_demand) begin_change(d.next_phase);
      break;
    }
    case SignalStage::Yellow:
      clearance_time_ += dt;
      if (t_stage_ >= cur.yellow - 1e-9) {
        stage_ = SignalStage::AllRed;
        t_stage_ = 0.0;
      }
      break;
    case SignalStage::AllRed:
      clearance_time_ += dt;
      if (t_stage_ >= cur.all_red - 1e-9) {
        current_ = next_;
        stage_ = SignalStage::Green;
        t_stage_ = 0.0;
        std::fill(walk_start_.begin(), walk_start_.end(), -1.0);
        for (int c : phases_[static_cast<std::size_t>(current_)].crosswalks) walk_start_[static_cast<std::size_t>(c)] = 0.0;
      }
      break;
  }
  refresh_states();
}

void TrafficSignal::refresh_states() {
  const SignalPhase& cur = phases_[static_cast<std::size_t>(current_)];
  const SignalPhase& nxt = phases_[static_cast<std::size_t>(next_)];
  const auto& ms = conflicts_.movements();
  for (std::size_t i = 0; i < ms.size(); ++i) {
    const MovementId m = ms[i];
    LightState s = LightState::Red;
    if (cur.contains(m)) {
      if (stage_ == SignalStage::Green) s = LightState::Green;
      // Movements shared by the current and next phase keep their green through the change.
      else if (nxt.contains(m)) s = LightState::Green;
      else if (stage_ == SignalStage::Yellow) s = LightState::Yellow;
    }
    mstate_[i] = s;
  }
  for (std::size_t c = 0; c < cwstate_.size(); ++c) {
    WalkState w = WalkState::DontWalk;
    if (stage_ == SignalStage::Green && walk_start_[c] >= 0.0) {
      const double into = t_stage_ - walk_start_[c];
      if (into < cur.walk_time) w = WalkState::Walk;
      else if (into < cur.walk_time + cur.ped_clearance) w = WalkState::Clearance;
    }
    cwstate_[c] = w;
  }
}

void TrafficSignal::request_walk(int c) {
  if (stage_ != SignalStage::Green || c < 0 || static_cast<std::size_t>(c) >= cwstate_.size()) return;
  const SignalPhase& cur = phases_[static_cast<std::size_t>(current_)];
  if (!cur.contains_crosswalk(c) || cwstate_[static_cast<std::size_t>(c)] != WalkState::DontWalk) return;
  if (t_stage_ + cur.walk_time + cur.ped_clearance > cur.max_green) return;
  walk_start_[static_cast<std::size_t>(c)] = t_stage_;
  refresh_states();
}

bool TrafficSignal::pedestrian_interval_active() const {
  if (stage_ != SignalStage::Green) return false;
  for (WalkState w : cwstate_)
    if (w != WalkState::DontWalk) return true;
  return false;
}

LightState TrafficSignal::movement_state(MovementId m) const {
  const int i = conflicts_.local(m);
  return i < 0 ? LightState::Red : mstate_[static_cast<std::size_t>(i)];
}

WalkState TrafficSignal::crosswalk_state(int c) const {
  if (c < 0 || static_cast<std::size_t>(c) >= cwstate_.size()) return WalkState::DontWalk;
  return cwstate_[static_cast<std::size_t>(c)];
}

bool TrafficSignal::set_phase_timing(int phase, double min_green, double max_green, std::string* error) {
  if (phase < 0 || phase >= phase_count()) {
    if (error) *error = "phase index out of range";
    return false;
  }
  auto& p = phases_[static_cast<std::size_t>(phase)];
  const double ped_min = p.walk_time + p.ped_clearance;
  if (!(min_green > 0) || min_green > max_green || max_green > 600) {
    if (error) *error = "require 0 < min_green <= max_green <= 600";
    return false;
  }
  if (min_green < ped_min) {
    if (error) *error = "min_green must cover pedestrian walk + clearance (" + std::to_string(ped_min) + " s)";
    return false;
  }
  p.min_green = min_green;
  p.max_green = max_green;
  return true;
}

bool TrafficSignal::invariants_hold(std::string* why) const {
  const auto& ms = conflicts_.movements();
  for (std::size_t i = 0; i < ms.size(); ++i) {
    if (mstate_[i] == LightState::Red) continue;
    for (std::size_t j = i + 1; j < ms.size(); ++j) {
      if (mstate_[j] == LightState::Red) continue;
      if (conflicts_.conflicts(ms[i], ms[j])) {
        if (why) *why = "conflicting movements both non-red";
        return false;
      }
    }
    for (std::size_t c = 0; c < cwstate_.size(); ++c)
      if (cwstate_[c] == WalkState::Walk && conflicts_.crosswalk_conflicts(static_cast<int>(c), ms[i])) {
        if (why) *why = "WALK shown across a non-red movement";
        return false;
      }
  }
  return true;
}

}  // namespace traffic
