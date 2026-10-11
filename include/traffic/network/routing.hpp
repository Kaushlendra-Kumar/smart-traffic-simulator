#pragma once
#include <functional>
#include <optional>
#include <vector>

#include "traffic/network/road_network.hpp"

namespace traffic {

/// Cost (seconds or metres) of traversing a road. Must be non-negative.
using RoadCostFn = std::function<double(const Road&)>;

double free_flow_cost(const Road& road);
double distance_cost(const Road& road);

/// Edge-based Dijkstra: nodes of the search are roads, so turn restrictions
/// (missing movements) are respected automatically. Returns the sequence of
/// roads starting with one of `first_roads` and ending with a road whose
/// downstream node is `destination`. Closed roads are never used, except that
/// `first_roads` themselves may be closed when `allow_closed_first` is set (a
/// vehicle already on a road can always finish it). Ties are broken by road id,
/// so results are deterministic.
std::optional<std::vector<RoadId>> shortest_route(const RoadNetwork& net,
                                                  const std::vector<RoadId>& first_roads,
                                                  NodeId destination, const RoadCostFn& cost,
                                                  bool allow_closed_first = false);

/// Convenience: route from a boundary origin node to a destination node.
std::optional<std::vector<RoadId>> route_between(const RoadNetwork& net, NodeId origin,
                                                 NodeId destination, const RoadCostFn& cost);

/// BFS over nodes ignoring closures: which nodes can reach `target`?
std::vector<bool> nodes_reaching(const RoadNetwork& net, NodeId target);

}  // namespace traffic
