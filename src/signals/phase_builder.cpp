#include <algorithm>
#include <numeric>

#include "traffic/signals/traffic_signal.hpp"

namespace traffic {

bool SignalPhase::contains(MovementId m) const {
  return std::find(movements.begin(), movements.end(), m) != movements.end();
}
bool SignalPhase::contains_crosswalk(int c) const {
  return std::find(crosswalks.begin(), crosswalks.end(), c) != crosswalks.end();
}

ConflictModel::ConflictModel(const RoadNetwork& net, NodeId node) : node_(node) {
  const Node& n = net.node(node);
  movements_ = n.movements;
  first_ = movements_.empty() ? 0 : movements_.front();
  const std::size_t k = movements_.size();
  conflict_.assign(k, std::vector<bool>(k, false));
  for (std::size_t i = 0; i < k; ++i) {
    for (std::size_t j = i + 1; j < k; ++j) {
      const Movement& a = net.movement(movements_[i]);
      const Movement& b = net.movement(movements_[j]);
      bool c = false;
      if (a.in != b.in) {
        if (a.out == b.out) c = true;  // merge conflict
        for (const auto& ca : a.connectors) {
          for (const auto& cb : b.connectors)
            if (ca.path.intersects(cb.path)) { c = true; break; }
          if (c) break;
        }
      }
      conflict_[i][j] = conflict_[j][i] = c;
    }
  }
  ped_.assign(n.crosswalks.size(), std::vector<bool>(k, false));
  for (const auto& cw : n.crosswalks) {
    for (std::size_t i = 0; i < k; ++i) {
      const Movement& m = net.movement(movements_[i]);
      ped_[static_cast<std::size_t>(cw.index)][i] = (m.in == cw.road_in) || (m.out == cw.road_out);
    }
  }
}

int ConflictModel::local(MovementId m) const {
  const MovementId idx = m - first_;
  if (idx >= 0 && static_cast<std::size_t>(idx) < movements_.size() && movements_[static_cast<std::size_t>(idx)] == m)
    return idx;
  auto it = std::find(movements_.begin(), movements_.end(), m);
  return it == movements_.end() ? -1 : static_cast<int>(it - movements_.begin());
}

bool ConflictModel::conflicts(MovementId a, MovementId b) const {
  const int i = local(a), j = local(b);
  if (i < 0 || j < 0) return false;
  return conflict_[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
}

bool ConflictModel::crosswalk_conflicts(int crosswalk, MovementId m) const {
  const int i = local(m);
  if (i < 0 || crosswalk < 0 || static_cast<std::size_t>(crosswalk) >= ped_.size()) return false;
  return ped_[static_cast<std::size_t>(crosswalk)][static_cast<std::size_t>(i)];
}

namespace {

bool compatible_with_phase(const ConflictModel& cm, const SignalPhase& p, MovementId m) {
  for (MovementId o : p.movements)
    if (o == m || cm.conflicts(o, m)) return false;
  for (int c : p.crosswalks)
    if (cm.crosswalk_conflicts(c, m)) return false;
  return true;
}

void apply_timing(SignalPhase& p, const SignalTimingSpec& t, const PedestrianSpec& peds,
                  const Node& node) {
  p.min_green = t.min_green;
  p.max_green = t.max_green;
  p.green = t.fixed_green;
  p.yellow = p.serves_vehicles() ? t.yellow : 0.0;
  p.all_red = t.all_red;
  if (!p.crosswalks.empty()) {
    double longest = 0.0;
    for (int c : p.crosswalks) longest = std::max(longest, node.crosswalks[static_cast<std::size_t>(c)].length);
    p.walk_time = peds.walk_time;
    p.ped_clearance = longest / std::max(peds.clearance_speed, 0.3);
    const double ped_min = p.walk_time + p.ped_clearance;
    p.min_green = std::max(p.min_green, ped_min);
    p.green = std::max(p.green, ped_min);
    p.max_green = std::max(p.max_green, p.min_green);
    if (!p.serves_vehicles()) {  // pedestrian-only phase: fixed length
      p.green = p.min_green;
      p.max_green = p.min_green;
    }
  }
}

std::string phase_name(const RoadNetwork& net, const SignalPhase& p, int index) {
  std::string name = "P" + std::to_string(index + 1);
  if (!p.serves_vehicles()) return name + " pedestrian scramble";
  // Summarise by approach: e.g. "N:SLR S:SR"
  std::vector<std::pair<std::string, std::string>> parts;
  for (MovementId m : p.movements) {
    const Movement& mv = net.movement(m);
    const std::string approach = mv.label.substr(0, mv.label.find("→"));
    auto it = std::find_if(parts.begin(), parts.end(), [&](auto& x) { return x.first == approach; });
    if (it == parts.end()) parts.push_back({approach, std::string(to_string(mv.turn))});
    else it->second += std::string(to_string(mv.turn));
  }
  name += " ";
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) name += " ";
    name += parts[i].first + ":" + parts[i].second;
  }
  return name;
}

}  // namespace

