#pragma once
#include <vector>

#include "traffic/core/types.hpp"

namespace traffic {

/// Intelligent Driver Model parameters (Treiber, Hennecke & Helbing 2000).
struct IdmParams {
  double desired_speed_factor = 1.0;  // multiplies the road speed limit
  double max_accel = 1.5;             // a  (m/s^2)
  double comfortable_decel = 2.0;     // b  (m/s^2)
  double time_headway = 1.4;          // T  (s)
  double min_gap = 2.0;               // s0 (m)
  double length = 4.5;                // vehicle length (m)
};

IdmParams default_params(VehicleType type);

/// IDM acceleration. `gap` is the bumper-to-bumper distance to the leader (use a
/// large value when free), `dv` = v - v_leader (approach rate).
double idm_acceleration(const IdmParams& p, double v, double v0, double gap, double dv);

/// Hard physical braking limit used to clamp the IDM output (m/s^2, positive).
inline constexpr double kMaxBraking = 9.0;

struct Vehicle {
  bool active = false;
  VehicleId id = 0;
  VehicleType type = VehicleType::Car;
  IdmParams params;

  // ---- position
  bool on_connector = false;
  RoadId road = kInvalidRoad;        // current road (or the incoming road while on a connector)
  int lane = 0;
  MovementId movement = kInvalidMovement;  // while on a connector
  int connector = 0;                 // connector index (= incoming lane) while on a connector
  double s = 0.0;                    // distance along lane / connector (front bumper)
  double v = 0.0;
  double a = 0.0;
  double lateral_offset = 0.0;       // render-only offset that decays after a lane change

  // ---- route
  NodeId origin = kInvalidNode;
  NodeId destination = kInvalidNode;
  std::vector<RoadId> route;
  std::size_t route_index = 0;
  MovementId next_movement = kInvalidMovement;  // movement at the end of the current road

  // ---- control state
  bool committed = false;            // passed the point of no return for the next junction
  double lane_change_cooldown = 0.0;

  // ---- bookkeeping for metrics
  double demand_time = 0.0;          // when the trip was requested (arrival at the source)
  double enter_time = -1.0;          // when it physically entered the network
  double freeflow_time = 0.0;        // free-flow time of fully traversed segments
  double stopped_time = 0.0;
  int stops = 0;
  bool is_stopped = false;
  double queued_since = -1.0;        // time it joined the queue on the current road
  bool measured = true;              // false for warm-up vehicles
  int reroutes = 0;

  [[nodiscard]] RoadId next_road() const {
    return route_index + 1 < route.size() ? route[route_index + 1] : kInvalidRoad;
  }
  [[nodiscard]] bool on_last_road() const { return route_index + 1 >= route.size(); }
};

}  // namespace traffic
