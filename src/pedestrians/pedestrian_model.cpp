#include <algorithm>

#include "traffic/pedestrians/pedestrian.hpp"

namespace traffic {

PedestrianModel::PedestrianModel(const PedestrianSpec& spec, const RoadNetwork& net, std::uint64_t seed)
    : net_(&net), spec_(spec), rng_(seed, static_cast<std::uint64_t>(RngStream::Pedestrians)) {
  occupancy_.resize(net.nodes.size());
  waiting_.resize(net.nodes.size());
  for (const auto& n : net.nodes) {
    occupancy_[static_cast<std::size_t>(n.id)].assign(n.crosswalks.size(), 0);
    waiting_[static_cast<std::size_t>(n.id)].assign(n.crosswalks.size(), 0);
  }
}

void PedestrianModel::generate(double t, double dt, double measure_from) {
  const double rate = spec_.rate_per_hour * multiplier_;
  for (const auto& n : net_->nodes) {
    if (!n.signalized || n.crosswalks.empty()) continue;
    const double per_cw = rate / static_cast<double>(n.crosswalks.size());
    for (const auto& cw : n.crosswalks) {
      const double p = std::min(0.95, per_cw / 3600.0 * dt);
      const double u = rng_.uniform();  // always consumed
      if (u >= p) continue;
      Pedestrian ped;
      ped.id = next_id_++;
      ped.node = n.id;
      ped.crosswalk = cw.index;
      ped.forward = rng_.uniform() < 0.5;
      ped.speed = rng_.uniform(spec_.walk_speed_min, spec_.walk_speed_max);
      ped.arrival_time = t;
      ped.measured = t >= measure_from;
      ++waiting_[static_cast<std::size_t>(n.id)][static_cast<std::size_t>(cw.index)];
      peds_.push_back(ped);
    }
  }
}

bool PedestrianModel::crosswalk_occupied(NodeId node, int crosswalk) const {
  return occupancy_[static_cast<std::size_t>(node)][static_cast<std::size_t>(crosswalk)] > 0;
}

int PedestrianModel::waiting(NodeId node, int crosswalk) const {
  return waiting_[static_cast<std::size_t>(node)][static_cast<std::size_t>(crosswalk)];
}

double PedestrianModel::oldest_wait(NodeId node, int crosswalk, double t) const {
  double oldest = 0.0;
  for (const auto& p : peds_)
    if (p.node == node && p.crosswalk == crosswalk && !p.crossing()) oldest = std::max(oldest, t - p.arrival_time);
  return oldest;
}

Vec2 PedestrianModel::position(const Pedestrian& p) const {
  const auto& cw = net_->node(p.node).crosswalks[static_cast<std::size_t>(p.crosswalk)];
  const Vec2 from = p.forward ? cw.a : cw.b;
  const Vec2 to = p.forward ? cw.b : cw.a;
  if (!p.crossing()) {
    // Wait just behind the kerb end, spread out a little by id.
    const Vec2 dir = (to - from).normalized();
    const double jitter = static_cast<double>(p.id % 5) * 0.6 - 1.2;
    return from - dir * 1.0 + dir.right_normal() * jitter;
  }
  return from + (to - from) * std::clamp(p.progress, 0.0, 1.0);
}

}  // namespace traffic