PhasePlan build_phase_plan(const RoadNetwork& net, NodeId node_id, const ConflictModel& cm,
                           const SignalTimingSpec& timing, const PedestrianSpec& peds,
                           const IntersectionSignalSpec* explicit_spec) {
  PhasePlan plan;
  const Node& node = net.node(node_id);

  if (explicit_spec && !explicit_spec->phases.empty()) {
    for (const PhaseSpec& ps : explicit_spec->phases) {
      SignalPhase p;
      for (auto [from, to] : ps.movements) {
        const RoadId in = net.find_road(from, node_id);
        const RoadId out = net.find_road(node_id, to);
        const MovementId m = (in == kInvalidRoad || out == kInvalidRoad) ? kInvalidMovement
                                                                          : net.find_movement(in, out);
        if (m == kInvalidMovement) {
          plan.errors.push_back("node " + std::to_string(node_id) + ": no movement " +
                                std::to_string(from) + "->" + std::to_string(to));
          continue;
        }
        p.movements.push_back(m);
      }
      for (NodeId arm : ps.crosswalks) {
        auto it = std::find_if(node.crosswalks.begin(), node.crosswalks.end(),
                               [&](const Crosswalk& c) { return c.arm_node == arm; });
        if (it == node.crosswalks.end()) {
          plan.errors.push_back("node " + std::to_string(node_id) + ": no crosswalk on arm " + std::to_string(arm));
          continue;
        }
        p.crosswalks.push_back(it->index);
      }
      apply_timing(p, timing, peds, node);
      if (ps.min_green) p.min_green = std::max(*ps.min_green, p.walk_time + p.ped_clearance);
      if (ps.max_green) p.max_green = *ps.max_green;
      if (ps.green) p.green = *ps.green;
      p.name = ps.name.empty() ? phase_name(net, p, static_cast<int>(plan.phases.size())) : ps.name;
      plan.phases.push_back(std::move(p));
    }
  } else {
    // 1) Greedy colouring of the conflict graph (through + left movements). Order: through movements first, then
    //    left turns, then right turns (ties: higher conflict degree, then id). Grouping
    //    opposing through movements first yields NEMA-like phase sets.
    std::vector<MovementId> order = cm.movements();
    auto degree = [&](MovementId m) {
      int d = 0;
      for (MovementId o : cm.movements()) d += cm.conflicts(m, o) ? 1 : 0;
      return d;
    };
    auto rank = [&](MovementId m) {
      switch (net.movement(m).turn) {
        case TurnType::Straight: return 0;
        case TurnType::Left: return 1;
        case TurnType::Right: return 2;
        case TurnType::UTurn: return 3;
      }
      return 3;
    };
    std::stable_sort(order.begin(), order.end(), [&](MovementId a, MovementId b) {
      if (rank(a) != rank(b)) return rank(a) < rank(b);
      const int da = degree(a), db = degree(b);
      return da != db ? da > db : a < b;
    });
    std::string strategy = timing.phasing;
    if (strategy != "split" && strategy != "protected") {  // "auto"
      bool wide = true;
      for (const Arm& arm : node.arms)
        if (arm.in != kInvalidRoad && net.road(arm.in).lane_count() < 3) wide = false;
      strategy = wide ? "protected" : "split";
    }
    std::vector<SignalPhase> phases;
    auto colour = [&](MovementId m) {
      bool placed = false;
      for (auto& p : phases)
        if (compatible_with_phase(cm, p, m)) { p.movements.push_back(m); placed = true; break; }
      if (!placed) {
        SignalPhase p;
        p.movements = {m};
        phases.push_back(std::move(p));
      }
    };
    if (strategy == "split") {
      // Split phasing: each approach gets its own phase serving all of its movements.
      // Shared lanes can never be blocked by a vehicle waiting for another phase.
      for (const Arm& arm : node.arms) {
        if (arm.in == kInvalidRoad) continue;
        SignalPhase p;
        for (MovementId m : cm.movements())
          if (net.movement(m).in == arm.in) p.movements.push_back(m);
        if (!p.movements.empty()) phases.push_back(std::move(p));
      }
    } else {
      // Right turns are left out of the colouring: they fit almost anywhere and would
      // otherwise block parallel pedestrian crossings. They join in step 3.
      for (MovementId m : order)
        if (rank(m) < 2) colour(m);
    }
    // 2) Crosswalks join every compatible phase (before augmentation, so parallel
    //    pedestrian movements are not crowded out by turning traffic); leftovers get an
    //    exclusive pedestrian phase.
    std::vector<int> unserved;
    for (const auto& cw : node.crosswalks) {
      bool served = false;
      for (auto& p : phases) {
        bool ok = true;
        for (MovementId m : p.movements)
          if (cm.crosswalk_conflicts(cw.index, m)) { ok = false; break; }
        if (ok) { p.crosswalks.push_back(cw.index); served = true; }
      }
      if (!served) unserved.push_back(cw.index);
    }
    if (!unserved.empty()) {
      SignalPhase p;
      p.crosswalks = unserved;
      phases.push_back(std::move(p));
    }
    // 3) Maximal augmentation: add every movement compatible with the phase's movements
    //    and crosswalks.
    for (auto& p : phases)
      for (MovementId m : cm.movements())
        if (!p.contains(m) && compatible_with_phase(cm, p, m)) p.movements.push_back(m);
    // 4) Anything still unserved (rare) gets coloured into additional phases.
    for (MovementId m : order)
      if (std::none_of(phases.begin(), phases.end(), [&](const SignalPhase& p) { return p.contains(m); })) colour(m);
    for (auto& p : phases) std::sort(p.movements.begin(), p.movements.end());
    for (std::size_t i = 0; i < phases.size(); ++i) {
      apply_timing(phases[i], timing, peds, node);
      phases[i].name = phase_name(net, phases[i], static_cast<int>(i));
    }
    plan.phases = std::move(phases);
  }

  auto problems = validate_phase_plan(net, node_id, cm, plan.phases);
  plan.errors.insert(plan.errors.end(), problems.begin(), problems.end());
  return plan;
}

