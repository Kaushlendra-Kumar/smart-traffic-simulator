#include "traffic/infrastructure/snapshot_json.hpp"

#include <cmath>

#include "traffic/infrastructure/scenario_io.hpp"

namespace traffic::io {

using nlohmann::json;

namespace {
double r2(double x) { return std::round(x * 100.0) / 100.0; }
double r3(double x) { return std::round(x * 1000.0) / 1000.0; }
json pt(Vec2 p) { return json::array({r2(p.x), r2(p.y)}); }

char light_char(LightState s) {
  switch (s) {
    case LightState::Green: return 'G';
    case LightState::Yellow: return 'Y';
    case LightState::Red: return 'R';
  }
  return 'R';
}
char walk_char(WalkState s) {
  switch (s) {
    case WalkState::Walk: return 'W';
    case WalkState::Clearance: return 'C';
    case WalkState::DontWalk: return 'D';
  }
  return 'D';
}
}  // namespace

json metrics_to_json(const MetricsSummary& m) {
  json j{{"sim_time", r2(m.sim_time)},
         {"measured_seconds", r2(m.measured_seconds)},
         {"spawned", m.spawned},
         {"entered", m.entered},
         {"completed", m.completed},
         {"measured_completed", m.measured_completed},
         {"in_network", m.in_network},
         {"backlog", m.backlog},
         {"unserved", m.unserved},
         {"avg_travel_time", r2(m.avg_travel_time)},
         {"median_travel_time", r2(m.median_travel_time)},
         {"p95_travel_time", m.p95_travel_time ? json(r2(*m.p95_travel_time)) : json(nullptr)},
         {"avg_delay", r2(m.avg_delay)},
         {"avg_delay_all", r2(m.avg_delay_all)},
         {"total_delay_hours", r3(m.total_delay_hours)},
         {"avg_stopped_time", r2(m.avg_stopped_time)},
         {"avg_stops", r3(m.avg_stops)},
         {"avg_queue", r2(m.avg_queue)},
         {"max_queue", m.max_queue},
         {"throughput_vph", r2(m.throughput_vph)},
         {"peds_completed", m.peds_completed},
         {"ped_avg_wait", r2(m.ped_avg_wait)},
         {"ped_max_wait", r2(m.ped_max_wait)},
         {"phase_switches", m.phase_switches},
         {"clearance_fraction", r3(m.clearance_fraction)},
         {"red_light_guard_events", m.red_light_guard_events},
         {"following_violations", m.following_violations},
         {"forced_reroutes", m.forced_reroutes},
         {"lane_changes", m.lane_changes}};
  return j;
}

json series_to_json(const std::vector<TimeSample>& series) {
  json arr = json::array();
  for (const auto& s : series)
    arr.push_back({{"t", r2(s.t)},
                   {"in_network", s.in_network},
                   {"backlog", s.backlog},
                   {"avg_speed", r2(s.avg_speed)},
                   {"total_queue", s.total_queue},
                   {"completed", s.completed},
                   {"throughput_vph", r2(s.throughput_vph)},
                   {"peds_waiting", s.peds_waiting},
                   {"avg_delay", r2(s.avg_delay)}});
  return arr;
}

json intersections_to_json(const std::vector<IntersectionStats>& stats) {
  json arr = json::array();
  for (const auto& is : stats) {
    json phases = json::array();
    for (const auto& p : is.phases)
      phases.push_back({{"name", p.name}, {"green_time", r2(p.green_time)}, {"discharged", p.discharged},
                        {"arrivals", r2(p.arrivals)}});
    arr.push_back({{"node", is.node},
                   {"throughput", is.throughput},
                   {"avg_queue", r2(is.avg_queue)},
                   {"max_queue", is.max_queue},
                   {"switches", is.switches},
                   {"clearance_time", r2(is.clearance_time)},
                   {"phases", phases}});
  }
  return arr;
}

json network_to_json(const SimulationEngine& engine) {
  const RoadNetwork& net = engine.network();
  json j;
  j["v"] = kProtocolVersion;
  j["type"] = "network";
  j["scenario"] = {{"id", engine.scenario().id}, {"name", engine.scenario().name},
                   {"description", engine.scenario().description}, {"seed", engine.scenario().seed},
                   {"dt", engine.dt()}, {"duration", engine.scenario().duration}, {"warmup", engine.scenario().warmup}};
  double minx = 1e18, miny = 1e18, maxx = -1e18, maxy = -1e18;
  json nodes = json::array();
  for (const auto& n : net.nodes) {
    minx = std::min(minx, n.pos.x);
    miny = std::min(miny, n.pos.y);
    maxx = std::max(maxx, n.pos.x);
    maxy = std::max(maxy, n.pos.y);
    json cws = json::array();
    for (const auto& c : n.crosswalks) cws.push_back({{"arm", c.arm_node}, {"a", pt(c.a)}, {"b", pt(c.b)}});
    json arms = json::array();
    for (const auto& a : n.arms) arms.push_back({{"neighbor", a.neighbor}, {"in", a.in}, {"out", a.out}, {"compass", a.compass}});
    nodes.push_back({{"id", n.id}, {"pos", pt(n.pos)}, {"boundary", n.boundary}, {"signalized", n.signalized},
                     {"radius", r2(n.radius)}, {"movements", n.movements}, {"crosswalks", cws}, {"arms", arms}});
  }
  j["nodes"] = nodes;
  j["bounds"] = {r2(minx), r2(miny), r2(maxx), r2(maxy)};
  json roads = json::array();
  for (const auto& r : net.roads) {
    json offsets = json::array();
    for (int i = 0; i < r.lane_count(); ++i) offsets.push_back(r2(r.lane_offset(i)));
    roads.push_back({{"id", r.id}, {"from", r.from}, {"to", r.to}, {"reverse", r.reverse}, {"name", r.name},
                     {"lanes", r.lane_count()}, {"lane_width", r.lane_width}, {"lane_offsets", offsets},
                     {"speed_limit_kmh", r2(r.speed_limit * 3.6)}, {"start", pt(r.start)}, {"end", pt(r.end)},
                     {"length", r2(r.length)}});
  }
  j["roads"] = roads;
  json movements = json::array();
  for (const auto& m : net.movements) {
    json paths = json::array();
    for (const auto& c : m.connectors) {
      json p = json::array();
      for (const auto& q : c.path.points()) p.push_back(pt(q));
      paths.push_back(p);
    }
    movements.push_back({{"id", m.id}, {"node", m.node}, {"in", m.in}, {"out", m.out},
                         {"turn", std::string(to_string(m.turn))}, {"label", m.label}, {"paths", paths}});
  }
  j["movements"] = movements;
  json signals = json::array();
  for (const auto& s : engine.signals()) {
    json phases = json::array();
    for (const auto& p : s.phases())
      phases.push_back({{"name", p.name}, {"movements", p.movements}, {"crosswalks", p.crosswalks},
                        {"min_green", r2(p.min_green)}, {"max_green", r2(p.max_green)}, {"green", r2(p.green)},
                        {"yellow", r2(p.yellow)}, {"all_red", r2(p.all_red)}, {"walk_time", r2(p.walk_time)},
                        {"ped_clearance", r2(p.ped_clearance)}});
    signals.push_back({{"node", s.node()}, {"phases", phases}});
  }
  j["signals"] = signals;
  j["controller"] = controller_to_json(engine.controller_config());
  std::vector<NodeId> boundaries = net.boundary_nodes();
  j["boundary_nodes"] = boundaries;
  return j;
}

std::string snapshot_to_json(const SimulationSnapshot& s, std::uint64_t seq) {
  json j;
  j["v"] = kProtocolVersion;
  j["type"] = "snapshot";
  j["seq"] = seq;
  j["step"] = s.step;
  j["t"] = r2(s.time);
  json veh = json::array();
  for (const auto& v : s.vehicles)
    veh.push_back({v.id, r2(v.x), r2(v.y), r3(v.heading), r2(v.speed), static_cast<int>(v.type), r2(v.length)});
  j["veh"] = std::move(veh);
  json ped = json::array();
  for (const auto& p : s.pedestrians) ped.push_back({p.id, r2(p.x), r2(p.y), p.crossing ? 1 : 0});
  j["ped"] = std::move(ped);
  json sig = json::array();
  for (const auto& sv : s.signals) {
    std::string mv, cw;
    for (auto st : sv.movements) mv.push_back(light_char(st));
    for (auto st : sv.crosswalks) cw.push_back(walk_char(st));
    sig.push_back({{"node", sv.node}, {"phase", sv.phase}, {"next", sv.next_phase},
                   {"stage", std::string(to_string(sv.stage))}, {"t", r2(sv.time_in_stage)}, {"mv", mv}, {"cw", cw},
                   {"why", sv.reason}});
  }
  j["sig"] = std::move(sig);
  json q = json::array();
  for (const auto& a : s.queues) q.push_back({a.road, a.queued});
  j["q"] = std::move(q);
  j["closed"] = s.closed_roads;
  json blk = json::array();
  for (const auto& b : s.blockages) blk.push_back({b.road, b.lane, r2(b.position)});
  j["blk"] = std::move(blk);
  j["m"] = metrics_to_json(s.metrics);
  j["demand"] = r3(s.demand_multiplier);
  j["ped_demand"] = r3(s.pedestrian_multiplier);
  j["demand_vph"] = r2(s.demand_vph);
  j["controller"] = s.controller;
  return j.dump();
}

}  // namespace traffic::io
