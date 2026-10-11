#include <gtest/gtest.h>

#include <cmath>

#include "traffic/vehicles/vehicle.hpp"

using namespace traffic;

TEST(Idm, AcceleratesFromRestOnFreeRoad) {
  IdmParams p;
  EXPECT_NEAR(idm_acceleration(p, 0.0, 14.0, 1e9, 0.0), p.max_accel, 1e-9);
}

TEST(Idm, ZeroAccelerationAtDesiredSpeed) {
  IdmParams p;
  EXPECT_NEAR(idm_acceleration(p, 14.0, 14.0, 1e9, 0.0), 0.0, 1e-9);
  EXPECT_LT(idm_acceleration(p, 16.0, 14.0, 1e9, 0.0), 0.0);  // above the limit: slow down
}

TEST(Idm, BrakesWhenClosingOnSlowerLeader) {
  IdmParams p;
  const double a = idm_acceleration(p, 14.0, 14.0, 20.0, 14.0);  // leader stopped 20 m ahead
  EXPECT_LT(a, -p.comfortable_decel);
  EXPECT_GE(a, -kMaxBraking);
}

TEST(Idm, EquilibriumGapGrowsWithSpeed) {
  IdmParams p;
  auto equilibrium_gap = [&](double v, double v0) {
    // solve a(gap)=0 with dv=0 by bisection
    double lo = 0.1, hi = 500;
    for (int i = 0; i < 200; ++i) {
      const double mid = 0.5 * (lo + hi);
      (idm_acceleration(p, v, v0, mid, 0.0) < 0 ? lo : hi) = mid;
    }
    return lo;
  };
  const double g5 = equilibrium_gap(5, 14), g10 = equilibrium_gap(10, 14);
  EXPECT_GT(g10, g5);
  EXPECT_GT(g5, p.min_gap + 5 * p.time_headway);  // at least s0 + vT
}

TEST(Idm, VehicleStopsBehindObstacleWithoutCollision) {
  // Integrate one vehicle approaching a stopped obstacle with the ballistic scheme.
  IdmParams p;
  double s = 0, v = 13.9;
  const double obstacle = 120.0, dt = 0.1;
  double min_gap = 1e9;
  for (int i = 0; i < 1000; ++i) {
    const double a = idm_acceleration(p, v, 13.9, obstacle - s, v);
    const double nv = v + a * dt;
    if (nv < 0) {
      s += -v * v / (2 * a);
      v = 0;
    } else {
      s += v * dt + 0.5 * a * dt * dt;
      v = nv;
    }
    min_gap = std::min(min_gap, obstacle - s);
    ASSERT_GE(v, 0.0);
  }
  EXPECT_LT(v, 0.05);
  EXPECT_GT(min_gap, 0.5);         // never touches the obstacle
  EXPECT_NEAR(obstacle - s, p.min_gap, 0.6);  // ends roughly s0 behind it
}

TEST(Idm, VehicleClassesDiffer) {
  EXPECT_GT(default_params(VehicleType::Bus).length, default_params(VehicleType::Car).length);
  EXPECT_LT(default_params(VehicleType::Bus).max_accel, default_params(VehicleType::Car).max_accel);
  EXPECT_GT(default_params(VehicleType::Emergency).desired_speed_factor, 1.0);
}
