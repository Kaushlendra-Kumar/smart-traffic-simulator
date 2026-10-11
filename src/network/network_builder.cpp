#include "traffic/network/network_builder.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>

namespace traffic {

NetworkSpec expand_grid(const GridSpec& g) {
  NetworkSpec spec;
  const int rows = std::max(1, g.rows);
  const int cols = std::max(1, g.cols);
  auto interior = [cols](int r, int c) { return r * cols + c; };
  // Row 0 is the northern-most row; y decreases with r.
  for (int r = 0; r < rows; ++r)
    for (int c = 0; c < cols; ++c)
      spec.nodes.push_back({interior(r, c), c * g.block_length, -r * g.block_length, std::nullopt});

  NodeId next = rows * cols;
  auto add_road = [&](NodeId a, NodeId b) {
    RoadSpec rs;
    rs.from = a;
    rs.to = b;
    rs.lanes = g.lanes;
    rs.speed_limit_kmh = g.speed_limit_kmh;
    rs.two_way = true;
    spec.roads.push_back(rs);
  };
  auto add_boundary = [&](double x, double y, NodeId attach) {
    spec.nodes.push_back({next, x, y, false});
    add_road(next, attach);
    ++next;
  };
  for (int c = 0; c < cols; ++c) add_boundary(c * g.block_length, g.stub_length, interior(0, c));
  for (int c = 0; c < cols; ++c)
    add_boundary(c * g.block_length, -(rows - 1) * g.block_length - g.stub_length, interior(rows - 1, c));
  for (int r = 0; r < rows; ++r) add_boundary(-g.stub_length, -r * g.block_length, interior(r, 0));
  for (int r = 0; r < rows; ++r)
    add_boundary((cols - 1) * g.block_length + g.stub_length, -r * g.block_length, interior(r, cols - 1));

  for (int r = 0; r < rows; ++r)
    for (int c = 0; c + 1 < cols; ++c) add_road(interior(r, c), interior(r, c + 1));
  for (int r = 0; r + 1 < rows; ++r)
    for (int c = 0; c < cols; ++c) add_road(interior(r, c), interior(r + 1, c));
  return spec;
}

namespace {

TurnType classify_turn(Vec2 din, Vec2 dout) {
  const double ang = std::atan2(cross(din, dout), dot(din, dout));  // + = counter-clockwise
  const double deg = ang * 180.0 / std::numbers::pi;
  if (std::abs(deg) < 35.0) return TurnType::Straight;
  if (std::abs(deg) > 165.0) return TurnType::UTurn;
  return deg > 0 ? TurnType::Left : TurnType::Right;
}

}  // namespace

BuildResult build_network(const NetworkSpec& input) {
  BuildResult result;
  auto& errors = result.errors;
  RoadNetwork& net = result.network;

  const NetworkSpec spec = input.grid ? expand_grid(*input.grid) : input;
  if (spec.nodes.empty()) {
    errors.push_back("network has no nodes");
    return result;
  }
  // --- nodes: ids must be unique and dense 0..N-1 (keeps lookups O(1) and deterministic)
  std::set<NodeId> ids;
  for (const auto& n : spec.nodes) {
    if (!ids.insert(n.id).second) errors.push_back("duplicate node id " + std::to_string(n.id));
  }
  if (!errors.empty()) return result;
  if (*ids.begin() != 0 || *ids.rbegin() != static_cast<NodeId>(spec.nodes.size()) - 1) {
    errors.push_back("node ids must be dense in 0..N-1");
    return result;
  }
  net.nodes.resize(spec.nodes.size());
  for (const auto& n : spec.nodes) {
    Node& node = net.node(n.id);
    node.id = n.id;
    node.pos = {n.x, n.y};
  }

  // --- roads
  const double w = spec.lane_width > 0 ? spec.lane_width : 3.5;
  auto make_road = [&](NodeId from, NodeId to, int lanes, double kmh, const std::string& name) -> RoadId {
    Road r;
    r.id = static_cast<RoadId>(net.roads.size());
    r.from = from;
    r.to = to;
    r.lane_width = w;
    r.speed_limit = kmh / 3.6;
    r.name = name;
    for (int i = 0; i < std::clamp(lanes, 1, 5); ++i) r.lanes.push_back(Lane{r.id, i, {}, std::nullopt, {}});
    if (lanes < 1 || lanes > 5) errors.push_back("road " + std::to_string(from) + "->" + std::to_string(to) + " must have 1..5 lanes");
    net.roads.push_back(std::move(r));
    return net.roads.back().id;
  };
  for (const auto& rs : spec.roads) {
    if (!ids.contains(rs.from) || !ids.contains(rs.to)) {
      errors.push_back("road references unknown node " + std::to_string(rs.from) + "->" + std::to_string(rs.to));
      continue;
    }
    if (rs.from == rs.to) {
      errors.push_back("self-loop road at node " + std::to_string(rs.from));
      continue;
    }
    if (!(rs.speed_limit_kmh > 0)) {
      errors.push_back("road " + std::to_string(rs.from) + "->" + std::to_string(rs.to) + " needs a positive speed limit");
      continue;
    }
    const RoadId a = make_road(rs.from, rs.to, rs.lanes, rs.speed_limit_kmh, rs.name);
    if (rs.two_way) {
      const RoadId b = make_road(rs.to, rs.from, rs.lanes_reverse.value_or(rs.lanes), rs.speed_limit_kmh, rs.name);
      net.road(a).reverse = b;
      net.road(b).reverse = a;
    }
  }
  if (!errors.empty()) return result;

  for (const auto& r : net.roads) {
    net.node(r.from).out.push_back(r.id);
    net.node(r.to).in.push_back(r.id);
  }

  // --- arms, boundary flags, signalisation, junction radius
  for (auto& node : net.nodes) {
    std::vector<NodeId> neighbors;
    for (RoadId r : node.out) neighbors.push_back(net.road(r).to);
    for (RoadId r : node.in) neighbors.push_back(net.road(r).from);
    std::sort(neighbors.begin(), neighbors.end());
    neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
    for (NodeId nb : neighbors) {
      Arm arm;
      arm.neighbor = nb;
      arm.in = net.find_road(nb, node.id);
      arm.out = net.find_road(node.id, nb);
      arm.direction = (net.node(nb).pos - node.pos).normalized();
      arm.compass = compass_label(arm.direction);
      node.arms.push_back(arm);
    }
    // Order arms clockwise starting from north for stable, readable phase labels.
    std::sort(node.arms.begin(), node.arms.end(), [](const Arm& a, const Arm& b) {
      auto key = [](const Arm& x) {
        double ang = std::atan2(x.direction.x, x.direction.y);  // 0 = north, clockwise
        if (ang < 0) ang += 2 * std::numbers::pi;
        return ang;
      };
      return key(a) < key(b);
    });
    node.boundary = node.arms.size() == 1;
    const auto& spec_node = *std::find_if(spec.nodes.begin(), spec.nodes.end(),
                                          [&](const NodeSpec& s) { return s.id == node.id; });
    node.signalized = !node.boundary && spec_node.signalized.value_or(node.arms.size() >= 3);
    double half_width = 0.0;
    for (const Arm& arm : node.arms) {
      int lanes = 0;
      if (arm.in != kInvalidRoad) lanes += net.road(arm.in).lane_count();
      if (arm.out != kInvalidRoad) lanes += net.road(arm.out).lane_count();
      half_width = std::max(half_width, lanes * w / 2.0);
    }
    if (node.boundary) node.radius = 0.0;
    else if (node.signalized) node.radius = half_width + 5.0;  // room for the crosswalk band
    else node.radius = half_width + 1.0;
  }

  // --- road geometry (trimmed to junction boxes)
  for (auto& r : net.roads) {
    const Vec2 a = net.node(r.from).pos;
    const Vec2 b = net.node(r.to).pos;
    r.dir = (b - a).normalized();
    r.start = a + r.dir * net.node(r.from).radius;
    r.end = b - r.dir * net.node(r.to).radius;
    r.length = (r.end - r.start).length();
    if (dot(r.end - r.start, r.dir) <= 0) r.length = 0;
  }

  // --- movements and connectors
  for (auto& node : net.nodes) {
    if (node.boundary) continue;
    for (const Arm& from_arm : node.arms) {
      if (from_arm.in == kInvalidRoad) continue;
      for (const Arm& to_arm : node.arms) {
        if (to_arm.out == kInvalidRoad || to_arm.neighbor == from_arm.neighbor) continue;  // no U-turns
        const Road& in = net.road(from_arm.in);
        const Road& out = net.road(to_arm.out);
        Movement m;
        m.id = static_cast<MovementId>(net.movements.size());
        m.node = node.id;
        m.in = in.id;
        m.out = out.id;
        m.turn = node.arms.size() <= 2 ? TurnType::Straight : classify_turn(in.dir, out.dir);
        const double limit = std::min(in.speed_limit, out.speed_limit);
        m.turn_speed = m.turn == TurnType::Left    ? std::min(limit, 9.0)
                       : m.turn == TurnType::Right ? std::min(limit, 7.0)
                                                   : limit;
        m.label = from_arm.compass + "→" + to_arm.compass + " (" + std::string(to_string(m.turn)) + ")";
        for (int i = 0; i < in.lane_count(); ++i) {
          Connector c;
          c.in_lane = i;
          c.out_lane = RoadNetwork::connector_out_lane(m.turn, i, out.lane_count());
          const Vec2 p0 = in.lane_point(i, in.length);
          const Vec2 p2 = out.lane_point(c.out_lane, 0.0);
          Vec2 ctrl = (p0 + p2) * 0.5;
          if (m.turn == TurnType::Left && node.arms.size() > 2) {
            ctrl = node.pos;  // opposing left turns pass each other without crossing
          } else if (m.turn != TurnType::Straight || node.arms.size() <= 2) {
            Vec2 x;
            if (line_intersection(p0, in.dir, p2, out.dir, x) && (x - node.pos).length() < 3 * node.radius + 5)
              ctrl = x;
          }
          c.path = quadratic_bezier(p0, ctrl, p2, 16);
          m.connectors.push_back(std::move(c));
        }
        node.movements.push_back(m.id);
        net.movements.push_back(std::move(m));
      }
    }
  }
  for (const auto& m : net.movements) {
    net.road(m.in).next.push_back({m.out, m.id});
    if (m.turn == TurnType::Left || m.turn == TurnType::UTurn) net.road(m.in).has_left = true;
    if (m.turn == TurnType::Right) net.road(m.in).has_right = true;
    for (const auto& c : m.connectors)
      net.road(m.out).lanes[static_cast<std::size_t>(c.out_lane)].feeders.push_back({m.id, c.in_lane});
  }

  // --- crosswalks at signalised nodes, one per arm
  for (auto& node : net.nodes) {
    if (!node.signalized) continue;
    for (const Arm& arm : node.arms) {
      double hw = 0.0;
      if (arm.in != kInvalidRoad) hw = std::max(hw, net.road(arm.in).lane_count() * w);
      if (arm.out != kInvalidRoad) hw = std::max(hw, net.road(arm.out).lane_count() * w);
      Crosswalk cw;
      cw.index = static_cast<int>(node.crosswalks.size());
      cw.arm_node = arm.neighbor;
      cw.road_in = arm.in;
      cw.road_out = arm.out;
      const Vec2 centre = node.pos + arm.direction * (node.radius - 2.5);
      const Vec2 perp = arm.direction.right_normal();
      cw.a = centre - perp * (hw + 1.0);
      cw.b = centre + perp * (hw + 1.0);
      cw.length = (cw.b - cw.a).length();
      node.crosswalks.push_back(cw);
    }
  }

  auto problems = net.validate();
  errors.insert(errors.end(), problems.begin(), problems.end());
  return result;
}

}  // namespace traffic
