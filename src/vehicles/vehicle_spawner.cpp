#include "traffic/vehicles/vehicle_spawner.hpp"

#include <algorithm>

#include "traffic/network/routing.hpp"

namespace traffic {

VehicleSpawner::VehicleSpawner(const DemandSpec& spec, const RoadNetwork& net, std::uint64_t seed)
    : profile_(spec.profile),
      arrivals_(seed, static_cast<std::uint64_t>(RngStream::Demand)),
      choices_(seed, static_cast<std::uint64_t>(RngStream::VehicleChoice)) {
  std::sort(profile_.begin(), profile_.end());
  double total = 0.0;
  for (double w : spec.vehicle_mix) total += std::max(0.0, w);
  if (total <= 0) total = 1.0;
  double acc = 0.0;
  for (std::size_t i = 0; i < spec.vehicle_mix.size(); ++i) {
    acc += std::max(0.0, spec.vehicle_mix[i]) / total;
    mix_cumulative_[i] = acc;
  }
  mix_cumulative_.back() = 1.0;

  const auto boundaries = net.boundary_nodes();
  for (NodeId b : boundaries) {
    if (net.node(b).out.empty()) continue;
    const SourceSpec* explicit_spec = nullptr;
    for (const auto& s : spec.sources)
      if (s.node == b) explicit_spec = &s;
    Source src;
    src.node = b;
    src.rate_vph = explicit_spec ? explicit_spec->rate_vph : spec.default_rate_vph;
    if (src.rate_vph <= 0) continue;
    std::vector<std::pair<NodeId, double>> weights;
    if (explicit_spec && !explicit_spec->destinations.empty()) {
      for (auto [d, w] : explicit_spec->destinations)
        if (d != b && w > 0 && d >= 0 && static_cast<std::size_t>(d) < net.nodes.size() && !net.node(d).in.empty())
          weights.push_back({d, w});
    } else {
      for (NodeId d : boundaries)
        if (d != b && !net.node(d).in.empty() && route_between(net, b, d, distance_cost)) weights.push_back({d, 1.0});
    }
    if (weights.empty()) continue;
    double sum = 0.0;
    for (auto& [d, w] : weights) sum += w;
    double c = 0.0;
    for (auto& [d, w] : weights) {
      c += w / sum;
      src.destinations.push_back(d);
      src.cumulative.push_back(c);
    }
    src.cumulative.back() = 1.0;
    sources_.push_back(std::move(src));
  }
}

double VehicleSpawner::profile_at(double t) const {
  if (profile_.empty()) return 1.0;
  if (t <= profile_.front().first) return profile_.front().second;
  if (t >= profile_.back().first) return profile_.back().second;
  for (std::size_t i = 0; i + 1 < profile_.size(); ++i) {
    const auto [t0, m0] = profile_[i];
    const auto [t1, m1] = profile_[i + 1];
    if (t >= t0 && t <= t1) return t1 > t0 ? m0 + (m1 - m0) * (t - t0) / (t1 - t0) : m1;
  }
  return 1.0;
}

double VehicleSpawner::total_rate_vph(double t) const {
  double r = 0.0;
  for (const auto& s : sources_) r += s.rate_vph;
  return r * profile_at(t) * multiplier_;
}

IdmParams VehicleSpawner::sample_params(VehicleType type) {
  IdmParams p = default_params(type);
  p.desired_speed_factor *= choices_.uniform(0.9, 1.1);
  p.time_headway *= choices_.uniform(0.85, 1.15);
  return p;
}

void VehicleSpawner::generate(double t, double dt, std::vector<TripRequest>& out) {
  const double factor = profile_at(t) * multiplier_;
  for (const auto& src : sources_) {
    const double p = std::min(0.95, src.rate_vph / 3600.0 * factor * dt);
    const double u = arrivals_.uniform();  // always consumed: keeps the stream aligned
    if (u >= p) continue;
    TripRequest trip;
    trip.id = next_id_++;
    trip.origin = src.node;
    trip.demand_time = t;
    const double ud = choices_.uniform();
    const auto it = std::lower_bound(src.cumulative.begin(), src.cumulative.end(), ud);
    trip.destination = src.destinations[static_cast<std::size_t>(std::min<std::ptrdiff_t>(
        it - src.cumulative.begin(), static_cast<std::ptrdiff_t>(src.destinations.size()) - 1))];
    const double ut = choices_.uniform();
    std::size_t type_idx = 0;
    while (type_idx + 1 < mix_cumulative_.size() && ut >= mix_cumulative_[type_idx]) ++type_idx;
    trip.type = static_cast<VehicleType>(type_idx);
    trip.params = sample_params(trip.type);  // drawn before the enable check: stream stays aligned
    if (!enabled_[type_idx]) {
      trip.type = VehicleType::Car;
      const double f = trip.params.desired_speed_factor / default_params(static_cast<VehicleType>(type_idx)).desired_speed_factor;
      const double h = trip.params.time_headway / default_params(static_cast<VehicleType>(type_idx)).time_headway;
      trip.params = default_params(VehicleType::Car);
      trip.params.desired_speed_factor *= f;
      trip.params.time_headway *= h;
    }
    out.push_back(trip);
  }
}

TripRequest VehicleSpawner::manual_trip(VehicleType type, NodeId origin, NodeId destination, double t) {
  TripRequest trip;
  trip.id = next_id_++;
  trip.type = type;
  trip.origin = origin;
  trip.destination = destination;
  trip.demand_time = t;
  trip.params = default_params(type);
  return trip;
}

}  // namespace traffic
