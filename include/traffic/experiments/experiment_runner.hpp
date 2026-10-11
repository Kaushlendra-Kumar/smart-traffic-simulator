#pragma once
// Runs the same scenario under several controllers and seeds. For a given seed every
// controller sees the identical demand realisation (same arrivals, origins,
// destinations, vehicle classes and driver parameters), so differences are paired.
// Independent runs share nothing and are executed on a worker pool.

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "traffic/core/config.hpp"
#include "traffic/core/simulation_engine.hpp"

namespace traffic {

struct ExperimentSpec {
  std::string id;
  ScenarioConfig scenario;
  std::vector<ControllerConfig> controllers;  // first entry is the baseline
  std::vector<std::uint64_t> seeds;
  double duration = 900.0;
  int threads = 0;  // 0 => hardware concurrency
  bool keep_series = true;
};

struct RunResult {
  std::string controller;
  std::size_t controller_index = 0;
  std::uint64_t seed = 0;
  MetricsSummary metrics;
  std::vector<IntersectionStats> intersections;
  std::vector<TimeSample> series;
  std::uint64_t final_hash = 0;
  double wall_seconds = 0.0;
  std::string error;
};

struct MetricDef {
  std::string key;
  std::string label;
  std::string unit;
  bool lower_is_better = true;
};
const std::vector<MetricDef>& comparison_metrics();
std::optional<double> metric_value(const MetricsSummary& m, const std::string& key);

struct MetricStat {
  std::string key;
  double mean = 0.0;
  double ci95 = 0.0;
  int n = 0;
};

struct ControllerAggregate {
  std::string controller;
  std::vector<MetricStat> metrics;
};

struct MetricComparison {
  std::string key;
  double baseline_mean = 0.0;
  double candidate_mean = 0.0;
  double diff_mean = 0.0;   // candidate - baseline, paired by seed
  double diff_ci95 = 0.0;
  std::optional<double> pct_change;  // nullopt when the baseline is ~0
  int candidate_better = 0;          // seeds where the candidate was strictly better
  int n = 0;
};

struct ControllerComparison {
  std::string baseline;
  std::string candidate;
  std::vector<MetricComparison> metrics;
};

struct ExperimentResult {
  std::string id;
  std::string scenario_id;
  double duration = 0.0;
  std::vector<std::uint64_t> seeds;
  std::vector<std::string> controllers;
  std::vector<RunResult> runs;  // ordered by (controller, seed) regardless of thread timing
  std::vector<ControllerAggregate> aggregates;
  std::vector<ControllerComparison> comparisons;
  std::vector<std::string> explanations;
  double wall_seconds = 0.0;
  bool cancelled = false;
};

using ProgressFn = std::function<void(int done, int total)>;

class ExperimentRunner {
 public:
  static ExperimentResult run(const ExperimentSpec& spec, const ProgressFn& progress = {},
                              const std::atomic<bool>* cancel = nullptr);
  static RunResult run_single(const ScenarioConfig& scenario, const ControllerConfig& controller, std::uint64_t seed,
                              double duration, bool keep_series);
  static void analyse(ExperimentResult& result);
};

}  // namespace traffic
