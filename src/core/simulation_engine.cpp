#include "traffic/core/simulation_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>

#include "traffic/network/network_builder.hpp"
#include "traffic/network/routing.hpp"

namespace traffic {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kQueueSpeed = 2.0;     // m/s: below this a vehicle counts as queued
constexpr double kStopSpeed = 0.5;      // m/s: below this a vehicle counts as stopped
constexpr double kApproachRange = 150;  // m: detector range for "approaching" vehicles
constexpr double kLaneChangeCooldown = 3.0;

template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
// Explicit deduction guide: older compilers (e.g. Clang 15) lack aggregate CTAD.
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

void hash_mix(std::uint64_t& h, const void* data, std::size_t n) {
  const auto* p = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
}
template <typename T>
void hash_value(std::uint64_t& h, const T& v) {
  hash_mix(h, &v, sizeof(T));
}

}  // namespace

// ============================================================ construction

SimulationEngine::SimulationEngine(ScenarioConfig config, RoadNetwork net)
    : config_(std::move(config)),
      net_(std::move(net)),
      clock_(config_.dt),
      controller_cfg_(config_.controller),
      spawner_(config_.demand, net_, config_.seed),
      peds_(config_.pedestrians, net_, config_.seed),
      metrics_(config_.warmup) {
  backlog_.resize(net_.nodes.size());
  signal_of_node_.assign(net_.nodes.size(), -1);
  events_ = config_.events;
  std::stable_sort(events_.begin(), events_.end(), [](const EventSpec& a, const EventSpec& b) { return a.time < b.time; });
}

EngineCreateResult SimulationEngine::create(const ScenarioConfig& config) {
  EngineCreateResult out;
  auto& errors = out.errors;
  if (!(config.dt > 0.0 && config.dt <= 0.5)) errors.push_back("dt must be in (0, 0.5] s");
  if (!(config.duration > 0.0)) errors.push_back("duration must be positive");
  if (!(config.warmup >= 0.0)) errors.push_back("warmup must be >= 0");
  if (!(config.demand.default_rate_vph >= 0.0)) errors.push_back("default_rate_vph must be >= 0");
  for (const auto& s : config.demand.sources)
    if (!(s.rate_vph >= 0.0 && s.rate_vph <= 5000.0)) errors.push_back("source rate_vph must be in [0, 5000]");
  for (const auto& [t, m] : config.demand.profile)
    if (!(m >= 0.0 && m <= 10.0) || t < 0) errors.push_back("demand profile entries need time >= 0 and multiplier in [0, 10]");
  if (!(config.pedestrians.rate_per_hour >= 0.0)) errors.push_back("pedestrian rate must be >= 0");
  if (!(config.pedestrians.walk_speed_min > 0.2 && config.pedestrians.walk_speed_max >= config.pedestrians.walk_speed_min))
    errors.push_back("pedestrian walk speeds must satisfy 0.2 < min <= max");
  if (config.signals.phasing != "auto" && config.signals.phasing != "split" && config.signals.phasing != "protected")
    errors.push_back("signals.phasing must be auto, split or protected");
  if (!(config.signals.yellow >= 2.0 && config.signals.all_red >= 0.0 && config.signals.min_green > 0.0 &&
        config.signals.min_green <= config.signals.max_green))
    errors.push_back("signal timing needs yellow >= 2, all_red >= 0 and 0 < min_green <= max_green");
  auto cerr = validate_controller_config(config.controller);
  errors.insert(errors.end(), cerr.begin(), cerr.end());
  if (!errors.empty()) return out;

  BuildResult built = build_network(config.network);
  if (!built.ok()) {
    errors = built.errors;
    return out;
  }
  std::unique_ptr<SimulationEngine> eng(new SimulationEngine(config, std::move(built.network)));
  for (const auto& node : eng->net_.nodes) {
    if (!node.signalized) continue;
    const IntersectionSignalSpec* spec = nullptr;
    for (const auto& s : config.signals.intersections)
      if (s.node == node.id) spec = &s;
    ConflictModel cm(eng->net_, node.id);
    PhasePlan plan = build_phase_plan(eng->net_, node.id, cm, config.signals, config.pedestrians, spec);
    if (!plan.errors.empty()) {
      errors.insert(errors.end(), plan.errors.begin(), plan.errors.end());
      continue;
    }
    eng->signal_of_node_[static_cast<std::size_t>(node.id)] = static_cast<int>(eng->signals_.size());
    eng->signals_.emplace_back(eng->net_, node.id, std::move(plan.phases));
  }
  for (const auto& s : config.signals.intersections)
    if (s.node < 0 || static_cast<std::size_t>(s.node) >= eng->net_.nodes.size() || !eng->net_.node(s.node).signalized)
      errors.push_back("signal spec for node " + std::to_string(s.node) + " which is not a signalised node");
  for (const auto& e : config.events) {
    static const std::set<std::string> known{"close_road", "open_road", "block_lane", "clear_lane", "set_demand",
                                             "set_pedestrian_demand"};
    if (!known.contains(e.action)) {
      errors.push_back("unknown event action '" + e.action + "'");
      continue;
    }
    if (e.action.find("road") != std::string::npos || e.action.find("lane") != std::string::npos) {
      const RoadId r = eng->net_.find_road(e.from, e.to);
      if (r == kInvalidRoad) errors.push_back("event references missing road " + std::to_string(e.from) + "->" + std::to_string(e.to));
      else if (e.action.find("lane") != std::string::npos &&
               (e.lane < 0 || e.lane >= eng->net_.road(r).lane_count() || e.position < 0 || e.position > eng->net_.road(r).length))
        errors.push_back("event lane/position out of range on road " + std::to_string(e.from) + "->" + std::to_string(e.to));
    }
  }
  if (!errors.empty()) return out;

  eng->movements_into_road_.assign(eng->net_.roads.size(), 0);
  for (const auto& m : eng->net_.movements) ++eng->movements_into_road_[static_cast<std::size_t>(m.out)];
  eng->road_vehicles_.assign(eng->net_.roads.size(), 0);
  for (const auto& n : eng->net_.nodes) eng->ped_oldest_.emplace_back(n.crosswalks.size(), 0.0);
  eng->build_controllers(config.controller);
  eng->istats_.resize(eng->signals_.size());
  eng->queue_integral_.assign(eng->signals_.size(), 0.0);
  for (std::size_t k = 0; k < eng->signals_.size(); ++k) {
    auto& st = eng->istats_[k];
    st.node = eng->signals_[k].node();
    for (const auto& p : eng->signals_[k].phases()) st.phases.push_back(PhaseStats{p.name, 0.0, 0, 0.0});
    eng->movement_obs_.emplace_back(eng->signals_[k].conflicts().movements().size());
  }
  out.engine = std::move(eng);
  return out;
}

void SimulationEngine::build_controllers(const ControllerConfig& cfg) {
  controllers_.clear();
  last_reasons_.assign(signals_.size(), "");
  for (const auto& sig : signals_) {
    auto c = make_controller(cfg);
    c->initialize(IntersectionConfig{sig.node(), sig.phases()});
    controllers_.push_back(std::move(c));
  }
  controller_cfg_ = cfg;
}

