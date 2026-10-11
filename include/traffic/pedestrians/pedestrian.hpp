#pragma once
#include <vector>

#include "traffic/core/config.hpp"
#include "traffic/core/rng.hpp"
#include "traffic/network/road_network.hpp"
#include "traffic/signals/traffic_signal.hpp"

namespace traffic {

struct Pedestrian {
  PedestrianId id = 0;
  NodeId node = kInvalidNode;
  int crosswalk = 0;
  bool forward = true;        // walks a->b if true
  double arrival_time = 0.0;
  double start_time = -1.0;   // < 0 while waiting at the kerb
  double progress = 0.0;      // 0..1 along the crosswalk
  double speed = 1.2;
  bool measured = true;

  [[nodiscard]] bool crossing() const { return start_time >= 0.0; }
};

struct PedestrianCompletion {
  NodeId node;
  double wait;
  bool measured;
};

/// Pedestrians arrive at crosswalks of signalised intersections (Bernoulli per step),
/// wait at the kerb, start only during WALK, and cross at their own speed. Crosswalk
/// occupancy is exposed so vehicles never enter a crosswalk with someone on it.
/// This is a behavioural proxy, not a certified pedestrian-safety model.
class PedestrianModel {
 public:
  PedestrianModel(const PedestrianSpec& spec, const RoadNetwork& net, std::uint64_t seed);

  void generate(double t, double dt, double measure_from);
  /// `signal_for(node)` returns the signal controlling a node (never null for signalised nodes).
  template <typename SignalLookup>
  void update(double t, double dt, SignalLookup&& signal_for, std::vector<PedestrianCompletion>& done);

  [[nodiscard]] bool crosswalk_occupied(NodeId node, int crosswalk) const;
  [[nodiscard]] int waiting(NodeId node, int crosswalk) const;
  [[nodiscard]] double oldest_wait(NodeId node, int crosswalk, double t) const;
  [[nodiscard]] const std::vector<Pedestrian>& pedestrians() const { return peds_; }
  [[nodiscard]] Vec2 position(const Pedestrian& p) const;

  void set_multiplier(double m) { multiplier_ = m; }
  [[nodiscard]] double multiplier() const { return multiplier_; }

 private:
  const RoadNetwork* net_;
  PedestrianSpec spec_;
  Rng rng_;
  double multiplier_ = 1.0;
  PedestrianId next_id_ = 1;
  std::vector<Pedestrian> peds_;
  std::vector<std::vector<int>> occupancy_;  // [node][crosswalk] -> pedestrians on the crossing
  std::vector<std::vector<int>> waiting_;    // [node][crosswalk] -> pedestrians at the kerb
};

template <typename SignalLookup>
void PedestrianModel::update(double t, double dt, SignalLookup&& signal_for,
                             std::vector<PedestrianCompletion>& done) {
  for (auto& p : peds_) {
    auto& occ = occupancy_[static_cast<std::size_t>(p.node)][static_cast<std::size_t>(p.crosswalk)];
    auto& wt = waiting_[static_cast<std::size_t>(p.node)][static_cast<std::size_t>(p.crosswalk)];
    if (!p.crossing()) {
      const TrafficSignal* sig = signal_for(p.node);
      if (sig && sig->crosswalk_state(p.crosswalk) == WalkState::Walk) {
        p.start_time = t;
        ++occ;
        --wt;
      }
      continue;
    }
    const double len = net_->node(p.node).crosswalks[static_cast<std::size_t>(p.crosswalk)].length;
    p.progress += p.speed * dt / std::max(len, 1.0);
    if (p.progress >= 1.0) {
      --occ;
      done.push_back({p.node, p.start_time - p.arrival_time, p.measured});
    }
  }
  std::erase_if(peds_, [](const Pedestrian& p) { return p.progress >= 1.0; });
}

}  // namespace traffic