std::vector<std::string> validate_phase_plan(const RoadNetwork& net, NodeId node_id,
                                             const ConflictModel& cm,
                                             const std::vector<SignalPhase>& phases) {
  std::vector<std::string> errors;
  const std::string where = "node " + std::to_string(node_id) + ": ";
  if (phases.empty()) errors.push_back(where + "no phases");
  for (std::size_t pi = 0; pi < phases.size(); ++pi) {
    const auto& p = phases[pi];
    for (std::size_t i = 0; i < p.movements.size(); ++i)
      for (std::size_t j = i + 1; j < p.movements.size(); ++j)
        if (cm.conflicts(p.movements[i], p.movements[j]))
          errors.push_back(where + "phase " + std::to_string(pi + 1) + " gives green to conflicting movements " +
                           net.movement(p.movements[i]).label + " and " + net.movement(p.movements[j]).label);
    for (int c : p.crosswalks)
      for (MovementId m : p.movements)
        if (cm.crosswalk_conflicts(c, m))
          errors.push_back(where + "phase " + std::to_string(pi + 1) + " gives WALK to a crosswalk crossed by " +
                           net.movement(m).label);
    if (!(p.min_green > 0) || p.min_green > p.max_green)
      errors.push_back(where + "phase " + std::to_string(pi + 1) + " needs 0 < min_green <= max_green");
    if (p.serves_vehicles() && p.yellow < 2.0)
      errors.push_back(where + "phase " + std::to_string(pi + 1) + " yellow must be >= 2 s");
    if (p.all_red < 0.0) errors.push_back(where + "phase " + std::to_string(pi + 1) + " all_red must be >= 0");
  }
  for (MovementId m : cm.movements()) {
    bool served = std::any_of(phases.begin(), phases.end(), [&](const SignalPhase& p) { return p.contains(m); });
    if (!served) errors.push_back(where + "movement " + net.movement(m).label + " is never served");
  }
  return errors;
}

}  // namespace traffic