const TrafficSignal* SimulationEngine::signal_at(NodeId node) const {
  if (node < 0 || static_cast<std::size_t>(node) >= signal_of_node_.size()) return nullptr;
  const int k = signal_of_node_[static_cast<std::size_t>(node)];
  return k < 0 ? nullptr : &signals_[static_cast<std::size_t>(k)];
}

int SimulationEngine::backlog_count() const {
  int n = 0;
  for (const auto& q : backlog_) n += static_cast<int>(q.size());
  return n;
}

// ============================================================ main loop

void SimulationEngine::step() {
  apply_due_events();
  generate_demand();
  admit_from_backlog();
  peds_.generate(time(), dt(), config_.warmup);
  update_signals();
  ped_done_.clear();
  peds_.update(time(), dt(), [this](NodeId n) { return signal_at(n); }, ped_done_);
  for (const auto& p : ped_done_) metrics_.on_pedestrian_completed(p.wait, p.measured);
  compute_accelerations();
  integrate_and_transition();
  change_lanes();
  update_bookkeeping();
  clock_.advance();
}

void SimulationEngine::run_for(double seconds) { run_until(time() + seconds); }

void SimulationEngine::run_until(double t) {
  while (time() + 1e-9 < t) step();
}

// ============================================================ events & demand

void SimulationEngine::apply_due_events() {
  while (next_event_ < events_.size() && events_[next_event_].time <= time() + 1e-9) {
    const EventSpec& e = events_[next_event_++];
    const RoadId r = net_.find_road(e.from, e.to);
    if (e.action == "close_road") apply_impl(CmdCloseRoad{r});
    else if (e.action == "open_road") apply_impl(CmdOpenRoad{r});
    else if (e.action == "block_lane") apply_impl(CmdBlockLane{r, e.lane, e.position});
    else if (e.action == "clear_lane") apply_impl(CmdClearLane{r, e.lane});
    else if (e.action == "set_demand") apply_impl(CmdSetDemand{e.value});
    else if (e.action == "set_pedestrian_demand") apply_impl(CmdSetPedestrianDemand{e.value});
  }
}

double SimulationEngine::route_cost(const Road& r) const {
  switch (config_.routing) {
    case RoutingMode::Distance: return distance_cost(r);
    case RoutingMode::FreeFlowTime: return free_flow_cost(r);
    case RoutingMode::CongestionAware: {
      double sum = 0.0;
      int n = 0;
      for (const auto& lane : r.lanes)
        for (SlotIndex s : lane.vehicles) {
          sum += vehicles_[s].v;
          ++n;
        }
      const double speed = n == 0 ? r.speed_limit : std::clamp(sum / n, 2.0, r.speed_limit);
      return r.length / speed;
    }
  }
  return free_flow_cost(r);
}

void SimulationEngine::generate_demand() {
  trip_buffer_.clear();
  spawner_.generate(time(), dt(), trip_buffer_);
  const auto cost = [this](const Road& r) { return route_cost(r); };
  for (const auto& trip : trip_buffer_) {
    metrics_.on_trip_requested();
    auto route = route_between(net_, trip.origin, trip.destination, cost);
    if (!route) {
      metrics_.on_unserved();
      continue;
    }
    backlog_[static_cast<std::size_t>(trip.origin)].push_back({trip, std::move(*route)});
  }
}

void SimulationEngine::admit_from_backlog() {
  const auto cost = [this](const Road& r) { return route_cost(r); };
  for (auto& queue : backlog_) {
    if (queue.empty()) continue;
    PendingTrip& pending = queue.front();
    const TripRequest& trip = pending.trip;
    // Replan only if a closure invalidated the cached route.
    if (std::any_of(pending.route.begin(), pending.route.end(), [this](RoadId r) { return net_.road(r).closed; })) {
      auto fresh = route_between(net_, trip.origin, trip.destination, cost);
      if (!fresh) {
        metrics_.on_unserved();
        queue.pop_front();
        continue;
      }
      pending.route = std::move(*fresh);
    }
    std::vector<RoadId>* route = &pending.route;
    const Road& road = net_.road(route->front());
    TurnType turn = TurnType::Straight;
    if (route->size() > 1) turn = net_.movement(net_.find_movement(route->at(0), route->at(1))).turn;
    int best_lane = -1;
    double best_free = -kInf;
    double tail_speed = 0.0;
    for (int li = 0; li < road.lane_count(); ++li) {
      if (!net_.lane_permits(road, li, turn)) continue;
      const Lane& lane = road.lanes[static_cast<std::size_t>(li)];
      double free = road.length;
      double tv = -1.0;
      if (!lane.vehicles.empty()) {
        const Vehicle& tail = vehicles_[lane.vehicles.back()];
        free = tail.s - tail.params.length;
        tv = tail.v;
      }
      if (lane.blockage) free = std::min(free, *lane.blockage);
      if (free > best_free) {
        best_free = free;
        best_lane = li;
        tail_speed = tv;
      }
    }
    const IdmParams& p = trip.params;
    if (best_lane < 0 || best_free < 2.0 * p.length + p.min_gap + 1.0) continue;  // entry blocked: wait

    const SlotIndex slot = allocate_slot();
    Vehicle& v = vehicles_[slot];
    v = Vehicle{};
    v.active = true;
    v.id = trip.id;
    v.type = trip.type;
    v.params = p;
    v.origin = trip.origin;
    v.destination = trip.destination;
    v.route = std::move(*route);
    v.route_index = 0;
    v.road = road.id;
    v.lane = best_lane;
    v.s = p.length;
    const double v0 = free_speed_on_road(v, road);
    const double room = best_free - 2.0 * p.length - p.min_gap;
    v.v = tail_speed < 0 ? 0.8 * v0 : std::clamp(std::min(tail_speed, room / p.time_headway), 0.0, 0.8 * v0);
    v.demand_time = trip.demand_time;
    v.enter_time = time();
    v.measured = trip.demand_time >= config_.warmup;
    set_next_movement(v);
    net_.road(road.id).lanes[static_cast<std::size_t>(best_lane)].vehicles.push_back(slot);
    metrics_.on_vehicle_entered();
    queue.pop_front();
  }
}

SlotIndex SimulationEngine::allocate_slot() {
  ++active_;
  if (!free_slots_.empty()) {
    const SlotIndex s = free_slots_.back();
    free_slots_.pop_back();
    return s;
  }
  vehicles_.emplace_back();
  may_enter_.push_back(false);
  return static_cast<SlotIndex>(vehicles_.size() - 1);
}

void SimulationEngine::release(SlotIndex slot) {
  vehicles_[slot].active = false;
  vehicles_[slot].route.clear();
  free_slots_.push_back(slot);
  --active_;
}

