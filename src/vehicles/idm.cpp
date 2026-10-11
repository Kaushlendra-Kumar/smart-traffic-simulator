#include <algorithm>
#include <cmath>

#include "traffic/vehicles/vehicle.hpp"

namespace traffic {

IdmParams default_params(VehicleType type) {
  IdmParams p;
  switch (type) {
    case VehicleType::Car:
      break;
    case VehicleType::Bus:
      p.max_accel = 1.0;
      p.comfortable_decel = 1.5;
      p.time_headway = 1.6;
      p.min_gap = 2.5;
      p.length = 12.0;
      p.desired_speed_factor = 0.85;
      break;
    case VehicleType::Emergency:
      p.max_accel = 2.0;
      p.comfortable_decel = 2.5;
      p.time_headway = 1.1;
      p.min_gap = 2.0;
      p.length = 6.0;
      p.desired_speed_factor = 1.15;
      break;
  }
  return p;
}

double idm_acceleration(const IdmParams& p, double v, double v0, double gap, double dv) {
  v0 = std::max(v0, 0.1);
  const double free_term = std::pow(std::max(v, 0.0) / v0, 4.0);
  const double s_star =
      p.min_gap + std::max(0.0, v * p.time_headway + v * dv / (2.0 * std::sqrt(p.max_accel * p.comfortable_decel)));
  const double s = std::max(gap, 0.05);
  const double acc = p.max_accel * (1.0 - free_term - (s_star / s) * (s_star / s));
  return std::clamp(acc, -kMaxBraking, p.max_accel);
}

}  // namespace traffic
