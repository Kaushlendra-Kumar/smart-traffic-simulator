#pragma once
// Directed road graph. Nodes are intersections or boundary endpoints; roads are
// directed edges carrying one or more lanes; movements connect an incoming road
// to an outgoing road through an intersection via per-lane connector curves.
//
// Ownership: RoadNetwork owns all geometry and topology. It is built once from a
// NetworkSpec and is immutable afterwards except for the small runtime fields
// documented below (road closure flags, lane blockages, speed limits, and the
// vehicle occupancy lists, which the engine mutates on the simulation thread only).

#include <optional>
#include <string>
#include <vector>

#include "traffic/core/geometry.hpp"
#include "traffic/core/types.hpp"

namespace traffic {

struct Connector {
  int in_lane = 0;
  int out_lane = 0;
  Polyline path;
  /// Runtime: vehicles on this connector, front (closest to exit) first.
  std::vector<SlotIndex> vehicles;
};

struct Movement {
  MovementId id = kInvalidMovement;
  NodeId node = kInvalidNode;
  RoadId in = kInvalidRoad;
  RoadId out = kInvalidRoad;
  TurnType turn = TurnType::Straight;
  double turn_speed = 13.9;  // free speed inside the junction (m/s)
  std::vector<Connector> connectors;  // indexed by incoming lane
  std::string label;  // e.g. "N→E (L)"
};

struct Lane {
  RoadId road = kInvalidRoad;
  int index = 0;  // 0 = innermost (closest to the road centre line)
  /// Runtime: vehicles on this lane ordered front (largest s) first.
  std::vector<SlotIndex> vehicles;
  /// Runtime: a stationary blockage (incident) at this distance from lane start.
  std::optional<double> blockage;
  /// Connectors (movement id, connector index) that discharge into this lane.
  std::vector<std::pair<MovementId, int>> feeders;
};

struct Road {
  RoadId id = kInvalidRoad;
  NodeId from = kInvalidNode;
  NodeId to = kInvalidNode;
  RoadId reverse = kInvalidRoad;  // opposite-direction road, if two-way
  std::string name;
  double lane_width = 3.5;
  double speed_limit = 13.9;  // m/s (runtime-adjustable)
  Vec2 start, end;            // centre-line endpoints, trimmed to the junction boxes
  Vec2 dir;                   // unit direction from start to end
  double length = 0.0;
  bool closed = false;        // runtime: closed roads accept no new vehicles
  std::vector<Lane> lanes;
  /// Outgoing movements available at the downstream node: (out road, movement id).
  std::vector<std::pair<RoadId, MovementId>> next;
  bool has_left = false, has_right = false;  // turn availability at the downstream node

  [[nodiscard]] int lane_count() const { return static_cast<int>(lanes.size()); }
  /// Lateral offset (to the right of `dir`) of a lane centre.
  [[nodiscard]] double lane_offset(int lane) const;
  [[nodiscard]] Vec2 lane_point(int lane, double s) const;
};

struct Crosswalk {
  int index = 0;              // index within its intersection
  NodeId arm_node = kInvalidNode;  // neighbour node of the arm being crossed
  RoadId road_in = kInvalidRoad;   // road arriving from the arm (may be invalid)
  RoadId road_out = kInvalidRoad;  // road leaving towards the arm (may be invalid)
  Vec2 a, b;                  // endpoints (kerb to kerb)
  double length = 0.0;
};

struct Arm {
  NodeId neighbor = kInvalidNode;
  RoadId in = kInvalidRoad;   // road entering the intersection from this arm
  RoadId out = kInvalidRoad;  // road leaving the intersection to this arm
  Vec2 direction;             // unit vector from the node towards the neighbour
  std::string compass;        // N, NE, E, ...
};

struct Node {
  NodeId id = kInvalidNode;
  Vec2 pos;
  bool boundary = false;    // vehicles spawn/exit here
  bool signalized = false;
  double radius = 0.0;      // half-size of the junction box (roads are trimmed by this)
  std::vector<RoadId> in, out;
  std::vector<Arm> arms;
  std::vector<MovementId> movements;  // movements through this node
  std::vector<Crosswalk> crosswalks;  // only for signalised nodes
};

class RoadNetwork {
 public:
  std::vector<Node> nodes;          // indexed by NodeId (ids are dense 0..N-1)
  std::vector<Road> roads;          // indexed by RoadId
  std::vector<Movement> movements;  // indexed by MovementId

  [[nodiscard]] const Node& node(NodeId id) const { return nodes.at(static_cast<std::size_t>(id)); }
  [[nodiscard]] Node& node(NodeId id) { return nodes.at(static_cast<std::size_t>(id)); }
  [[nodiscard]] const Road& road(RoadId id) const { return roads.at(static_cast<std::size_t>(id)); }
  [[nodiscard]] Road& road(RoadId id) { return roads.at(static_cast<std::size_t>(id)); }
  [[nodiscard]] const Movement& movement(MovementId id) const {
    return movements.at(static_cast<std::size_t>(id));
  }
  [[nodiscard]] Movement& movement(MovementId id) { return movements.at(static_cast<std::size_t>(id)); }

  [[nodiscard]] RoadId find_road(NodeId from, NodeId to) const;
  [[nodiscard]] MovementId find_movement(RoadId in, RoadId out) const;
  [[nodiscard]] std::vector<NodeId> boundary_nodes() const;
  [[nodiscard]] std::vector<NodeId> signalized_nodes() const;

  /// Lane discipline: may a vehicle in `lane` of `road` perform `turn` at its end?
  ///   1 lane : everything
  ///   2 lanes: inner lane L/U/S, outer lane S/R
  ///   3+     : inner lane L/U, outer lane R, middle lanes S (an edge lane also permits S
  ///            when its turn does not exist at that junction)
  [[nodiscard]] bool lane_permits(const Road& road, int lane, TurnType turn) const;
  /// The target lane a connector uses for (turn, in_lane).
  [[nodiscard]] static int connector_out_lane(TurnType turn, int in_lane, int out_lane_count);

  /// Structural validation. Returns human-readable problems (empty => valid).
  [[nodiscard]] std::vector<std::string> validate() const;
};

std::string compass_label(Vec2 direction);

}  // namespace traffic