void SimulationEngine::set_next_movement(Vehicle& v) {
  const RoadId next = v.next_road();
  v.next_movement = next == kInvalidRoad ? kInvalidMovement : net_.find_movement(v.road, next);
  if (!v.measured || time() < config_.warmup || v.next_movement == kInvalidMovement) return;
  const int k = signal_of_node_[static_cast<std::size_t>(net_.road(v.road).to)];
  if (k < 0) return;
  const auto& phases = signals_[static_cast<std::size_t>(k)].phases();
  int containing = 0;
  for (const auto& ph : phases) containing += ph.contains(v.next_movement) ? 1 : 0;
  for (std::size_t i = 0; i < phases.size(); ++i)
    if (phases[i].contains(v.next_movement)) istats_[static_cast<std::size_t>(k)].phases[i].arrivals += 1.0 / containing;
}

// ============================================================ signals

void SimulationEngine::update_signals() {
  const double t = time();
  const double gap_out = controller_cfg_.gap_out_seconds;
  // Per-step aggregates shared by every intersection (one pass each).
  for (const auto& r : net_.roads) {
    int n = 0;
    for (const auto& lane : r.lanes) n += static_cast<int>(lane.vehicles.size());
    road_vehicles_[static_cast<std::size_t>(r.id)] = n;
  }
  for (auto& per_node : ped_oldest_) std::fill(per_node.begin(), per_node.end(), 0.0);
  for (const auto& p : peds_.pedestrians())
    if (!p.crossing()) {
      double& o = ped_oldest_[static_cast<std::size_t>(p.node)][static_cast<std::size_t>(p.crosswalk)];
      o = std::max(o, t - p.arrival_time);
    }
  for (std::size_t k = 0; k < signals_.size(); ++k) {
    TrafficSignal& sig = signals_[k];
    const Node& node = net_.node(sig.node());
    auto& mobs = movement_obs_[k];
    std::fill(mobs.begin(), mobs.end(), MovementObs{});
    for (RoadId rid : node.in) {
      const Road& road = net_.road(rid);
      for (const auto& lane : road.lanes) {
        // Lane-based detection: a stopped queue can only discharge when the movement of
        // its front vehicle is served, so queued vehicles are attributed to that movement.
        const MovementId front_m = lane.vehicles.empty() ? kInvalidMovement : vehicles_[lane.vehicles.front()].next_movement;
        for (SlotIndex slot : lane.vehicles) {
          const Vehicle& v = vehicles_[slot];
          if (v.next_movement == kInvalidMovement) continue;
          const double d = road.length - v.s;
          const bool queued = v.v < kQueueSpeed;
          const int local = sig.conflicts().local(queued && front_m != kInvalidMovement ? front_m : v.next_movement);
          if (local < 0) continue;
          MovementObs& mo = mobs[static_cast<std::size_t>(local)];
          if (queued) {
            ++mo.queued;
            const double w = v.queued_since >= 0 ? t - v.queued_since : 0.0;
            mo.wait_sum += w;
            mo.oldest = std::max(mo.oldest, w);
          } else if (d < kApproachRange) {
            ++mo.approaching;
            if (d / std::max(v.v, 0.1) <= gap_out) ++mo.arriving_soon;
          }
          if (v.type == VehicleType::Emergency && d < 200.0) mo.emergency = true;
        }
      }
    }

    IntersectionObservation obs;
    obs.node = sig.node();
    obs.time = t;
    obs.current_phase = sig.current_phase();
    obs.stage = sig.stage();
    obs.time_in_stage = sig.time_in_stage();
    obs.phases = &sig.phases();
    obs.phase_obs.resize(sig.phases().size());
    for (std::size_t p = 0; p < sig.phases().size(); ++p) {
      const SignalPhase& phase = sig.phases()[p];
      PhaseObservation& po = obs.phase_obs[p];
      for (MovementId m : phase.movements) {
        const MovementObs& mo = mobs[static_cast<std::size_t>(sig.conflicts().local(m))];
        po.queued += mo.queued;
        po.approaching += mo.approaching;
        po.arriving_soon += mo.arriving_soon;
        po.wait_sum += mo.wait_sum;
        po.oldest_wait = std::max(po.oldest_wait, mo.oldest);
        po.emergency = po.emergency || mo.emergency;
        // Max-pressure: upstream demand minus downstream occupancy (shared among the
        // movements that discharge into the same road).
        const auto out = static_cast<std::size_t>(net_.movement(m).out);
        po.pressure += (mo.queued + mo.approaching) -
                       static_cast<double>(road_vehicles_[out]) / std::max(movements_into_road_[out], 1);
      }
      for (int c : phase.crosswalks) {
        po.pedestrians_waiting += peds_.waiting(node.id, c);
        po.oldest_pedestrian_wait = std::max(
            po.oldest_pedestrian_wait, ped_oldest_[static_cast<std::size_t>(node.id)][static_cast<std::size_t>(c)]);
      }
    }
    // Pedestrian recall within the current green.
    if (sig.stage() == SignalStage::Green)
      for (int c : sig.phases()[static_cast<std::size_t>(sig.current_phase())].crosswalks)
        if (peds_.waiting(node.id, c) > 0) sig.request_walk(c);

    bool other_demand = false;
    for (int p = 0; p < sig.phase_count(); ++p)
      if (p != sig.current_phase() && obs.has_demand(p)) other_demand = true;

    const SignalDecision decision = controllers_[k]->decide(obs, t);
    if (t >= config_.warmup && sig.stage() == SignalStage::Green)
      istats_[k].phases[static_cast<std::size_t>(sig.current_phase())].green_time += dt();
    sig.step(dt(), decision, other_demand);
    last_reasons_[k] = controllers_[k]->last_reason();
  }
}

// ============================================================ vehicle dynamics

double SimulationEngine::free_speed_on_road(const Vehicle& v, const Road& r) const {
  return r.speed_limit * v.params.desired_speed_factor;
}

double SimulationEngine::free_speed_on_connector(const Vehicle& v, const Movement& m) const {
  return std::min(m.turn_speed, net_.road(m.out).speed_limit) * v.params.desired_speed_factor;
}

bool SimulationEngine::crosswalks_clear(const Movement& m) const {
  const Node& node = net_.node(m.node);
  for (const auto& cw : node.crosswalks)
    if ((cw.road_in == m.in || cw.road_out == m.out) && peds_.crosswalk_occupied(node.id, cw.index)) return false;
  return true;
}

bool SimulationEngine::downstream_has_space(const Vehicle& v, const Movement& m, int in_lane) const {
  const Connector& c = m.connectors[static_cast<std::size_t>(in_lane)];
  const Road& out = net_.road(m.out);
  const Lane& lane = out.lanes[static_cast<std::size_t>(c.out_lane)];
  double free = out.length;
  if (!lane.vehicles.empty()) {
    const Vehicle& tail = vehicles_[lane.vehicles.back()];
    free = tail.s - tail.params.length;
  }
  if (lane.blockage) free = std::min(free, *lane.blockage);
  for (const auto& [fm, fc] : lane.feeders)
    for (SlotIndex s : net_.movement(fm).connectors[static_cast<std::size_t>(fc)].vehicles)
      free -= vehicles_[s].params.length + vehicles_[s].params.min_gap;
  return free >= v.params.length + v.params.min_gap;
}

