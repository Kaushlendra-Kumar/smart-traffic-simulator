#include "traffic/network/road_network.hpp"

#include <cmath>
#include <numbers>
#include <set>

#include "traffic/network/routing.hpp"

namespace traffic {

double Road::lane_offset(int lane) const {
  const double n = static_cast<double>(lanes.size());
  if (reverse != kInvalidRoad) return (lane + 0.5) * lane_width;  // right-hand traffic
  return (lane + 0.5 - n / 2.0) * lane_width;                    // one-way: centred
}

Vec2 Road::lane_point(int lane, double s) const {
  return start + dir * s + dir.right_normal() * lane_offset(lane);
}

RoadId RoadNetwork::find_road(NodeId from, NodeId to) const {
  if (from < 0 || static_cast<std::size_t>(from) >= nodes.size()) return kInvalidRoad;
  for (RoadId r : node(from).out)
    if (road(r).to == to) return r;
  return kInvalidRoad;
}

MovementId RoadNetwork::find_movement(RoadId in, RoadId out) const {
  for (const auto& [o, m] : road(in).next)
    if (o == out) return m;
  return kInvalidMovement;
}

std::vector<NodeId> RoadNetwork::boundary_nodes() const {
  std::vector<NodeId> out;
  for (const auto& n : nodes)
    if (n.boundary) out.push_back(n.id);
  return out;
}

std::vector<NodeId> RoadNetwork::signalized_nodes() const {
  std::vector<NodeId> out;
  for (const auto& n : nodes)
    if (n.signalized) out.push_back(n.id);
  return out;
}

bool RoadNetwork::lane_permits(const Road& road, int lane, TurnType turn) const {
  const int n = road.lane_count();
  if (n <= 1) return true;
  const bool inner = lane == 0;
  const bool outer = lane == n - 1;
  switch (turn) {
    case TurnType::Left:
    case TurnType::UTurn: return inner;
    case TurnType::Right: return outer;
    case TurnType::Straight:
      if (n == 2) return true;
      if (!inner && !outer) return true;
      return (inner && !road.has_left) || (outer && !road.has_right);
  }
  return false;
}

int RoadNetwork::connector_out_lane(TurnType turn, int in_lane, int out_lane_count) {
  switch (turn) {
    case TurnType::Left:
    case TurnType::UTurn: return 0;
    case TurnType::Right: return out_lane_count - 1;
    case TurnType::Straight: return std::min(in_lane, out_lane_count - 1);
  }
  return 0;
}

std::vector<std::string> RoadNetwork::validate() const {
  std::vector<std::string> errors;
  std::set<std::pair<NodeId, NodeId>> seen;
  for (const auto& r : roads) {
    if (!seen.insert({r.from, r.to}).second)
      errors.push_back("duplicate road " + std::to_string(r.from) + "->" + std::to_string(r.to));
    if (r.length < 5.0)
      errors.push_back("road " + std::to_string(r.from) + "->" + std::to_string(r.to) +
                       " is too short after trimming junction boxes (" +
                       std::to_string(r.length) + " m)");
    if (r.lanes.empty() || r.lanes.size() > 5)
      errors.push_back("road " + std::to_string(r.id) + " must have 1..5 lanes");
    if (!(r.speed_limit > 0.0))
      errors.push_back("road " + std::to_string(r.id) + " has a non-positive speed limit");
    if (!node(r.to).boundary && r.next.empty())
      errors.push_back("road " + std::to_string(r.from) + "->" + std::to_string(r.to) +
                       " is a dead end (no movement leaves node " + std::to_string(r.to) + ")");
  }
  for (const auto& n : nodes) {
    if (n.in.empty() && n.out.empty())
      errors.push_back("node " + std::to_string(n.id) + " is isolated");
    if (n.arms.size() >= 3 && !n.signalized)
      errors.push_back("node " + std::to_string(n.id) +
                       " has 3+ arms but is unsignalised (unsupported: priority rules are not modelled)");
  }
  // Every boundary node that emits traffic must reach at least one other boundary node.
  const auto boundaries = boundary_nodes();
  for (NodeId o : boundaries) {
    if (node(o).out.empty()) continue;
    bool any = false;
    for (NodeId d : boundaries) {
      if (d == o || node(d).in.empty()) continue;
      if (route_between(*this, o, d, distance_cost)) {
        any = true;
        break;
      }
    }
    if (!any) errors.push_back("boundary node " + std::to_string(o) + " cannot reach any destination");
  }
  return errors;
}

std::string compass_label(Vec2 d) {
  static const char* names[] = {"E", "NE", "N", "NW", "W", "SW", "S", "SE"};
  double ang = std::atan2(d.y, d.x);  // 0 = east, counter-clockwise
  if (ang < 0) ang += 2 * std::numbers::pi;
  const int idx = static_cast<int>(std::lround(ang / (std::numbers::pi / 4))) % 8;
  return names[idx];
}

}  // namespace traffic
