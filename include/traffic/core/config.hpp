#pragma once
// Immutable scenario description. Everything needed to reproduce a run lives here
// (plus the seed and the ordered command log). Runtime state lives in SimulationEngine.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "traffic/core/types.hpp"

namespace traffic {

struct NodeSpec {
  NodeId id = kInvalidNode;
  double x = 0.0;
  double y = 0.0;
  /// nullopt => automatic: nodes with >= 3 arms are signalised.
  std::optional<bool> signalized;
};

struct RoadSpec {
  NodeId from = kInvalidNode;
  NodeId to = kInvalidNode;
  int lanes = 2;
  double speed_limit_kmh = 50.0;
  bool two_way = true;
  /// Lane count of the reverse direction; nullopt => same as `lanes`.
  std::optional<int> lanes_reverse;
  std::string name;
};

struct GridSpec {
  int rows = 1;
  int cols = 1;
  double block_length = 200.0;  // distance between adjacent intersections
  double stub_length = 200.0;   // distance from edge intersection to boundary node
  int lanes = 2;
  double speed_limit_kmh = 50.0;
};

struct NetworkSpec {
  /// If set, the network is generated; otherwise nodes/roads are used verbatim.
  std::optional<GridSpec> grid;
  std::vector<NodeSpec> nodes;
  std::vector<RoadSpec> roads;
  double lane_width = 3.5;
};

struct SourceSpec {
  NodeId node = kInvalidNode;
  double rate_vph = 300.0;  // vehicles per hour before multipliers
  /// destination node -> relative weight. Empty => all other boundary nodes equally.
  std::map<NodeId, double> destinations;
};

struct DemandSpec {
  std::vector<SourceSpec> sources;
  /// Used for boundary nodes that have no explicit SourceSpec. 0 disables them.
  double default_rate_vph = 300.0;
  /// Piecewise-linear multiplier over simulation time: (time_s, multiplier).
  std::vector<std::pair<double, double>> profile;
  /// Probability of each vehicle class (car, bus, emergency); normalised at load.
  std::array<double, kVehicleTypeCount> vehicle_mix{0.92, 0.06, 0.02};
};

struct PedestrianSpec {
  double rate_per_hour = 120.0;  // per signalised intersection, spread over crosswalks
  double walk_speed_min = 1.0;   // m/s
  double walk_speed_max = 1.5;
  double walk_time = 7.0;        // WALK indication length (s)
  double clearance_speed = 1.0;  // m/s used to size the flashing clearance interval
};

struct PhaseSpec {
  std::string name;
  /// Movements as (from_node, to_node) pairs relative to the intersection node.
  std::vector<std::pair<NodeId, NodeId>> movements;
  /// Crosswalks named by the neighbouring node of the arm they cross.
  std::vector<NodeId> crosswalks;
  std::optional<double> min_green, max_green, green;
};

struct IntersectionSignalSpec {
  NodeId node = kInvalidNode;
  std::vector<PhaseSpec> phases;  // empty => generated automatically
};

struct SignalTimingSpec {
  double min_green = 7.0;
  double max_green = 60.0;
  double yellow = 3.5;
  double all_red = 1.5;
  double fixed_green = 20.0;  // default green duration used by the fixed-time controller
  /// Automatic phase generation: "split" (one phase per approach), "protected"
  /// (NEMA-style protected through/left groups by graph colouring) or "auto"
  /// (protected only if every approach has >= 3 lanes, otherwise split).
  std::string phasing = "auto";
  std::vector<IntersectionSignalSpec> intersections;
};

struct ControllerConfig {
  std::string type = "fixed_time";  // fixed_time | adaptive | max_pressure
  std::string label;                // display name; defaults to type

  // fixed-time
  /// Optional green durations per phase (applied to every intersection whose phase count matches).
  std::vector<double> greens;
  /// Optional time-of-day plans: (start_time_s, greens).
  std::vector<std::pair<double, std::vector<double>>> plans;
  /// Per-intersection overrides of the green durations.
  std::map<NodeId, std::vector<double>> node_greens;

  // adaptive
  double w_queue = 1.0;
  double w_wait = 1.0;      // per minute of accumulated wait
  double w_oldest = 1.5;    // per 30 s of oldest wait
  double w_arrivals = 0.5;
  double w_pedestrian = 1.0;
  double hysteresis_ratio = 0.25;
  double hysteresis_abs = 2.0;
  double gap_out_seconds = 3.0;
  double max_wait = 90.0;            // starvation guard (s)
  double max_pedestrian_wait = 90.0; // starvation guard for pedestrians (s)
  bool emergency_priority = true;

  // max-pressure
  double decision_interval = 5.0;

  [[nodiscard]] std::string display_name() const { return label.empty() ? type : label; }
};

struct EventSpec {
  double time = 0.0;
  /// close_road | open_road | block_lane | clear_lane | set_demand | set_pedestrian_demand
  std::string action;
  NodeId from = kInvalidNode;  // road identification by its endpoints
  NodeId to = kInvalidNode;
  int lane = 0;
  double position = 0.0;  // metres from the start of the lane
  double value = 1.0;     // multiplier for set_* events
};

enum class RoutingMode : std::uint8_t { FreeFlowTime, Distance, CongestionAware };

struct ScenarioConfig {
  std::string id = "untitled";
  std::string name = "Untitled scenario";
  std::string description;
  double dt = 0.1;
  std::uint64_t seed = 1;
  double duration = 900.0;  // default length for experiments (s)
  double warmup = 120.0;    // metrics ignore vehicles that arrive before this time
  NetworkSpec network;
  DemandSpec demand;
  PedestrianSpec pedestrians;
  SignalTimingSpec signals;
  ControllerConfig controller;
  std::vector<EventSpec> events;
  RoutingMode routing = RoutingMode::FreeFlowTime;
};

}  // namespace traffic
