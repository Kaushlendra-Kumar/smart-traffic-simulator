#include "traffic/infrastructure/scenario_io.hpp"

#include <fstream>
#include <set>
#include <sstream>

namespace traffic::io {

using nlohmann::json;

namespace {

/// Tracks which keys of an object were consumed so unknown keys can be reported.
class Reader {
 public:
  Reader(const json& j, std::string path, std::vector<std::string>& errors)
      : j_(j), path_(std::move(path)), errors_(errors) {
    if (!j_.is_object()) error("expected an object");
  }

  [[nodiscard]] bool ok_object() const { return j_.is_object(); }
  [[nodiscard]] bool has(const char* key) const { return j_.is_object() && j_.contains(key); }
  const json* raw(const char* key) {
    if (!has(key)) return nullptr;
    seen_.insert(key);
    return &j_.at(key);
  }
  [[nodiscard]] std::string at(const std::string& key) const { return path_.empty() ? key : path_ + "." + key; }
  void error(const std::string& msg) const { errors_.push_back((path_.empty() ? std::string("<root>") : path_) + ": " + msg); }
  void error_at(const std::string& key, const std::string& msg) const { errors_.push_back(at(key) + ": " + msg); }

  void num(const char* key, double& out, double lo, double hi) {
    const json* v = raw(key);
    if (!v) return;
    if (!v->is_number()) return error_at(key, "expected a number");
    const double x = v->get<double>();
    if (!(x >= lo && x <= hi)) return error_at(key, "must be in [" + fmt(lo) + ", " + fmt(hi) + "]");
    out = x;
  }
  void integer(const char* key, int& out, int lo, int hi) {
    const json* v = raw(key);
    if (!v) return;
    if (!v->is_number_integer()) return error_at(key, "expected an integer");
    const auto x = v->get<long long>();
    if (x < lo || x > hi) return error_at(key, "must be in [" + std::to_string(lo) + ", " + std::to_string(hi) + "]");
    out = static_cast<int>(x);
  }
  void u64(const char* key, std::uint64_t& out) {
    const json* v = raw(key);
    if (!v) return;
    if (!v->is_number_integer() || v->get<long long>() < 0) return error_at(key, "expected a non-negative integer");
    out = v->get<std::uint64_t>();
  }
  void boolean(const char* key, bool& out) {
    const json* v = raw(key);
    if (!v) return;
    if (!v->is_boolean()) return error_at(key, "expected true/false");
    out = v->get<bool>();
  }
  void str(const char* key, std::string& out, std::size_t max_len = 200) {
    const json* v = raw(key);
    if (!v) return;
    if (!v->is_string()) return error_at(key, "expected a string");
    out = v->get<std::string>();
    if (out.size() > max_len) error_at(key, "too long");
  }
  void finish() const {
    if (!j_.is_object()) return;
    for (const auto& [k, _] : j_.items())
      if (!seen_.contains(k)) error_at(k, "unknown key");
  }
  [[nodiscard]] const std::string& path() const { return path_; }

  static std::string fmt(double x) {
    std::ostringstream os;
    os << x;
    return os.str();
  }

