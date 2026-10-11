#pragma once
#include <array>
#include <vector>

#include "traffic/core/config.hpp"
#include "traffic/core/rng.hpp"
#include "traffic/network/road_network.hpp"
#include "traffic/vehicles/vehicle.hpp"

namespace traffic {

struct TripRequest {
  VehicleId id = 0;
  VehicleType type = VehicleType::Car;
  NodeId origin = kInvalidNode;
  NodeId destination = kInvalidNode;
  double demand_time = 0.0;
  IdmParams params;
};

/// Generates the demand realisation: Bernoulli arrivals per source per step
/// (a discretised Poisson process). Each step consumes exactly one uniform draw per
/// source from the Demand stream regardless of outcome, and attributes of arriving
/// trips come from a separate stream, so the realisation depends only on
/// (seed, demand settings, time) — never on controller decisions.
class VehicleSpawner {
 public:
  VehicleSpawner(const DemandSpec& spec, const RoadNetwork& net, std::uint64_t seed);

  void generate(double t, double dt, std::vector<TripRequest>& out);
  TripRequest manual_trip(VehicleType type, NodeId origin, NodeId destination, double t);

  void set_multiplier(double m) { multiplier_ = m; }
  [[nodiscard]] double multiplier() const { return multiplier_; }
  void set_type_enabled(VehicleType t, bool enabled) { enabled_[static_cast<std::size_t>(t)] = enabled; }
  [[nodiscard]] bool type_enabled(VehicleType t) const { return enabled_[static_cast<std::size_t>(t)]; }
  [[nodiscard]] double profile_at(double t) const;
  /// Effective total demand in vehicles/hour at time t.
  [[nodiscard]] double total_rate_vph(double t) const;

  struct Source {
    NodeId node = kInvalidNode;
    double rate_vph = 0.0;
    std::vector<NodeId> destinations;
    std::vector<double> cumulative;  // normalised cumulative weights
  };
  [[nodiscard]] const std::vector<Source>& sources() const { return sources_; }

 private:
  IdmParams sample_params(VehicleType type);

  std::vector<Source> sources_;
  std::vector<std::pair<double, double>> profile_;
  std::array<double, kVehicleTypeCount> mix_cumulative_{};
  std::array<bool, kVehicleTypeCount> enabled_{true, true, true};
  double multiplier_ = 1.0;
  Rng arrivals_;
  Rng choices_;
  VehicleId next_id_ = 1;
};

}  // namespace traffic