bool SimulationEngine::may_enter_junction(Vehicle& v, const Road& road, double d) {
  if (v.on_last_road()) return true;
  const MovementId m = v.next_movement;
  if (m == kInvalidMovement) return false;
  const Movement& mv = net_.movement(m);
  if (v.committed) return true;
  if (net_.road(mv.out).closed) return false;

  bool clear = downstream_has_space(v, mv, v.lane) && crosswalks_clear(mv);
  // Intersection occupancy: yield to vehicles still clearing on conflicting movements,
  // including committed vehicles that have not yet crossed their stop line.
  const TrafficSignal* sig = signal_at(road.to);
  if (clear && sig) {
    for (MovementId other : sig->conflicts().movements()) {
      if (!sig->conflicts().conflicts(m, other)) continue;
      const Movement& om = net_.movement(other);
      for (const auto& c : om.connectors)
        if (!c.vehicles.empty()) { clear = false; break; }
      if (!clear) break;
      for (const auto& lane : net_.road(om.in).lanes) {
        if (lane.vehicles.empty()) continue;
        const Vehicle& front = vehicles_[lane.vehicles.front()];
        if (front.committed && front.next_movement == other) { clear = false; break; }
      }
      if (!clear) break;
    }
  }
  const double stop_dist = v.v * v.v / (2.0 * v.params.comfortable_decel);
  if (!sig) {
    if (clear && d < stop_dist + v.v + 5.0) v.committed = true;
    return clear;
  }
  switch (sig->movement_state(m)) {
    case LightState::Green:
      if (!clear) return false;
      if (d <= stop_dist + v.v + 2.0) v.committed = true;
      return true;
    case LightState::Yellow:
      // Dilemma-zone rule: proceed only if a comfortable stop is no longer possible.
      if (stop_dist > d - 0.5) {
        v.committed = true;
        return true;
      }
      return false;
    case LightState::Red:
      return false;
  }
  return false;
}

SimulationEngine::JunctionGap SimulationEngine::junction_gap(const Vehicle& me, const Movement& m, int conn,
                                                             double dist_to_out) const {
  JunctionGap best{kInf, 0.0};
  const Connector& c = m.connectors[static_cast<std::size_t>(conn)];
  const Road& out = net_.road(m.out);
  const Lane& lane = out.lanes[static_cast<std::size_t>(c.out_lane)];
  // Vehicles on any connector feeding the same target lane, compared by distance to the lane start.
  for (const auto& [fm, fc] : lane.feeders) {
    const Connector& fcon = net_.movement(fm).connectors[static_cast<std::size_t>(fc)];
    const double flen = fcon.path.length();
    for (SlotIndex s : fcon.vehicles) {
      const Vehicle& w = vehicles_[s];
      if (&w == &me) continue;
      const double wd = flen - w.s;
      if (wd < dist_to_out || (wd == dist_to_out && w.id < me.id)) {
        const double gap = dist_to_out - wd - w.params.length;
        if (gap < best.gap) best = {gap, w.v};
      }
    }
  }
  if (!lane.vehicles.empty()) {
    const Vehicle& tail = vehicles_[lane.vehicles.back()];
    const double gap = dist_to_out + tail.s - tail.params.length;
    if (gap < best.gap) best = {gap, tail.v};
  }
  if (lane.blockage) {
    const double gap = dist_to_out + *lane.blockage;
    if (gap < best.gap) best = {gap, 0.0};
  }
  return best;
}

void SimulationEngine::compute_accelerations() {
  for (auto& road : net_.roads) {
    for (auto& lane : road.lanes) {
      for (std::size_t i = 0; i < lane.vehicles.size(); ++i) {
        const SlotIndex slot = lane.vehicles[i];
        Vehicle& v = vehicles_[slot];
        double v0 = free_speed_on_road(v, road);
        const double d = road.length - v.s;
        if (!v.on_last_road() && v.next_movement != kInvalidMovement) {
          // Anticipate the turn: never approach faster than allows slowing to turn speed by the line.
          const double ts = free_speed_on_connector(v, net_.movement(v.next_movement));
          v0 = std::min(v0, std::sqrt(ts * ts + 2.0 * v.params.comfortable_decel * std::max(d, 0.0)));
        }
        double gap = kInf;
        double lead_v = 0.0;
        may_enter_[slot] = false;
        if (i > 0) {
          const Vehicle& leader = vehicles_[lane.vehicles[i - 1]];
          gap = leader.s - leader.params.length - v.s;
          lead_v = leader.v;
        } else {
          const bool ok = may_enter_junction(v, road, d);
          may_enter_[slot] = ok;
          if (!v.on_last_road()) {
            if (ok) {
              const Movement& m = net_.movement(v.next_movement);
              const auto jg = junction_gap(v, m, v.lane, d + m.connectors[static_cast<std::size_t>(v.lane)].path.length());
              gap = jg.gap;
              lead_v = jg.leader_speed;
            } else {
              gap = d + v.params.min_gap - 0.5;  // virtual stopped obstacle just past the stop line
              lead_v = 0.0;
            }
          }
        }
        if (lane.blockage && *lane.blockage >= v.s - 0.01) {
          const double gb = *lane.blockage - v.s;
          if (gb < gap) {
            gap = gb;
            lead_v = 0.0;
          }
        }
        v.a = idm_acceleration(v.params, v.v, v0, gap, v.v - lead_v);
      }
    }
  }
  for (auto& m : net_.movements) {
    for (std::size_t ci = 0; ci < m.connectors.size(); ++ci) {
      const auto& c = m.connectors[ci];
      for (SlotIndex slot : c.vehicles) {
        Vehicle& v = vehicles_[slot];
        const auto jg = junction_gap(v, m, static_cast<int>(ci), c.path.length() - v.s);
        v.a = idm_acceleration(v.params, v.v, free_speed_on_connector(v, m), jg.gap, v.v - jg.leader_speed);
      }
    }
  }
}

void SimulationEngine::enter_road(SlotIndex slot, RoadId road_id, int lane_idx, double s) {
  Vehicle& v = vehicles_[slot];
  Lane& lane = net_.road(road_id).lanes[static_cast<std::size_t>(lane_idx)];
  if (!lane.vehicles.empty()) {
    const Vehicle& tail = vehicles_[lane.vehicles.back()];
    const double max_s = tail.s - tail.params.length - 0.05;
    if (s > max_s) {
      metrics_.on_following_violation();
      s = std::max(0.0, max_s);
      v.v = std::min(v.v, tail.v);
    }
  }
  v.on_connector = false;
  v.movement = kInvalidMovement;
  v.road = road_id;
  v.lane = lane_idx;
  v.s = s;
  ++v.route_index;
  v.committed = false;
  v.queued_since = -1.0;
  set_next_movement(v);
  lane.vehicles.push_back(slot);
}

void SimulationEngine::complete_trip(SlotIndex slot) {
  Vehicle& v = vehicles_[slot];
  const Road& road = net_.road(v.road);
  const double t = time() + dt();
  const double ff = v.freeflow_time + road.length / std::max(free_speed_on_road(v, road), 0.1);
  metrics_.on_trip_completed(t - v.demand_time, ff, v.stopped_time, v.stops, v.measured, t);
  release(slot);
}

