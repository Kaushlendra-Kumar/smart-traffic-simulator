#include "traffic/network/routing.hpp"

#include <algorithm>
#include <limits>
#include <queue>

namespace traffic {

double free_flow_cost(const Road& road) { return road.length / std::max(road.speed_limit, 0.5); }
double distance_cost(const Road& road) { return road.length; }

std::optional<std::vector<RoadId>> shortest_route(const RoadNetwork& net,
                                                  const std::vector<RoadId>& first_roads,
                                                  NodeId destination, const RoadCostFn& cost,
                                                  bool allow_closed_first) {
  const std::size_t n = net.roads.size();
  constexpr double inf = std::numeric_limits<double>::infinity();
  std::vector<double> dist(n, inf);
  std::vector<RoadId> prev(n, kInvalidRoad);
  using Item = std::pair<double, RoadId>;  // (cost, road): ties broken by smaller road id
  std::priority_queue<Item, std::vector<Item>, std::greater<>> pq;

  for (RoadId r : first_roads) {
    const Road& road = net.road(r);
    if (road.closed && !allow_closed_first) continue;
    const double c = cost(road);
    if (c < dist[static_cast<std::size_t>(r)]) {
      dist[static_cast<std::size_t>(r)] = c;
      pq.push({c, r});
    }
  }
  RoadId best_end = kInvalidRoad;
  while (!pq.empty()) {
    auto [d, r] = pq.top();
    pq.pop();
    if (d > dist[static_cast<std::size_t>(r)]) continue;
    const Road& road = net.road(r);
    if (road.to == destination) {
      best_end = r;
      break;
    }
    for (const auto& [out, mid] : road.next) {
      const Road& next = net.road(out);
      if (next.closed) continue;
      const Movement& m = net.movement(mid);
      const double turn_cost = m.connectors.front().path.length() / std::max(m.turn_speed, 0.5);
      const double nd = d + turn_cost + cost(next);
      if (nd < dist[static_cast<std::size_t>(out)]) {
        dist[static_cast<std::size_t>(out)] = nd;
        prev[static_cast<std::size_t>(out)] = r;
        pq.push({nd, out});
      }
    }
  }
  if (best_end == kInvalidRoad) return std::nullopt;
  std::vector<RoadId> route;
  for (RoadId r = best_end; r != kInvalidRoad; r = prev[static_cast<std::size_t>(r)]) route.push_back(r);
  std::reverse(route.begin(), route.end());
  return route;
}

std::optional<std::vector<RoadId>> route_between(const RoadNetwork& net, NodeId origin,
                                                 NodeId destination, const RoadCostFn& cost) {
  if (origin == destination) return std::nullopt;
  return shortest_route(net, net.node(origin).out, destination, cost);
}

std::vector<bool> nodes_reaching(const RoadNetwork& net, NodeId target) {
  std::vector<bool> seen(net.nodes.size(), false);
  std::queue<NodeId> q;
  seen[static_cast<std::size_t>(target)] = true;
  q.push(target);
  while (!q.empty()) {
    const NodeId n = q.front();
    q.pop();
    for (RoadId r : net.node(n).in) {
      const NodeId f = net.road(r).from;
      if (!seen[static_cast<std::size_t>(f)]) {
        seen[static_cast<std::size_t>(f)] = true;
        q.push(f);
      }
    }
  }
  return seen;
}

}  // namespace traffic
