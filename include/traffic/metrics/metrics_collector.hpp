#pragma once
// Metric definitions (see docs/simulation-model.md#metrics for the full text):
//  * travel time  = exit time - demand time (includes waiting to enter the network)
//  * free-flow    = sum over traversed segments of length / free speed on that segment
//  * delay        = travel time - free-flow time
//  * stopped time = time spent with speed < 0.5 m/s; a stop starts when speed drops
//                   below 0.5 m/s after having exceeded 2 m/s (hysteresis)
//  * queue        = vehicles with speed < 2 m/s on roads entering signalised nodes;
//                   averaged over time (time-weighted), not over samples
// Only vehicles/pedestrians whose demand time is >= the warm-up time are "measured".

#include <optional>
#include <vector>

#include "traffic/core/types.hpp"

namespace traffic {

struct MetricsSummary {
  double sim_time = 0.0;
  double measured_seconds = 0.0;  // time integrated after warm-up
  long spawned = 0;               // trips requested (all)
  long entered = 0;               // vehicles that physically entered the network
  long completed = 0;             // all completed trips
  long measured_completed = 0;    // completed trips that count towards averages
  long in_network = 0;
  long backlog = 0;               // trips waiting at sources to enter
  long unserved = 0;              // trips with no route (e.g. after closures)
  double avg_travel_time = 0.0;
  double median_travel_time = 0.0;
  std::optional<double> p95_travel_time;  // only with >= 20 samples
  double avg_delay = 0.0;          // completed measured trips
  double avg_delay_all = 0.0;      // includes delay accrued so far by unfinished trips
  double total_delay_hours = 0.0;  // completed + unfinished, vehicle-hours
  double avg_stopped_time = 0.0;
  double avg_stops = 0.0;
  double avg_queue = 0.0;          // time-weighted network-wide queued vehicles
  int max_queue = 0;               // largest single-approach queue observed
  double throughput_vph = 0.0;     // measured completions per simulated hour
  long peds_completed = 0;
  double ped_avg_wait = 0.0;
  double ped_max_wait = 0.0;
  long phase_switches = 0;
  double clearance_fraction = 0.0;  // share of signal time spent in yellow/all-red
  long red_light_guard_events = 0;  // invariant guard triggered (should be 0)
  long following_violations = 0;    // overlap guard triggered (should be 0)
  long forced_reroutes = 0;
  long lane_changes = 0;
};

struct TimeSample {
  double t = 0.0;
  int in_network = 0;
  int backlog = 0;
  double avg_speed = 0.0;
  int total_queue = 0;
  long completed = 0;
  double throughput_vph = 0.0;  // completions in the last window, per hour
  int peds_waiting = 0;
  double avg_delay = 0.0;       // running average delay of measured completed trips
};

class MetricsCollector {
 public:
  explicit MetricsCollector(double measure_from = 0.0, double sample_interval = 5.0)
      : measure_from_(measure_from), sample_interval_(sample_interval) {}

  [[nodiscard]] double measure_from() const { return measure_from_; }

  void on_trip_requested() { ++s_.spawned; }
  void on_vehicle_entered() { ++s_.entered; }
  void on_unserved() { ++s_.unserved; }
  void on_lane_change() { ++s_.lane_changes; }
  void on_forced_reroute() { ++s_.forced_reroutes; }
  void on_red_light_guard() { ++s_.red_light_guard_events; }
  void on_following_violation() { ++s_.following_violations; }
  void on_trip_completed(double travel_time, double freeflow_time, double stopped_time, int stops,
                         bool measured, double t);
  void on_pedestrian_completed(double wait, bool measured);

  /// Called once per step with network-level aggregates.
  void on_step(double t, double dt, int total_queue, int max_approach_queue, int in_network, int backlog,
               double speed_sum, int peds_waiting);

  /// Finalises a summary; `unfinished_delay` is accrued delay of vehicles still travelling.
  [[nodiscard]] MetricsSummary summary(double t, double unfinished_delay, long unfinished_measured,
                                       long switches, double clearance_fraction) const;
  [[nodiscard]] const std::vector<TimeSample>& series() const { return series_; }

 private:
  double measure_from_;
  double sample_interval_;
  MetricsSummary s_;
  std::vector<double> travel_times_;
  double delay_sum_ = 0.0;
  double stopped_sum_ = 0.0;
  double stops_sum_ = 0.0;
  double queue_integral_ = 0.0;
  double measured_seconds_ = 0.0;
  double ped_wait_sum_ = 0.0;
  long ped_measured_ = 0;
  std::vector<TimeSample> series_;
  double next_sample_ = 0.0;
  std::vector<double> recent_completions_;  // completion times inside the rolling window
};

}  // namespace traffic