void SimulationEngine::integrate_and_transition() {
  const double h = dt();
  // Ballistic update (Treiber & Kanagaraj 2015): exact for constant acceleration and
  // never produces negative speeds.
  for (auto& v : vehicles_) {
    if (!v.active) continue;
    const double nv = v.v + v.a * h;
    if (nv < 0.0) {
      v.s += v.a < 0 ? -v.v * v.v / (2.0 * v.a) : 0.0;
      v.v = 0.0;
    } else {
      v.s += v.v * h + 0.5 * v.a * h * h;
      v.v = nv;
    }
  }
  // Ordering guard (should never trigger; counted as an invariant metric).
  auto guard = [this](std::vector<SlotIndex>& list) {
    for (std::size_t i = 1; i < list.size(); ++i) {
      const Vehicle& lead = vehicles_[list[i - 1]];
      Vehicle& v = vehicles_[list[i]];
      const double max_s = lead.s - lead.params.length - 0.05;
      if (v.s > max_s) {
        metrics_.on_following_violation();
        v.s = max_s;
        v.v = std::min(v.v, lead.v);
      }
    }
  };
  for (auto& road : net_.roads)
    for (auto& lane : road.lanes) guard(lane.vehicles);
  for (auto& m : net_.movements)
    for (auto& c : m.connectors) guard(c.vehicles);

  // Connector exits first so a vehicle can't traverse two segments in one step.
  for (auto& m : net_.movements) {
    for (auto& c : m.connectors) {
      const double len = c.path.length();
      while (!c.vehicles.empty() && vehicles_[c.vehicles.front()].s >= len) {
        const SlotIndex slot = c.vehicles.front();
        Vehicle& v = vehicles_[slot];
        const Lane& target = net_.road(m.out).lanes[static_cast<std::size_t>(c.out_lane)];
        if (!target.vehicles.empty()) {
          const Vehicle& tail = vehicles_[target.vehicles.back()];
          if (v.s - len > tail.s - tail.params.length - 0.05) {
            // No room yet: hold at the connector end rather than overlap (counted; should not happen).
            metrics_.on_following_violation();
            v.s = len;
            v.v = std::min(v.v, tail.v);
            break;
          }
        }
        c.vehicles.erase(c.vehicles.begin());
        v.freeflow_time += len / std::max(free_speed_on_connector(v, m), 0.1);
        enter_road(slot, m.out, c.out_lane, v.s - len);
      }
    }
  }
  for (auto& road : net_.roads) {
    for (auto& lane : road.lanes) {
      while (!lane.vehicles.empty() && vehicles_[lane.vehicles.front()].s >= road.length) {
        const SlotIndex slot = lane.vehicles.front();
        Vehicle& v = vehicles_[slot];
        if (v.on_last_road()) {
          lane.vehicles.erase(lane.vehicles.begin());
          complete_trip(slot);
          continue;
        }
        if (!may_enter_[slot]) {
          // Safety guard: an unpermitted vehicle never crosses the stop line.
          metrics_.on_red_light_guard();
          v.s = road.length - 0.01;
          v.v = 0.0;
          break;
        }
        lane.vehicles.erase(lane.vehicles.begin());
        Movement& m = net_.movement(v.next_movement);
        v.freeflow_time += road.length / std::max(free_speed_on_road(v, road), 0.1);
        v.on_connector = true;
        v.movement = m.id;
        v.connector = v.lane;
        v.s -= road.length;
        v.queued_since = -1.0;
        m.connectors[static_cast<std::size_t>(v.lane)].vehicles.push_back(slot);
        const int k = signal_of_node_[static_cast<std::size_t>(road.to)];
        if (k >= 0 && time() >= config_.warmup) {
          auto& st = istats_[static_cast<std::size_t>(k)];
          ++st.throughput;
          ++st.phases[static_cast<std::size_t>(signals_[static_cast<std::size_t>(k)].current_phase())].discharged;
        }
        may_enter_[slot] = false;  // only the front vehicle holds a permission
      }
    }
  }
}

// ============================================================ lane changes & routing

void SimulationEngine::change_lanes() {
  for (auto& road : net_.roads) {
    const int n = road.lane_count();
    if (n < 2) continue;
    for (int li = 0; li < n; ++li) {
      const std::vector<SlotIndex> snapshot_ids = road.lanes[static_cast<std::size_t>(li)].vehicles;
      for (SlotIndex slot : snapshot_ids) {
        Vehicle& v = vehicles_[slot];
        if (v.lane != li || v.on_connector || v.committed || v.lane_change_cooldown > 0) continue;
        const Lane& lane = road.lanes[static_cast<std::size_t>(li)];
        const TurnType turn =
            v.next_movement == kInvalidMovement ? TurnType::Straight : net_.movement(v.next_movement).turn;
        const bool need = !net_.lane_permits(road, li, turn);
        const bool blocked = lane.blockage && *lane.blockage > v.s && *lane.blockage - v.s < 80.0;
        if (!need && !blocked) continue;
        const double d = road.length - v.s;
        if (d < 3.0 || v.s - v.params.length < 20.0) continue;  // no lane changes right at junctions

        std::vector<int> targets;
        if (need) {
          int nearest = -1;
          for (int t = 0; t < n; ++t)
            if (net_.lane_permits(road, t, turn) && (nearest < 0 || std::abs(t - li) < std::abs(nearest - li))) nearest = t;
          if (nearest >= 0) targets.push_back(li + (nearest < li ? -1 : 1));
        } else {
          for (int t : {li - 1, li + 1})
            if (t >= 0 && t < n && net_.lane_permits(road, t, turn)) targets.push_back(t);
          for (int t : {li - 1, li + 1})
            if (t >= 0 && t < n && !net_.lane_permits(road, t, turn)) targets.push_back(t);
        }
        bool moved = false;
        for (int target : targets) {
          Lane& tl = road.lanes[static_cast<std::size_t>(target)];
          // Never move into a lane that is blocked ahead of us (a mandatory change can wait
          // until the obstacle has been passed in the current lane).
          if (tl.blockage && *tl.blockage > v.s - v.params.length) continue;
          // Gap acceptance in the target lane.
          const Vehicle* leader = nullptr;
          const Vehicle* follower = nullptr;
          std::size_t insert_at = tl.vehicles.size();
          for (std::size_t i = 0; i < tl.vehicles.size(); ++i) {
            const Vehicle& w = vehicles_[tl.vehicles[i]];
            if (w.s > v.s) leader = &w;
            else {
              follower = &w;
              insert_at = i;
              break;
            }
          }
          bool safe = true;
          if (leader && leader->s - leader->params.length - v.s < std::max(v.params.min_gap, 0.5 * v.v)) safe = false;
          if (tl.blockage && *tl.blockage > v.s && *tl.blockage - v.s < std::max(v.params.min_gap, 0.5 * v.v) + 2)
            safe = false;
          if (safe && follower) {
            const double back_gap = v.s - v.params.length - follower->s;
            if (back_gap < std::max(follower->params.min_gap, 0.5 * follower->v)) safe = false;
            else if (idm_acceleration(follower->params, follower->v, free_speed_on_road(*follower, road), back_gap,
                                      follower->v - v.v) < -4.0)
              safe = false;
          }
          // Vehicles about to leave a junction connector into the target lane are followers too.
          for (const auto& [fm, fc] : tl.feeders) {
            const Connector& con = net_.movement(fm).connectors[static_cast<std::size_t>(fc)];
            for (SlotIndex ws : con.vehicles) {
              const Vehicle& w = vehicles_[ws];
              const double back_gap = v.s - v.params.length - (w.s - con.path.length());
              if (back_gap < std::max(w.params.min_gap, 0.5 * w.v) + w.v) safe = false;
            }
          }
          if (!safe) continue;
          auto& from = road.lanes[static_cast<std::size_t>(li)].vehicles;
          from.erase(std::find(from.begin(), from.end(), slot));
          tl.vehicles.insert(tl.vehicles.begin() + static_cast<std::ptrdiff_t>(insert_at), slot);
          v.lateral_offset += road.lane_offset(li) - road.lane_offset(target);
          v.lane = target;
          v.lane_change_cooldown = kLaneChangeCooldown;
          metrics_.on_lane_change();
          moved = true;
          break;
        }
        // Couldn't reach a lane for the planned turn: pick a turn permitted from this lane.
        if (!moved && need && d < 25.0 && v.v < 1.0) forced_turn_reroute(v);
      }
    }
  }
}

