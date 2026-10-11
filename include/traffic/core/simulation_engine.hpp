#pragma once
// SimulationEngine owns all mutable world state and advances it in fixed steps.
// It is single-threaded by design: exactly one thread (the simulation thread or a
// test/experiment) may call non-const methods. It has no knowledge of networking,
// JSON, files or wall-clock time.
//
// Update order per step (see docs/simulation-model.md for the rationale):
//   1. scheduled scenario events          5. pedestrians start/advance
//   2. demand generation + network entry  6. vehicle accelerations (read-only pass)
//   3. pedestrian arrivals                7. integration + road/junction transitions
//   4. observations -> controllers ->      8. lane changes
//      signal state machines              9. metrics, clock advance

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "traffic/controllers/signal_controller.hpp"
#include "traffic/core/commands.hpp"
#include "traffic/core/config.hpp"
#include "traffic/core/simulation_clock.hpp"
#include "traffic/core/snapshot.hpp"
#include "traffic/metrics/metrics_collector.hpp"
#include "traffic/network/road_network.hpp"
#include "traffic/pedestrians/pedestrian.hpp"
#include "traffic/signals/traffic_signal.hpp"
#include "traffic/vehicles/vehicle.hpp"
#include "traffic/vehicles/vehicle_spawner.hpp"

namespace traffic {

struct PhaseStats {
  std::string name;
  double green_time = 0.0;
  long discharged = 0;    // vehicles that entered the junction while this phase was active
  double arrivals = 0.0;  // vehicles that arrived wanting a movement of this phase (split across phases)
};

struct IntersectionStats {
  NodeId node = kInvalidNode;
  long throughput = 0;
  double avg_queue = 0.0;
  int max_queue = 0;
  long switches = 0;
  double clearance_time = 0.0;
  std::vector<PhaseStats> phases;
};

struct EngineCreateResult {
  std::unique_ptr<class SimulationEngine> engine;
  std::vector<std::string> errors;
};

class SimulationEngine {
 public:
  /// Builds and validates the network, phases and controllers. Never throws on bad input.
  static EngineCreateResult create(const ScenarioConfig& config);

  SimulationEngine(const SimulationEngine&) = delete;
  SimulationEngine& operator=(const SimulationEngine&) = delete;

  void step();
  void run_for(double seconds);
  void run_until(double time);

  /// Validates and applies a command before the next step; successful commands are logged.
  CommandResult apply(const EngineCommand& command);

  [[nodiscard]] SimulationSnapshot snapshot() const;
  [[nodiscard]] MetricsSummary metrics() const;
  [[nodiscard]] const std::vector<TimeSample>& series() const { return metrics_.series(); }
  [[nodiscard]] std::vector<IntersectionStats> intersection_stats() const;

  [[nodiscard]] double time() const { return clock_.time(); }
  [[nodiscard]] std::uint64_t step_count() const { return clock_.step(); }
  [[nodiscard]] double dt() const { return clock_.dt(); }
  [[nodiscard]] const RoadNetwork& network() const { return net_; }
  [[nodiscard]] const ScenarioConfig& scenario() const { return config_; }
  [[nodiscard]] const std::vector<TrafficSignal>& signals() const { return signals_; }
  [[nodiscard]] const TrafficSignal* signal_at(NodeId node) const;
  [[nodiscard]] const ControllerConfig& controller_config() const { return controller_cfg_; }
  [[nodiscard]] const std::vector<LoggedCommand>& command_log() const { return log_; }
  [[nodiscard]] int vehicle_count() const { return active_; }
  [[nodiscard]] int backlog_count() const;
  [[nodiscard]] const PedestrianModel& pedestrians() const { return peds_; }

  /// Read-only iteration over active vehicles in deterministic (slot) order.
  template <typename F>
  void for_each_vehicle(F&& f) const {
    for (const auto& v : vehicles_)
      if (v.active) f(v);
  }

  /// Hash of the full dynamic state (bit-exact). Equal hashes => identical runs.
  [[nodiscard]] std::uint64_t state_hash() const;
  /// Checks physical/safety invariants; returns false with a description on failure.
  [[nodiscard]] bool check_invariants(std::string* why = nullptr) const;

 private:
  SimulationEngine(ScenarioConfig config, RoadNetwork net);

  struct JunctionGap {
    double gap;
    double leader_speed;
  };
  struct MovementObs {
    int queued = 0, approaching = 0, arriving_soon = 0;
    double wait_sum = 0.0, oldest = 0.0;
    bool emergency = false;
  };

  // --- step phases
  void apply_due_events();
  void generate_demand();
  void admit_from_backlog();
  void update_signals();
  void compute_accelerations();
  void integrate_and_transition();
  void change_lanes();
  void update_bookkeeping();

  // --- helpers
  SlotIndex allocate_slot();
  void release(SlotIndex slot);
  double free_speed_on_road(const Vehicle& v, const Road& r) const;
  double free_speed_on_connector(const Vehicle& v, const Movement& m) const;
  bool may_enter_junction(Vehicle& v, const Road& road, double dist_to_line);
  bool downstream_has_space(const Vehicle& v, const Movement& m, int in_lane) const;
  bool crosswalks_clear(const Movement& m) const;
  JunctionGap junction_gap(const Vehicle& me, const Movement& m, int conn, double dist_to_out) const;
  void set_next_movement(Vehicle& v);
  bool reroute(Vehicle& v);
  bool forced_turn_reroute(Vehicle& v);
  void reroute_all_using(RoadId closed);
  double route_cost(const Road& r) const;
  void complete_trip(SlotIndex slot);
  void enter_road(SlotIndex slot, RoadId road, int lane, double s);
  std::pair<Vec2, double> vehicle_pose(const Vehicle& v) const;
  CommandResult apply_impl(const EngineCommand& command);
  void build_controllers(const ControllerConfig& cfg);
  double unfinished_delay(long* measured_count) const;

  ScenarioConfig config_;
  RoadNetwork net_;
  SimulationClock clock_;
  ControllerConfig controller_cfg_;

  std::vector<Vehicle> vehicles_;   // slot array (contiguous; slots reused via free list)
  std::vector<SlotIndex> free_slots_;
  std::vector<bool> may_enter_;     // per slot: junction permission computed this step
  int active_ = 0;

  VehicleSpawner spawner_;
  std::vector<std::deque<TripRequest>> backlog_;  // per origin node
  std::vector<TripRequest> trip_buffer_;

  PedestrianModel peds_;
  std::vector<PedestrianCompletion> ped_done_;

  std::vector<TrafficSignal> signals_;
  std::vector<int> signal_of_node_;  // node id -> index into signals_, -1 if none
  std::vector<std::unique_ptr<SignalController>> controllers_;
  std::vector<std::vector<MovementObs>> movement_obs_;  // per signal, per local movement

  MetricsCollector metrics_;
  std::vector<IntersectionStats> istats_;
  std::vector<double> queue_integral_;  // per signal

  std::vector<EventSpec> events_;
  std::size_t next_event_ = 0;
  std::vector<LoggedCommand> log_;
  std::vector<std::string> last_reasons_;
};

}  // namespace traffic
