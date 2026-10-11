#include <gtest/gtest.h>

#include <algorithm>

#include "test_util.hpp"
#include "traffic/network/network_builder.hpp"
#include "traffic/network/routing.hpp"

using namespace traffic;

namespace {
NetworkSpec four_arm(int lanes = 2) { return test::single_intersection(0, lanes).network; }

bool has_error_containing(const std::vector<std::string>& errs, const std::string& needle) {
  return std::any_of(errs.begin(), errs.end(), [&](const std::string& e) { return e.find(needle) != std::string::npos; });
}
}  // namespace

TEST(Network, FourArmJunctionTopology) {
  auto res = build_network(four_arm());
  ASSERT_TRUE(res.ok()) << res.errors.front();
  const auto& net = res.network;
  EXPECT_EQ(net.nodes.size(), 5u);
  EXPECT_EQ(net.roads.size(), 8u);       // 4 two-way roads
  EXPECT_EQ(net.movements.size(), 12u);  // 4 approaches x 3 turns, no U-turns
  EXPECT_TRUE(net.node(0).signalized);
  EXPECT_FALSE(net.node(1).signalized);
  EXPECT_TRUE(net.node(1).boundary);
  EXPECT_EQ(net.node(0).crosswalks.size(), 4u);
  int left = 0, right = 0, straight = 0;
  for (const auto& m : net.movements) {
    left += m.turn == TurnType::Left;
    right += m.turn == TurnType::Right;
    straight += m.turn == TurnType::Straight;
  }
  EXPECT_EQ(left, 4);
  EXPECT_EQ(right, 4);
  EXPECT_EQ(straight, 4);
}

TEST(Network, TurnClassificationRightHandTraffic) {
  auto res = build_network(four_arm());
  const auto& net = res.network;
  // Southbound vehicles (from N=1) turning towards E=2 make a LEFT turn.
  const RoadId in = net.find_road(1, 0);
  EXPECT_EQ(net.movement(net.find_movement(in, net.find_road(0, 2))).turn, TurnType::Left);
  EXPECT_EQ(net.movement(net.find_movement(in, net.find_road(0, 4))).turn, TurnType::Right);
  EXPECT_EQ(net.movement(net.find_movement(in, net.find_road(0, 3))).turn, TurnType::Straight);
  // Lanes of a two-way road sit to the right of the direction of travel.
  const Road& south = net.road(in);  // heading south (dir = (0,-1)); right = west (-x)
  EXPECT_LT(south.lane_point(0, 10).x, 0.0);
}

TEST(Network, RoadsAreTrimmedToJunctionBoxes) {
  auto res = build_network(four_arm());
  const auto& net = res.network;
  const Road& r = net.road(net.find_road(1, 0));
  EXPECT_NEAR(r.length, 300.0 - net.node(0).radius, 1e-9);
  EXPECT_GT(net.node(0).radius, 7.0);
}

TEST(Network, GridExpansion) {
  GridSpec g;
  g.rows = 2;
  g.cols = 3;
  const auto spec = expand_grid(g);
  EXPECT_EQ(spec.nodes.size(), 6u + 2 * 3 + 2 * 2);
  auto res = build_network(NetworkSpec{g, {}, {}, 3.5});
  ASSERT_TRUE(res.ok()) << res.errors.front();
  EXPECT_EQ(res.network.signalized_nodes().size(), 6u);
  EXPECT_EQ(res.network.boundary_nodes().size(), 10u);
  // interior node (1,2) has id 5 and sits at (400, -200)
  EXPECT_EQ(res.network.node(5).pos, (Vec2{400, -200}));
}

TEST(NetworkValidation, RejectsDuplicateAndUnknownNodes) {
  NetworkSpec s = four_arm();
  s.nodes.push_back({2, 5, 5, {}});
  EXPECT_TRUE(has_error_containing(build_network(s).errors, "duplicate node id 2"));

  s = four_arm();
  s.roads.push_back({0, 9, 1, 50, true, {}, ""});
  EXPECT_TRUE(has_error_containing(build_network(s).errors, "unknown node"));
}

TEST(NetworkValidation, RejectsDuplicateRoads) {
  NetworkSpec s = four_arm();
  s.roads.push_back(s.roads.front());
  EXPECT_TRUE(has_error_containing(build_network(s).errors, "duplicate road"));
}

TEST(NetworkValidation, RejectsUnsignalisedMultiArmNode) {
  NetworkSpec s = four_arm();
  s.nodes[0].signalized = false;
  EXPECT_TRUE(has_error_containing(build_network(s).errors, "unsignalised"));
}