bool SimulationEngine::forced_turn_reroute(Vehicle& v) {
  const Road& road = net_.road(v.road);
  const auto cost = [this](const Road& r) { return route_cost(r); };
  std::optional<std::vector<RoadId>> best;
  double best_cost = kInf;
  for (const auto& [out, mid] : road.next) {
    if (!net_.lane_permits(road, v.lane, net_.movement(mid).turn) || net_.road(out).closed) continue;
    auto r = shortest_route(net_, {out}, v.destination, cost);
    if (!r) continue;
    double c = 0.0;
    for (RoadId x : *r) c += cost(net_.road(x));
    if (c < best_cost) {
      best_cost = c;
      best = std::move(r);
    }
  }
  if (!best) return false;
  v.route.resize(v.route_index + 1);
  v.route.insert(v.route.end(), best->begin(), best->end());
  ++v.reroutes;
  metrics_.on_forced_reroute();
  set_next_movement(v);
  return true;
}

bool SimulationEngine::reroute(Vehicle& v) {
  const auto cost = [this](const Road& r) { return route_cost(r); };
  // A vehicle on a connector is committed to its outgoing road; plan from there.
  const std::size_t keep = v.on_connector ? v.route_index + 1 : v.route_index;
  const RoadId from = v.route[keep];
  auto r = shortest_route(net_, {from}, v.destination, cost, true);
  if (!r) {
    // Destination unreachable: divert to the cheapest reachable exit.
    double best = kInf;
    for (NodeId b : net_.boundary_nodes()) {
      if (net_.node(b).in.empty() || b == v.origin) continue;
      auto alt = shortest_route(net_, {from}, b, cost, true);
      if (!alt) continue;
      double c = 0.0;
      for (RoadId x : *alt) c += cost(net_.road(x));
      if (c < best) {
        best = c;
        r = std::move(alt);
        v.destination = b;
      }
    }
  }
  if (!r) return false;
  v.route.resize(keep);
  v.route.insert(v.route.end(), r->begin(), r->end());
  ++v.reroutes;
  if (!v.on_connector) {
    v.committed = v.committed && v.next_road() != kInvalidRoad && !net_.road(v.next_road()).closed;
    set_next_movement(v);
  }
  return true;
}

void SimulationEngine::reroute_all_using(RoadId closed) {
  for (auto& v : vehicles_) {
    if (!v.active) continue;
    const std::size_t first = v.route_index + (v.on_connector ? 2 : 1);
    bool uses = false;
    for (std::size_t i = first; i < v.route.size(); ++i)
      if (v.route[i] == closed) uses = true;
    if (uses) reroute(v);
  }
}

// ============================================================ bookkeeping

void SimulationEngine::update_bookkeeping() {
  const double h = dt();
  const double t = time();
  double speed_sum = 0.0;
  for (auto& v : vehicles_) {
    if (!v.active) continue;
    speed_sum += v.v;
    if (v.v < kStopSpeed) {
      v.stopped_time += h;
      if (!v.is_stopped) {
        v.is_stopped = true;
        ++v.stops;
      }
    } else if (v.v > kQueueSpeed) {
      v.is_stopped = false;
    }
    if (!v.on_connector && v.v < kQueueSpeed && v.queued_since < 0) v.queued_since = t;
    if (v.lateral_offset != 0.0) {
      const double mag = std::max(0.0, std::abs(v.lateral_offset) - 1.5 * h);
      v.lateral_offset = std::copysign(mag, v.lateral_offset);
    }
    if (v.lane_change_cooldown > 0) v.lane_change_cooldown -= h;
  }
  int total_queue = 0;
  int max_queue = 0;
  for (std::size_t k = 0; k < signals_.size(); ++k) {
    int node_queue = 0;
    for (RoadId rid : net_.node(signals_[k].node()).in) {
      int q = 0;
      for (const auto& lane : net_.road(rid).lanes)
        for (SlotIndex s : lane.vehicles)
          if (vehicles_[s].v < kQueueSpeed) ++q;
      node_queue += q;
      max_queue = std::max(max_queue, q);
      if (t >= config_.warmup) istats_[k].max_queue = std::max(istats_[k].max_queue, q);
    }
    if (t >= config_.warmup) queue_integral_[k] += node_queue * h;
    total_queue += node_queue;
  }
  int peds_waiting = 0;
  for (const auto& p : peds_.pedestrians()) peds_waiting += p.crossing() ? 0 : 1;
  metrics_.on_step(t, h, total_queue, max_queue, active_, backlog_count(), speed_sum, peds_waiting);
}

double SimulationEngine::unfinished_delay(long* measured_count) const {
  const double t = time();
  double delay = 0.0;
  long n = 0;
  for (const auto& v : vehicles_) {
    if (!v.active || !v.measured) continue;
    double ff = v.freeflow_time;
    if (!v.on_connector) ff += v.s / std::max(free_speed_on_road(v, net_.road(v.road)), 0.1);
    else ff += v.s / std::max(free_speed_on_connector(v, net_.movement(v.movement)), 0.1);
    delay += std::max(0.0, (t - v.demand_time) - ff);
    ++n;
  }
  for (const auto& q : backlog_)
    for (const auto& pending : q)
      if (pending.trip.demand_time >= config_.warmup) {
        delay += t - pending.trip.demand_time;
        ++n;
      }
  *measured_count = n;
  return delay;
}