 private:
  const json& j_;
  std::string path_;
  std::vector<std::string>& errors_;
  std::set<std::string> seen_;
};

bool read_number_list(const json& v, std::vector<double>& out, const std::string& path, std::vector<std::string>& errors,
                      double lo, double hi) {
  if (!v.is_array()) {
    errors.push_back(path + ": expected an array of numbers");
    return false;
  }
  out.clear();
  for (const auto& x : v) {
    if (!x.is_number() || !(x.get<double>() >= lo && x.get<double>() <= hi)) {
      errors.push_back(path + ": entries must be numbers in [" + Reader::fmt(lo) + ", " + Reader::fmt(hi) + "]");
      return false;
    }
    out.push_back(x.get<double>());
  }
  return true;
}

void read_grid(const json& j, GridSpec& g, const std::string& path, std::vector<std::string>& errors) {
  Reader r(j, path, errors);
  r.integer("rows", g.rows, 1, 12);
  r.integer("cols", g.cols, 1, 12);
  r.num("block_length", g.block_length, 60, 2000);
  r.num("stub_length", g.stub_length, 60, 2000);
  r.integer("lanes", g.lanes, 1, 5);
  r.num("speed_limit_kmh", g.speed_limit_kmh, 10, 130);
  r.finish();
}

void read_network(const json& j, NetworkSpec& n, const std::string& path, std::vector<std::string>& errors) {
  Reader r(j, path, errors);
  r.num("lane_width", n.lane_width, 2.5, 5.0);
  if (const json* g = r.raw("grid")) {
    GridSpec grid;
    read_grid(*g, grid, r.at("grid"), errors);
    n.grid = grid;
  }
  if (const json* nodes = r.raw("nodes")) {
    if (!nodes->is_array()) r.error_at("nodes", "expected an array");
    else
      for (std::size_t i = 0; i < nodes->size(); ++i) {
        Reader nr((*nodes)[i], r.at("nodes") + "[" + std::to_string(i) + "]", errors);
        NodeSpec ns;
        nr.integer("id", ns.id, 0, 100000);
        nr.num("x", ns.x, -1e6, 1e6);
        nr.num("y", ns.y, -1e6, 1e6);
        if (nr.has("signalized")) {
          bool b = false;
          nr.boolean("signalized", b);
          ns.signalized = b;
        }
        nr.finish();
        n.nodes.push_back(ns);
      }
  }
  if (const json* roads = r.raw("roads")) {
    if (!roads->is_array()) r.error_at("roads", "expected an array");
    else
      for (std::size_t i = 0; i < roads->size(); ++i) {
        Reader rr((*roads)[i], r.at("roads") + "[" + std::to_string(i) + "]", errors);
        RoadSpec rs;
        rr.integer("from", rs.from, 0, 100000);
        rr.integer("to", rs.to, 0, 100000);
        rr.integer("lanes", rs.lanes, 1, 5);
        rr.num("speed_limit_kmh", rs.speed_limit_kmh, 10, 130);
        rr.boolean("two_way", rs.two_way);
        if (rr.has("lanes_reverse")) {
          int lr = rs.lanes;
          rr.integer("lanes_reverse", lr, 1, 5);
          rs.lanes_reverse = lr;
        }
        rr.str("name", rs.name);
        rr.finish();
        n.roads.push_back(rs);
      }
  }
  r.finish();
  if (!n.grid && n.nodes.empty()) errors.push_back(path + ": provide either 'grid' or 'nodes'/'roads'");
  if (n.grid && (!n.nodes.empty() || !n.roads.empty())) errors.push_back(path + ": use either 'grid' or explicit nodes/roads, not both");
}

void read_demand(const json& j, DemandSpec& d, const std::string& path, std::vector<std::string>& errors) {
  Reader r(j, path, errors);
  r.num("default_rate_vph", d.default_rate_vph, 0, 5000);
  if (const json* sources = r.raw("sources")) {
    if (!sources->is_array()) r.error_at("sources", "expected an array");
    else
      for (std::size_t i = 0; i < sources->size(); ++i) {
        const std::string sp = r.at("sources") + "[" + std::to_string(i) + "]";
        Reader sr((*sources)[i], sp, errors);
        SourceSpec s;
        sr.integer("node", s.node, 0, 100000);
        sr.num("rate_vph", s.rate_vph, 0, 5000);
        if (const json* dests = sr.raw("destinations")) {
          if (!dests->is_object()) sr.error_at("destinations", "expected an object {node_id: weight}");
          else
            for (const auto& [k, w] : dests->items()) {
              try {
                const int node = std::stoi(k);
                if (!w.is_number() || w.get<double>() < 0) throw std::invalid_argument("weight");
                s.destinations[node] = w.get<double>();
              } catch (const std::exception&) {
                errors.push_back(sp + ".destinations." + k + ": keys must be node ids and weights non-negative numbers");
              }
            }
        }
        sr.finish();
        d.sources.push_back(s);
      }
  }
  if (const json* profile = r.raw("profile")) {
    if (!profile->is_array()) r.error_at("profile", "expected [[time_s, multiplier], ...]");
    else
      for (const auto& e : *profile) {
        if (!e.is_array() || e.size() != 2 || !e[0].is_number() || !e[1].is_number()) {
          r.error_at("profile", "entries must be [time_s, multiplier]");
          break;
        }
        d.profile.push_back({e[0].get<double>(), e[1].get<double>()});
      }
  }
  if (const json* mix = r.raw("vehicle_mix")) {
    Reader mr(*mix, r.at("vehicle_mix"), errors);
    mr.num("car", d.vehicle_mix[0], 0, 1);
    mr.num("bus", d.vehicle_mix[1], 0, 1);
    mr.num("emergency", d.vehicle_mix[2], 0, 1);
    mr.finish();
  }
  r.finish();
}

void read_signals(const json& j, SignalTimingSpec& s, const std::string& path, std::vector<std::string>& errors) {
  Reader r(j, path, errors);
  r.num("min_green", s.min_green, 1, 120);
  r.num("max_green", s.max_green, 1, 300);
  r.num("yellow", s.yellow, 2, 10);
  r.num("all_red", s.all_red, 0, 10);
  r.num("fixed_green", s.fixed_green, 1, 300);
  r.str("phasing", s.phasing);
  if (const json* list = r.raw("intersections")) {
    if (!list->is_array()) r.error_at("intersections", "expected an array");
    else
      for (std::size_t i = 0; i < list->size(); ++i) {
        const std::string ip = r.at("intersections") + "[" + std::to_string(i) + "]";
        Reader ir((*list)[i], ip, errors);
        IntersectionSignalSpec is;
        ir.integer("node", is.node, 0, 100000);
        if (const json* phases = ir.raw("phases")) {
          if (!phases->is_array()) ir.error_at("phases", "expected an array");
          else
            for (std::size_t k = 0; k < phases->size(); ++k) {
              Reader pr((*phases)[k], ip + ".phases[" + std::to_string(k) + "]", errors);
              PhaseSpec ps;
              pr.str("name", ps.name);
              if (const json* mv = pr.raw("movements")) {
                if (!mv->is_array()) pr.error_at("movements", "expected [[from_node, to_node], ...]");
                else
                  for (const auto& m : *mv) {
                    if (!m.is_array() || m.size() != 2 || !m[0].is_number_integer() || !m[1].is_number_integer()) {
                      pr.error_at("movements", "entries must be [from_node, to_node]");
                      break;
                    }
                    ps.movements.push_back({m[0].get<int>(), m[1].get<int>()});
                  }
              }
              if (const json* cw = pr.raw("crosswalks")) {
                if (!cw->is_array()) pr.error_at("crosswalks", "expected an array of arm node ids");
                else
                  for (const auto& c : *cw) {
                    if (!c.is_number_integer()) {
                      pr.error_at("crosswalks", "expected node ids");
                      break;
                    }
                    ps.crosswalks.push_back(c.get<int>());
                  }
              }
              double v = 0;
              if (pr.has("min_green")) { pr.num("min_green", v, 1, 120); ps.min_green = v; }
              if (pr.has("max_green")) { pr.num("max_green", v, 1, 300); ps.max_green = v; }
              if (pr.has("green")) { pr.num("green", v, 1, 300); ps.green = v; }
              pr.finish();
              is.phases.push_back(std::move(ps));
            }
        }
        ir.finish();
        s.intersections.push_back(std::move(is));
      }
  }
  r.finish();
}

void read_pedestrians(const json& j, PedestrianSpec& p, const std::string& path, std::vector<std::string>& errors) {
  Reader r(j, path, errors);
  r.num("rate_per_hour", p.rate_per_hour, 0, 5000);
  r.num("walk_speed_min", p.walk_speed_min, 0.3, 3);
  r.num("walk_speed_max", p.walk_speed_max, 0.3, 3);
  r.num("walk_time", p.walk_time, 3, 60);
  r.num("clearance_speed", p.clearance_speed, 0.5, 2);
  r.finish();
}

void read_events(const json& j, std::vector<EventSpec>& out, const std::string& path, std::vector<std::string>& errors) {
  if (!j.is_array()) {
    errors.push_back(path + ": expected an array");
    return;
  }
  for (std::size_t i = 0; i < j.size(); ++i) {
    Reader r(j[i], path + "[" + std::to_string(i) + "]", errors);
    EventSpec e;
    r.num("time", e.time, 0, 1e7);
    r.str("action", e.action);
    r.integer("from", e.from, -1, 100000);
    r.integer("to", e.to, -1, 100000);
    r.integer("lane", e.lane, 0, 5);
    r.num("position", e.position, 0, 1e5);
    r.num("value", e.value, 0, 10);
    r.finish();
    out.push_back(e);
  }
}

const char* routing_name(RoutingMode m) {
  switch (m) {
    case RoutingMode::FreeFlowTime: return "free_flow_time";
    case RoutingMode::Distance: return "distance";
    case RoutingMode::CongestionAware: return "congestion_aware";
  }
  return "free_flow_time";
}

}  // namespace

std::optional<ControllerConfig> controller_from_json(const json& j, std::vector<std::string>& errors,
                                                     const std::string& path) {
  const std::size_t before = errors.size();
  ControllerConfig c;
  Reader r(j, path, errors);
  if (!r.ok_object()) return std::nullopt;
  r.str("type", c.type, 40);
  r.str("label", c.label, 60);
  if (const json* g = r.raw("greens")) read_number_list(*g, c.greens, r.at("greens"), errors, 1, 300);
  if (const json* plans = r.raw("plans")) {
    if (!plans->is_array()) r.error_at("plans", "expected an array");
    else
      for (std::size_t i = 0; i < plans->size(); ++i) {
        Reader pr((*plans)[i], r.at("plans") + "[" + std::to_string(i) + "]", errors);
        double start = 0;
        std::vector<double> greens;
        pr.num("start", start, 0, 1e7);
        if (const json* g = pr.raw("greens")) read_number_list(*g, greens, pr.at("greens"), errors, 1, 300);
        pr.finish();
        c.plans.push_back({start, greens});
      }
  }
  if (const json* ng = r.raw("node_greens")) {
    if (!ng->is_object()) r.error_at("node_greens", "expected {node_id: [greens]}");
    else
      for (const auto& [k, v] : ng->items()) {
        std::vector<double> greens;
        try {
          const int node = std::stoi(k);
          if (read_number_list(v, greens, r.at("node_greens") + "." + k, errors, 1, 300)) c.node_greens[node] = greens;
        } catch (const std::exception&) {
          errors.push_back(r.at("node_greens") + "." + k + ": key must be a node id");
        }
      }
  }
  r.num("w_queue", c.w_queue, 0, 100);
  r.num("w_wait", c.w_wait, 0, 100);
  r.num("w_oldest", c.w_oldest, 0, 100);
  r.num("w_arrivals", c.w_arrivals, 0, 100);
  r.num("w_pedestrian", c.w_pedestrian, 0, 100);
  r.num("hysteresis_ratio", c.hysteresis_ratio, 0, 10);
  r.num("hysteresis_abs", c.hysteresis_abs, 0, 100);
  r.num("gap_out_seconds", c.gap_out_seconds, 0.5, 15);
  r.num("max_wait", c.max_wait, 10, 600);
  r.num("max_pedestrian_wait", c.max_pedestrian_wait, 10, 600);
  r.boolean("emergency_priority", c.emergency_priority);
  r.num("decision_interval", c.decision_interval, 1, 60);
  r.finish();
  if (c.type != "fixed_time" && c.type != "adaptive" && c.type != "max_pressure")
    errors.push_back(r.at("type") + ": must be fixed_time, adaptive or max_pressure");
  if (errors.size() != before) return std::nullopt;
  return c;
}

json controller_to_json(const ControllerConfig& c) {
  json j{{"type", c.type},
         {"w_queue", c.w_queue},
         {"w_wait", c.w_wait},
         {"w_oldest", c.w_oldest},
         {"w_arrivals", c.w_arrivals},
         {"w_pedestrian", c.w_pedestrian},
         {"hysteresis_ratio", c.hysteresis_ratio},
         {"hysteresis_abs", c.hysteresis_abs},
         {"gap_out_seconds", c.gap_out_seconds},
         {"max_wait", c.max_wait},
         {"max_pedestrian_wait", c.max_pedestrian_wait},
         {"emergency_priority", c.emergency_priority},
         {"decision_interval", c.decision_interval}};
  if (!c.label.empty()) j["label"] = c.label;
  if (!c.greens.empty()) j["greens"] = c.greens;
  if (!c.plans.empty()) {
    j["plans"] = json::array();
    for (const auto& [start, g] : c.plans) j["plans"].push_back({{"start", start}, {"greens", g}});
  }
  if (!c.node_greens.empty()) {
    j["node_greens"] = json::object();
    for (const auto& [n, g] : c.node_greens) j["node_greens"][std::to_string(n)] = g;
  }
  return j;
}

ParseResult scenario_from_json(const json& j) {
  ParseResult res;
  auto& errors = res.errors;
  ScenarioConfig s;
  Reader r(j, "", errors);
  if (!r.ok_object()) return res;
  r.str("id", s.id, 64);
  r.str("name", s.name);
  r.str("description", s.description, 2000);
  r.num("dt", s.dt, 0.01, 0.5);
  r.u64("seed", s.seed);
  r.num("duration", s.duration, 10, 86400);
  r.num("warmup", s.warmup, 0, 86400);
  std::string routing = routing_name(s.routing);
  r.str("routing", routing);
  if (routing == "free_flow_time") s.routing = RoutingMode::FreeFlowTime;
  else if (routing == "distance") s.routing = RoutingMode::Distance;
  else if (routing == "congestion_aware") s.routing = RoutingMode::CongestionAware;
  else r.error_at("routing", "must be free_flow_time, distance or congestion_aware");
  if (const json* n = r.raw("network")) read_network(*n, s.network, "network", errors);
  else errors.push_back("network: required");
  if (const json* d = r.raw("demand")) read_demand(*d, s.demand, "demand", errors);
  if (const json* p = r.raw("pedestrians")) read_pedestrians(*p, s.pedestrians, "pedestrians", errors);
  if (const json* sg = r.raw("signals")) read_signals(*sg, s.signals, "signals", errors);
  if (const json* c = r.raw("controller")) {
    if (auto cc = controller_from_json(*c, errors)) s.controller = *cc;
  }
  if (const json* e = r.raw("events")) read_events(*e, s.events, "events", errors);
  r.finish();
  for (char ch : s.id)
    if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_')) {
      errors.push_back("id: only letters, digits, '-' and '_' are allowed");
      break;
    }
  if (errors.empty()) res.scenario = std::move(s);
  return res;
}