TEST(NetworkValidation, RejectsDeadEndsAndShortRoads) {
  NetworkSpec s;
  s.nodes = {{0, 0, 0, {}}, {1, 100, 0, {}}, {2, 200, 0, {}}};
  s.roads = {{0, 1, 1, 50, false, {}, ""}, {1, 2, 1, 50, false, {}, ""}};
  // 1 -> 2 is fine but node 2 has only an incoming road and is a boundary sink; 0 is a source.
  EXPECT_TRUE(build_network(s).ok());
  s.roads[1] = {2, 1, 1, 50, false, {}, ""};  // now node 1 only receives traffic: dead end
  EXPECT_FALSE(build_network(s).ok());

  NetworkSpec tiny = four_arm();
  tiny.nodes[1].y = 14;  // shorter than the junction box
  EXPECT_TRUE(has_error_containing(build_network(tiny).errors, "too short"));
}

TEST(Routing, ShortestRouteAndClosureDetour) {
  auto res = build_network(NetworkSpec{GridSpec{2, 2, 200, 200, 2, 50}, {}, {}, 3.5});
  ASSERT_TRUE(res.ok());
  auto& net = res.network;
  // Boundary nodes: N edge = 4,5 ; S edge = 6,7 ; W edge = 8,9 ; E edge = 10,11
  const NodeId origin = 4;       // above interior node 0
  const NodeId destination = 7;  // below interior node 3
  auto route = route_between(net, origin, destination, free_flow_cost);
  ASSERT_TRUE(route.has_value());
  EXPECT_EQ(route->size(), 4u);  // stub in, two blocks, stub out
  EXPECT_EQ(net.road(route->front()).from, origin);
  EXPECT_EQ(net.road(route->back()).to, destination);
  for (std::size_t i = 0; i + 1 < route->size(); ++i)
    EXPECT_NE(net.find_movement(route->at(i), route->at(i + 1)), kInvalidMovement);

  // Close every road used by the route between interior nodes; a detour must appear.
  for (std::size_t i = 1; i + 1 < route->size(); ++i) net.road(route->at(i)).closed = true;
  auto detour = route_between(net, origin, destination, free_flow_cost);
  ASSERT_TRUE(detour.has_value());
  for (RoadId r : *detour) EXPECT_FALSE(net.road(r).closed);
  EXPECT_NE(*detour, *route);  // symmetric grid: same length, different roads
}

TEST(Routing, NoRouteWhenDisconnected) {
  auto res = build_network(four_arm());
  auto& net = res.network;
  net.road(net.find_road(0, 3)).closed = true;
  EXPECT_FALSE(route_between(net, 1, 3, free_flow_cost).has_value());
  EXPECT_TRUE(route_between(net, 1, 2, free_flow_cost).has_value());
}

TEST(Routing, DistanceVersusTimeCanDiffer) {
  // Two paths from A to D: short but slow (20 km/h) vs long but fast (100 km/h).
  NetworkSpec s;
  s.nodes = {{0, 0, 0, {}}, {1, 200, 0, {}}, {2, 200, 400, {}}, {3, 400, 0, {}}, {4, 600, 0, {}}};
  s.roads = {{0, 1, 1, 50, false, {}, ""}, {1, 3, 1, 20, false, {}, ""}, {1, 2, 1, 100, false, {}, ""},
             {2, 3, 1, 100, false, {}, ""}, {3, 4, 1, 50, false, {}, ""}};
  auto res = build_network(s);
  ASSERT_TRUE(res.ok()) << (res.errors.empty() ? "" : res.errors.front());
  auto by_distance = route_between(res.network, 0, 4, distance_cost);
  auto by_time = route_between(res.network, 0, 4, free_flow_cost);
  ASSERT_TRUE(by_distance && by_time);
  EXPECT_EQ(by_distance->size(), 3u);
  EXPECT_EQ(by_time->size(), 4u);
}

TEST(Lanes, DisciplineDependsOnLaneCountAndAvailableTurns) {
  auto two = build_network(four_arm(2)).network;
  const Road& r2 = two.road(two.find_road(1, 0));
  EXPECT_TRUE(two.lane_permits(r2, 0, TurnType::Left));
  EXPECT_TRUE(two.lane_permits(r2, 0, TurnType::Straight));
  EXPECT_FALSE(two.lane_permits(r2, 0, TurnType::Right));
  EXPECT_TRUE(two.lane_permits(r2, 1, TurnType::Right));
  EXPECT_FALSE(two.lane_permits(r2, 1, TurnType::Left));

  auto three = build_network(four_arm(3)).network;
  const Road& r3 = three.road(three.find_road(1, 0));
  EXPECT_TRUE(three.lane_permits(r3, 0, TurnType::Left));
  EXPECT_FALSE(three.lane_permits(r3, 0, TurnType::Straight));  // dedicated left lane
  EXPECT_TRUE(three.lane_permits(r3, 1, TurnType::Straight));
  EXPECT_FALSE(three.lane_permits(r3, 2, TurnType::Straight));  // dedicated right lane
}