MetricsSummary SimulationEngine::metrics() const {
  long unfinished = 0;
  const double ud = unfinished_delay(&unfinished);
  long switches = 0;
  double clearance = 0.0;
  for (const auto& s : signals_) {
    switches += s.switches();
    clearance += s.clearance_time();
  }
  const double denom = time() * static_cast<double>(signals_.size());
  MetricsSummary m = metrics_.summary(time(), ud, unfinished, switches, denom > 0 ? clearance / denom : 0.0);
  m.in_network = active_;
  m.backlog = backlog_count();
  return m;
}

std::vector<IntersectionStats> SimulationEngine::intersection_stats() const {
  std::vector<IntersectionStats> out = istats_;
  const double measured = std::max(0.0, time() - config_.warmup);
  for (std::size_t k = 0; k < out.size(); ++k) {
    out[k].avg_queue = measured > 0 ? queue_integral_[k] / measured : 0.0;
    out[k].switches = signals_[k].switches();
    out[k].clearance_time = signals_[k].clearance_time();
  }
  return out;
}

// ============================================================ commands

CommandResult SimulationEngine::apply(const EngineCommand& command) {
  CommandResult r = apply_impl(command);
  if (r.ok) log_.push_back({clock_.step(), command});
  return r;
}

CommandResult SimulationEngine::apply_impl(const EngineCommand& command) {
  auto valid_road = [this](RoadId r) { return r >= 0 && static_cast<std::size_t>(r) < net_.roads.size(); };
  return std::visit(
      overloaded{
          [&](const CmdSetDemand& c) {
            if (!(c.multiplier >= 0.0 && c.multiplier <= 5.0)) return CommandResult::fail("demand multiplier must be in [0, 5]");
            spawner_.set_multiplier(c.multiplier);
            return CommandResult::success();
          },
          [&](const CmdSetPedestrianDemand& c) {
            if (!(c.multiplier >= 0.0 && c.multiplier <= 10.0))
              return CommandResult::fail("pedestrian multiplier must be in [0, 10]");
            peds_.set_multiplier(c.multiplier);
            return CommandResult::success();
          },
          [&](const CmdSetVehicleType& c) {
            if (c.type == VehicleType::Car) return CommandResult::fail("cars cannot be disabled");
            spawner_.set_type_enabled(c.type, c.enabled);
            return CommandResult::success();
          },
          [&](const CmdCloseRoad& c) {
            if (!valid_road(c.road)) return CommandResult::fail("unknown road");
            Road& r = net_.road(c.road);
            if (net_.node(r.from).boundary || net_.node(r.to).boundary)
              return CommandResult::fail("boundary entry/exit roads cannot be closed (close an interior road instead)");
            if (r.closed) return CommandResult::success();
            r.closed = true;
            reroute_all_using(c.road);
            return CommandResult::success();
          },
          [&](const CmdOpenRoad& c) {
            if (!valid_road(c.road)) return CommandResult::fail("unknown road");
            net_.road(c.road).closed = false;
            return CommandResult::success();
          },
          [&](const CmdBlockLane& c) {
            if (!valid_road(c.road)) return CommandResult::fail("unknown road");
            Road& r = net_.road(c.road);
            if (c.lane < 0 || c.lane >= r.lane_count()) return CommandResult::fail("lane out of range");
            if (!(c.position >= 40.0 && c.position <= r.length - 5.0))
              return CommandResult::fail("blockage position must be >= 40 m from the lane start and >= 5 m from its end");
            int blocked = 0;
            for (const auto& l : r.lanes) blocked += l.blockage ? 1 : 0;
            if (!r.lanes[static_cast<std::size_t>(c.lane)].blockage && blocked + 1 >= r.lane_count())
              return CommandResult::fail("at least one lane must stay open (close the road instead)");
            r.lanes[static_cast<std::size_t>(c.lane)].blockage = c.position;
            return CommandResult::success();
          },
          [&](const CmdClearLane& c) {
            if (!valid_road(c.road)) return CommandResult::fail("unknown road");
            Road& r = net_.road(c.road);
            if (c.lane < 0 || c.lane >= r.lane_count()) return CommandResult::fail("lane out of range");
            r.lanes[static_cast<std::size_t>(c.lane)].blockage.reset();
            return CommandResult::success();
          },
          [&](const CmdSetController& c) {
            auto errs = validate_controller_config(c.config);
            if (!errs.empty()) return CommandResult::fail(errs.front());
            ControllerConfig cfg = c.config;
            if (cfg.node_greens.empty()) cfg.node_greens = controller_cfg_.node_greens;  // keep edited plans
            build_controllers(cfg);
            return CommandResult::success();
          },
          [&](const CmdSetSignalPlan& c) {
            const int k = (c.node >= 0 && static_cast<std::size_t>(c.node) < signal_of_node_.size())
                              ? signal_of_node_[static_cast<std::size_t>(c.node)]
                              : -1;
            if (k < 0) return CommandResult::fail("node is not signalised");
            const auto& phases = signals_[static_cast<std::size_t>(k)].phases();
            if (c.greens.size() != phases.size())
              return CommandResult::fail("expected " + std::to_string(phases.size()) + " green durations");
            for (std::size_t i = 0; i < phases.size(); ++i)
              if (!(c.greens[i] >= phases[i].min_green && c.greens[i] <= phases[i].max_green))
                return CommandResult::fail("green for phase " + std::to_string(i + 1) + " must be within [" +
                                           std::to_string(static_cast<int>(phases[i].min_green)) + ", " +
                                           std::to_string(static_cast<int>(phases[i].max_green)) + "] s");
            controller_cfg_.node_greens[c.node] = c.greens;
            if (auto* fixed = dynamic_cast<FixedTimeController*>(controllers_[static_cast<std::size_t>(k)].get()))
              fixed->set_greens(c.greens);
            return CommandResult::success();
          },
          [&](const CmdSetPhaseTiming& c) {
            const int k = (c.node >= 0 && static_cast<std::size_t>(c.node) < signal_of_node_.size())
                              ? signal_of_node_[static_cast<std::size_t>(c.node)]
                              : -1;
            if (k < 0) return CommandResult::fail("node is not signalised");
            std::string err;
            if (!signals_[static_cast<std::size_t>(k)].set_phase_timing(c.phase, c.min_green, c.max_green, &err))
              return CommandResult::fail(err);
            controllers_[static_cast<std::size_t>(k)]->initialize(
                IntersectionConfig{signals_[static_cast<std::size_t>(k)].node(), signals_[static_cast<std::size_t>(k)].phases()});
            return CommandResult::success();
          },
          [&](const CmdSetSpeedLimit& c) {
            if (!valid_road(c.road)) return CommandResult::fail("unknown road");
            if (!(c.kmh >= 10.0 && c.kmh <= 130.0)) return CommandResult::fail("speed limit must be in [10, 130] km/h");
            net_.road(c.road).speed_limit = c.kmh / 3.6;
            return CommandResult::success();
          },
          [&](const CmdSpawnVehicle& c) {
            auto ok_node = [this](NodeId n) {
              return n >= 0 && static_cast<std::size_t>(n) < net_.nodes.size() && net_.node(n).boundary;
            };
            if (!ok_node(c.origin) || !ok_node(c.destination) || c.origin == c.destination)
              return CommandResult::fail("origin and destination must be distinct boundary nodes");
            if (net_.node(c.origin).out.empty() || net_.node(c.destination).in.empty())
              return CommandResult::fail("origin must emit and destination must accept traffic");
            if (!route_between(net_, c.origin, c.destination, [this](const Road& r) { return route_cost(r); }))
              return CommandResult::fail("no open route between those nodes");
            auto route = route_between(net_, c.origin, c.destination, [this](const Road& r) { return route_cost(r); });
            TripRequest trip = spawner_.manual_trip(c.type, c.origin, c.destination, time());
            metrics_.on_trip_requested();
            backlog_[static_cast<std::size_t>(c.origin)].push_front({trip, std::move(*route)});
            return CommandResult::success();
          },
      },
      command);
}

