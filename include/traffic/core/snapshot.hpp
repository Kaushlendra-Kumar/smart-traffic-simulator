#pragma once
// Compact, self-contained copy of what renderers and dashboards need. Produced on
// the simulation thread; afterwards it is immutable and shared read-only
// (std::shared_ptr<const SimulationSnapshot>) so readers never touch engine state.

#include <cstdint>
#include <string>
#include <vector>

#include "traffic/core/types.hpp"
#include "traffic/metrics/metrics_collector.hpp"

namespace traffic {

struct VehicleView {
  VehicleId id = 0;
  float x = 0, y = 0;   // centre of the vehicle, world metres
  float heading = 0;    // radians, 0 = east, counter-clockwise
  float speed = 0;      // m/s
  float length = 4.5f;
  VehicleType type = VehicleType::Car;
  bool stopped = false;
};

struct PedestrianView {
  PedestrianId id = 0;
  float x = 0, y = 0;
  bool crossing = false;
};

struct SignalView {
  NodeId node = kInvalidNode;
  int phase = 0;
  int next_phase = 0;
  SignalStage stage = SignalStage::Green;
  float time_in_stage = 0;
  std::vector<LightState> movements;  // same order as Node::movements
  std::vector<WalkState> crosswalks;  // same order as Node::crosswalks
  std::string reason;                 // controller's most recent explanation
};

struct ApproachQueue {
  RoadId road = kInvalidRoad;  // incoming road
  int queued = 0;
};

struct BlockageView {
  RoadId road = kInvalidRoad;
  int lane = 0;
  double position = 0;
};

struct SimulationSnapshot {
  std::uint64_t step = 0;
  double time = 0.0;
  std::vector<VehicleView> vehicles;
  std::vector<PedestrianView> pedestrians;
  std::vector<SignalView> signals;
  std::vector<ApproachQueue> queues;
  std::vector<RoadId> closed_roads;
  std::vector<BlockageView> blockages;
  MetricsSummary metrics;
  double demand_multiplier = 1.0;
  double pedestrian_multiplier = 1.0;
  double demand_vph = 0.0;
  std::string controller;
};

}  // namespace traffic
