#include "traffic/metrics/metrics_collector.hpp"

#include <algorithm>

#include "traffic/metrics/statistics.hpp"

namespace traffic {

namespace {
constexpr double kThroughputWindow = 60.0;  // s
}

void MetricsCollector::on_trip_completed(double travel_time, double freeflow_time, double stopped_time, int stops,
                                         bool measured, double t) {
  ++s_.completed;
  recent_completions_.push_back(t);
  if (!measured) return;
  ++s_.measured_completed;
  travel_times_.push_back(travel_time);
  delay_sum_ += std::max(0.0, travel_time - freeflow_time);
  stopped_sum_ += stopped_time;
  stops_sum_ += stops;
}

void MetricsCollector::on_pedestrian_completed(double wait, bool measured) {
  ++s_.peds_completed;
  if (!measured) return;
  ++ped_measured_;
  ped_wait_sum_ += wait;
  s_.ped_max_wait = std::max(s_.ped_max_wait, wait);
}

void MetricsCollector::on_step(double t, double dt, int total_queue, int max_approach_queue, int in_network,
                               int backlog, double speed_sum, int peds_waiting) {
  if (t >= measure_from_) {
    queue_integral_ += total_queue * dt;
    measured_seconds_ += dt;
    s_.max_queue = std::max(s_.max_queue, max_approach_queue);
  }
  if (t + 1e-9 >= next_sample_) {
    next_sample_ += sample_interval_;
    std::erase_if(recent_completions_, [&](double c) { return c < t - kThroughputWindow; });
    TimeSample ts;
    ts.t = t;
    ts.in_network = in_network;
    ts.backlog = backlog;
    ts.avg_speed = in_network > 0 ? speed_sum / in_network : 0.0;
    ts.total_queue = total_queue;
    ts.completed = s_.completed;
    ts.throughput_vph = static_cast<double>(recent_completions_.size()) * 3600.0 / std::min(kThroughputWindow, std::max(t, dt));
    ts.peds_waiting = peds_waiting;
    ts.avg_delay = s_.measured_completed > 0 ? delay_sum_ / static_cast<double>(s_.measured_completed) : 0.0;
    series_.push_back(ts);
  }
}

MetricsSummary MetricsCollector::summary(double t, double unfinished_delay, long unfinished_measured, long switches,
                                         double clearance_fraction) const {
  MetricsSummary out = s_;
  out.sim_time = t;
  out.measured_seconds = measured_seconds_;
  const auto n = static_cast<double>(s_.measured_completed);
  if (n > 0) {
    out.avg_travel_time = stats::mean(travel_times_);
    out.median_travel_time = *stats::percentile(travel_times_, 0.5);
    if (travel_times_.size() >= 20) out.p95_travel_time = stats::percentile(travel_times_, 0.95);
    out.avg_delay = delay_sum_ / n;
    out.avg_stopped_time = stopped_sum_ / n;
    out.avg_stops = stops_sum_ / n;
  }
  const double all = n + static_cast<double>(unfinished_measured);
  out.avg_delay_all = all > 0 ? (delay_sum_ + unfinished_delay) / all : 0.0;
  out.total_delay_hours = (delay_sum_ + unfinished_delay) / 3600.0;
  out.avg_queue = measured_seconds_ > 0 ? queue_integral_ / measured_seconds_ : 0.0;
  out.throughput_vph = measured_seconds_ > 0 ? n * 3600.0 / measured_seconds_ : 0.0;
  out.ped_avg_wait = ped_measured_ > 0 ? ped_wait_sum_ / static_cast<double>(ped_measured_) : 0.0;
  out.phase_switches = switches;
  out.clearance_fraction = clearance_fraction;
  return out;
}

}  // namespace traffic