// ============================================================ observation & verification

std::pair<Vec2, double> SimulationEngine::vehicle_pose(const Vehicle& v) const {
  const double back = v.s - v.params.length / 2.0;
  if (!v.on_connector) {
    const Road& r = net_.road(v.road);
    const Vec2 p = r.lane_point(v.lane, back) + r.dir.right_normal() * v.lateral_offset;
    return {p, std::atan2(r.dir.y, r.dir.x)};
  }
  const Movement& m = net_.movement(v.movement);
  const Connector& c = m.connectors[static_cast<std::size_t>(v.connector)];
  if (back >= 0) {
    const Vec2 t = c.path.tangent_at(back);
    return {c.path.point_at(back), std::atan2(t.y, t.x)};
  }
  const Road& in = net_.road(m.in);
  return {in.lane_point(v.connector, in.length + back), std::atan2(in.dir.y, in.dir.x)};
}

SimulationSnapshot SimulationEngine::snapshot() const {
  SimulationSnapshot snap;
  snap.step = clock_.step();
  snap.time = time();
  snap.vehicles.reserve(static_cast<std::size_t>(active_));
  for (const auto& v : vehicles_) {
    if (!v.active) continue;
    auto [p, heading] = vehicle_pose(v);
    snap.vehicles.push_back({v.id, static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(heading),
                             static_cast<float>(v.v), static_cast<float>(v.params.length), v.type, v.v < kStopSpeed});
  }
  for (const auto& p : peds_.pedestrians()) {
    const Vec2 pos = peds_.position(p);
    snap.pedestrians.push_back({p.id, static_cast<float>(pos.x), static_cast<float>(pos.y), p.crossing()});
  }
  for (std::size_t k = 0; k < signals_.size(); ++k) {
    const auto& s = signals_[k];
    SignalView sv;
    sv.node = s.node();
    sv.phase = s.current_phase();
    sv.next_phase = s.next_phase();
    sv.stage = s.stage();
    sv.time_in_stage = static_cast<float>(s.time_in_stage());
    sv.movements = s.movement_states();
    sv.crosswalks = s.crosswalk_states();
    sv.reason = last_reasons_[k];
    snap.signals.push_back(std::move(sv));
    for (RoadId rid : net_.node(s.node()).in) {
      int q = 0;
      for (const auto& lane : net_.road(rid).lanes)
        for (SlotIndex slot : lane.vehicles)
          if (vehicles_[slot].v < kQueueSpeed) ++q;
      snap.queues.push_back({rid, q});
    }
  }
  for (const auto& r : net_.roads) {
    if (r.closed) snap.closed_roads.push_back(r.id);
    for (const auto& l : r.lanes)
      if (l.blockage) snap.blockages.push_back({r.id, l.index, *l.blockage});
  }
  snap.metrics = metrics();
  snap.demand_multiplier = spawner_.multiplier();
  snap.pedestrian_multiplier = peds_.multiplier();
  snap.demand_vph = spawner_.total_rate_vph(time());
  snap.controller = controller_cfg_.display_name();
  return snap;
}

std::uint64_t SimulationEngine::state_hash() const {
  std::uint64_t h = 1469598103934665603ULL;
  const std::uint64_t step = clock_.step();
  hash_value(h, step);
  for (std::size_t i = 0; i < vehicles_.size(); ++i) {
    const auto& v = vehicles_[i];
    if (!v.active) continue;
    hash_value(h, i);
    hash_value(h, v.id);
    hash_value(h, v.road);
    hash_value(h, v.lane);
    hash_value(h, v.on_connector);
    hash_value(h, v.s);
    hash_value(h, v.v);
    hash_value(h, v.route_index);
  }
  for (const auto& s : signals_) {
    hash_value(h, s.current_phase());
    hash_value(h, s.stage());
    hash_value(h, s.time_in_stage());
  }
  for (const auto& p : peds_.pedestrians()) {
    hash_value(h, p.id);
    hash_value(h, p.progress);
  }
  return h;
}

bool SimulationEngine::check_invariants(std::string* why) const {
  auto fail = [why](std::string msg) {
    if (why) *why = std::move(msg);
    return false;
  };
  std::set<VehicleId> ids;
  int counted = 0;
  for (std::size_t slot = 0; slot < vehicles_.size(); ++slot) {
    const auto& v = vehicles_[slot];
    if (!v.active) continue;
    ++counted;
    if (!ids.insert(v.id).second) return fail("duplicate vehicle id " + std::to_string(v.id));
    if (!std::isfinite(v.s) || !std::isfinite(v.v)) return fail("non-finite vehicle state");
    if (v.v < 0.0 || v.v > 60.0) return fail("invalid speed " + std::to_string(v.v));
    if (!v.on_connector) {
      const Road& r = net_.road(v.road);
      if (v.s < -0.01 || v.s > r.length + 0.01) return fail("vehicle outside its lane bounds");
      const auto& list = r.lanes[static_cast<std::size_t>(v.lane)].vehicles;
      if (std::find(list.begin(), list.end(), slot) == list.end()) return fail("vehicle missing from its lane list");
    } else {
      const auto& c = net_.movement(v.movement).connectors[static_cast<std::size_t>(v.connector)];
      if (v.s < -0.01 || v.s > c.path.length() + 0.01) return fail("vehicle outside its connector");
    }
  }
  if (counted != active_) return fail("active count mismatch");
  for (const auto& r : net_.roads)
    for (const auto& l : r.lanes)
      for (std::size_t i = 1; i < l.vehicles.size(); ++i) {
        const auto& a = vehicles_[l.vehicles[i - 1]];
        const auto& b = vehicles_[l.vehicles[i]];
        if (b.s > a.s - a.params.length + 0.01)
          return fail("vehicles " + std::to_string(a.id) + " (s=" + std::to_string(a.s) + ") and " +
                      std::to_string(b.id) + " (s=" + std::to_string(b.s) + ") overlap on road " +
                      std::to_string(r.id) + " lane " + std::to_string(l.index));
      }
  for (const auto& s : signals_) {
    std::string reason;
    if (!s.invariants_hold(&reason)) return fail("signal at node " + std::to_string(s.node()) + ": " + reason);
  }
  return true;
}

}  // namespace traffic