ParseResult scenario_from_string(const std::string& text) {
  json j = json::parse(text, nullptr, false);
  if (j.is_discarded()) return {std::nullopt, {"invalid JSON"}};
  return scenario_from_json(j);
}

ParseResult scenario_from_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) return {std::nullopt, {"cannot open " + path}};
  std::stringstream ss;
  ss << in.rdbuf();
  return scenario_from_string(ss.str());
}

json scenario_to_json(const ScenarioConfig& s) {
  json j;
  j["id"] = s.id;
  j["name"] = s.name;
  j["description"] = s.description;
  j["dt"] = s.dt;
  j["seed"] = s.seed;
  j["duration"] = s.duration;
  j["warmup"] = s.warmup;
  j["routing"] = routing_name(s.routing);
  json net;
  net["lane_width"] = s.network.lane_width;
  if (s.network.grid) {
    const auto& g = *s.network.grid;
    net["grid"] = {{"rows", g.rows}, {"cols", g.cols}, {"block_length", g.block_length}, {"stub_length", g.stub_length},
                   {"lanes", g.lanes}, {"speed_limit_kmh", g.speed_limit_kmh}};
  } else {
    net["nodes"] = json::array();
    for (const auto& n : s.network.nodes) {
      json nj{{"id", n.id}, {"x", n.x}, {"y", n.y}};
      if (n.signalized) nj["signalized"] = *n.signalized;
      net["nodes"].push_back(nj);
    }
    net["roads"] = json::array();
    for (const auto& r : s.network.roads) {
      json rj{{"from", r.from}, {"to", r.to}, {"lanes", r.lanes}, {"speed_limit_kmh", r.speed_limit_kmh}, {"two_way", r.two_way}};
      if (r.lanes_reverse) rj["lanes_reverse"] = *r.lanes_reverse;
      if (!r.name.empty()) rj["name"] = r.name;
      net["roads"].push_back(rj);
    }
  }
  j["network"] = net;
  json d{{"default_rate_vph", s.demand.default_rate_vph},
         {"vehicle_mix", {{"car", s.demand.vehicle_mix[0]}, {"bus", s.demand.vehicle_mix[1]}, {"emergency", s.demand.vehicle_mix[2]}}}};
  d["sources"] = json::array();
  for (const auto& src : s.demand.sources) {
    json sj{{"node", src.node}, {"rate_vph", src.rate_vph}};
    if (!src.destinations.empty()) {
      sj["destinations"] = json::object();
      for (const auto& [n, w] : src.destinations) sj["destinations"][std::to_string(n)] = w;
    }
    d["sources"].push_back(sj);
  }
  d["profile"] = json::array();
  for (const auto& [t, m] : s.demand.profile) d["profile"].push_back({t, m});
  j["demand"] = d;
  j["pedestrians"] = {{"rate_per_hour", s.pedestrians.rate_per_hour}, {"walk_speed_min", s.pedestrians.walk_speed_min},
                      {"walk_speed_max", s.pedestrians.walk_speed_max}, {"walk_time", s.pedestrians.walk_time},
                      {"clearance_speed", s.pedestrians.clearance_speed}};
  json sig{{"min_green", s.signals.min_green}, {"max_green", s.signals.max_green}, {"yellow", s.signals.yellow},
           {"all_red", s.signals.all_red}, {"fixed_green", s.signals.fixed_green}, {"phasing", s.signals.phasing}};
  if (!s.signals.intersections.empty()) {
    sig["intersections"] = json::array();
    for (const auto& is : s.signals.intersections) {
      json ij{{"node", is.node}, {"phases", json::array()}};
      for (const auto& p : is.phases) {
        json pj{{"name", p.name}, {"movements", json::array()}, {"crosswalks", p.crosswalks}};
        for (auto [a, b] : p.movements) pj["movements"].push_back({a, b});
        if (p.min_green) pj["min_green"] = *p.min_green;
        if (p.max_green) pj["max_green"] = *p.max_green;
        if (p.green) pj["green"] = *p.green;
        ij["phases"].push_back(pj);
      }
      sig["intersections"].push_back(ij);
    }
  }
  j["signals"] = sig;
  j["controller"] = controller_to_json(s.controller);
  j["events"] = json::array();
  for (const auto& e : s.events)
    j["events"].push_back({{"time", e.time}, {"action", e.action}, {"from", e.from}, {"to", e.to}, {"lane", e.lane},
                           {"position", e.position}, {"value", e.value}});
  return j;
}

}  // namespace traffic::io
