#include "traffic/experiments/experiment_runner.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <thread>

#include "traffic/metrics/statistics.hpp"

namespace traffic {

const std::vector<MetricDef>& comparison_metrics() {
  static const std::vector<MetricDef> defs = {
      {"avg_delay_all", "Average delay (incl. unfinished trips)", "s", true},
      {"avg_delay", "Average delay (completed trips)", "s", true},
      {"avg_travel_time", "Average travel time", "s", true},
      {"median_travel_time", "Median travel time", "s", true},
      {"p95_travel_time", "95th percentile travel time", "s", true},
      {"avg_stops", "Stops per vehicle", "stops", true},
      {"avg_stopped_time", "Stopped time per vehicle", "s", true},
      {"avg_queue", "Average queued vehicles (time-weighted)", "veh", true},
      {"max_queue", "Maximum approach queue", "veh", true},
      {"throughput_vph", "Throughput", "veh/h", false},
      {"measured_completed", "Completed trips", "veh", false},
      {"ped_avg_wait", "Pedestrian average wait", "s", true},
      {"phase_switches", "Phase changes", "count", true},
      {"clearance_fraction", "Time in yellow/all-red", "fraction", true},
  };
  return defs;
}

std::optional<double> metric_value(const MetricsSummary& m, const std::string& key) {
  if (key == "avg_delay_all") return m.avg_delay_all;
  if (key == "avg_delay") return m.avg_delay;
  if (key == "avg_travel_time") return m.avg_travel_time;
  if (key == "median_travel_time") return m.median_travel_time;
  if (key == "p95_travel_time") return m.p95_travel_time;
  if (key == "avg_stops") return m.avg_stops;
  if (key == "avg_stopped_time") return m.avg_stopped_time;
  if (key == "avg_queue") return m.avg_queue;
  if (key == "max_queue") return static_cast<double>(m.max_queue);
  if (key == "throughput_vph") return m.throughput_vph;
  if (key == "measured_completed") return static_cast<double>(m.measured_completed);
  if (key == "ped_avg_wait") return m.ped_avg_wait;
  if (key == "phase_switches") return static_cast<double>(m.phase_switches);
  if (key == "clearance_fraction") return m.clearance_fraction;
  return std::nullopt;
}

RunResult ExperimentRunner::run_single(const ScenarioConfig& scenario, const ControllerConfig& controller,
                                       std::uint64_t seed, double duration, bool keep_series) {
  RunResult r;
  r.controller = controller.display_name();
  r.seed = seed;
  const auto t0 = std::chrono::steady_clock::now();
  ScenarioConfig cfg = scenario;
  cfg.seed = seed;
  cfg.controller = controller;
  auto created = SimulationEngine::create(cfg);
  if (!created.engine) {
    r.error = created.errors.empty() ? "failed to create engine" : created.errors.front();
    return r;
  }
  SimulationEngine& eng = *created.engine;
  eng.run_until(duration);
  r.metrics = eng.metrics();
  r.intersections = eng.intersection_stats();
  if (keep_series) r.series = eng.series();
  r.final_hash = eng.state_hash();
  r.wall_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return r;
}

ExperimentResult ExperimentRunner::run(const ExperimentSpec& spec, const ProgressFn& progress,
                                       const std::atomic<bool>* cancel) {
  ExperimentResult result;
  result.id = spec.id;
  result.scenario_id = spec.scenario.id;
  result.duration = spec.duration;
  result.seeds = spec.seeds;
  for (const auto& c : spec.controllers) result.controllers.push_back(c.display_name());
  const auto t0 = std::chrono::steady_clock::now();

  const std::size_t total = spec.controllers.size() * spec.seeds.size();
  result.runs.resize(total);
  std::atomic<std::size_t> next{0};
  std::atomic<int> done{0};
  int threads = spec.threads > 0 ? spec.threads : static_cast<int>(std::thread::hardware_concurrency());
  threads = std::clamp(threads, 1, std::max(1, static_cast<int>(total)));

  auto worker = [&] {
    for (;;) {
      if (cancel && cancel->load()) return;
      const std::size_t job = next.fetch_add(1);
      if (job >= total) return;
      const std::size_t ci = job / spec.seeds.size();
      const std::size_t si = job % spec.seeds.size();
      RunResult r = run_single(spec.scenario, spec.controllers[ci], spec.seeds[si], spec.duration, spec.keep_series);
      r.controller_index = ci;
      result.runs[job] = std::move(r);  // each job writes only its own slot
      const int d = ++done;
      if (progress) progress(d, static_cast<int>(total));
    }
  };
  std::vector<std::thread> pool;
  for (int i = 0; i < threads; ++i) pool.emplace_back(worker);
  for (auto& t : pool) t.join();

  result.cancelled = done.load() < static_cast<int>(total);
  result.wall_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (!result.cancelled) analyse(result);
  return result;
}

namespace {
/// Human-readable controller names for explanations (labels set by users pass through).
std::string pretty(const std::string& name) {
  if (name == "fixed_time") return "Fixed-time";
  if (name == "adaptive") return "Adaptive";
  if (name == "max_pressure") return "Max-pressure";
  return name;
}
}  // namespace

void ExperimentRunner::analyse(ExperimentResult& result) {
  const std::size_t nc = result.controllers.size();
  const std::size_t ns = result.seeds.size();
  if (nc == 0 || ns == 0 || result.runs.size() != nc * ns) return;
  auto run_at = [&](std::size_t c, std::size_t s) -> const RunResult& { return result.runs[c * ns + s]; };

  for (const auto& r : result.runs)
    if (!r.error.empty()) {
      result.explanations.push_back("Run failed (" + r.controller + ", seed " + std::to_string(r.seed) + "): " + r.error);
      return;
    }

  // ---- per-controller aggregates
  for (std::size_t c = 0; c < nc; ++c) {
    ControllerAggregate agg;
    agg.controller = result.controllers[c];
    for (const auto& def : comparison_metrics()) {
      std::vector<double> xs;
      for (std::size_t s = 0; s < ns; ++s)
        if (auto v = metric_value(run_at(c, s).metrics, def.key)) xs.push_back(*v);
      agg.metrics.push_back({def.key, stats::mean(xs), stats::ci95_half_width(xs), static_cast<int>(xs.size())});
    }
    result.aggregates.push_back(std::move(agg));
  }

  // ---- paired comparisons against the baseline (controller 0)
  for (std::size_t c = 1; c < nc; ++c) {
    ControllerComparison cmp;
    cmp.baseline = result.controllers[0];
    cmp.candidate = result.controllers[c];
    for (const auto& def : comparison_metrics()) {
      std::vector<double> base, cand, diff;
      int better = 0;
      for (std::size_t s = 0; s < ns; ++s) {
        auto b = metric_value(run_at(0, s).metrics, def.key);
        auto x = metric_value(run_at(c, s).metrics, def.key);
        if (!b || !x) continue;  // e.g. p95 unavailable with < 20 trips
        base.push_back(*b);
        cand.push_back(*x);
        diff.push_back(*x - *b);
        if (def.lower_is_better ? *x < *b : *x > *b) ++better;
      }
      MetricComparison mc;
      mc.key = def.key;
      mc.n = static_cast<int>(diff.size());
      mc.baseline_mean = stats::mean(base);
      mc.candidate_mean = stats::mean(cand);
      mc.diff_mean = stats::mean(diff);
      mc.diff_ci95 = stats::ci95_half_width(diff);
      if (std::abs(mc.baseline_mean) > 1e-9) mc.pct_change = 100.0 * mc.diff_mean / mc.baseline_mean;
      mc.candidate_better = better;
      cmp.metrics.push_back(mc);
    }
    result.comparisons.push_back(std::move(cmp));
  }

  // ---- data-driven explanations
  char buf[512];
  auto find = [](const ControllerComparison& cmp, const std::string& key) -> const MetricComparison* {
    for (const auto& m : cmp.metrics)
      if (m.key == key) return &m;
    return nullptr;
  };
  for (const auto& cmp : result.comparisons) {
    if (const auto* d = find(cmp, "avg_delay_all"); d && d->n > 0) {
      const bool significant = d->n >= 2 && std::abs(d->diff_mean) > d->diff_ci95;
      const char* verdict = d->diff_mean < 0 ? "reduced" : "increased";
      std::snprintf(buf, sizeof buf,
                    "%s %s average delay by %.1f s (%s) versus %s; 95%% CI of the paired difference [%+.1f, %+.1f] s; "
                    "better in %d of %d seeds. %s",
                    pretty(cmp.candidate).c_str(), verdict, std::abs(d->diff_mean),
                    d->pct_change ? (std::to_string(static_cast<int>(std::lround(*d->pct_change))) + "%").c_str() : "n/a",
                    pretty(cmp.baseline).c_str(), d->diff_mean - d->diff_ci95, d->diff_mean + d->diff_ci95, d->candidate_better,
                    d->n,
                    d->n < 2 ? "Use more seeds for a confidence interval."
                    : significant ? "The difference is larger than seed-to-seed noise."
                                  : "The difference is within seed-to-seed noise; it is not statistically distinguishable.");
      result.explanations.push_back(buf);
    }
    const auto* sw = find(cmp, "phase_switches");
    const auto* cl = find(cmp, "clearance_fraction");
    if (sw && cl && sw->n > 0) {
      std::snprintf(buf, sizeof buf,
                    "%s changed phase %.0f times per run versus %.0f for %s, spending %.1f%% of signal time in yellow/all-red "
                    "clearance versus %.1f%%. More switching shortens waits but adds lost time.",
                    pretty(cmp.candidate).c_str(), sw->candidate_mean, sw->baseline_mean, pretty(cmp.baseline).c_str(),
                    100.0 * cl->candidate_mean, 100.0 * cl->baseline_mean);
      result.explanations.push_back(buf);
    }
    if (const auto* p = find(cmp, "ped_avg_wait"); p && p->n > 0 && (p->baseline_mean > 0 || p->candidate_mean > 0)) {
      std::snprintf(buf, sizeof buf, "Pedestrians waited %.1f s on average under %s versus %.1f s under %s.",
                    p->candidate_mean, pretty(cmp.candidate).c_str(), p->baseline_mean, pretty(cmp.baseline).c_str());
      result.explanations.push_back(buf);
    }
  }

  // Green allocation versus demand, summed over seeds, per controller.
  struct Share {
    std::string phase;
    double green = 0, arrivals = 0;
  };
  std::vector<std::map<std::pair<NodeId, std::size_t>, Share>> shares(nc);
  for (std::size_t c = 0; c < nc; ++c)
    for (std::size_t s = 0; s < ns; ++s)
      for (const auto& is : run_at(c, s).intersections)
        for (std::size_t p = 0; p < is.phases.size(); ++p) {
          auto& sh = shares[c][{is.node, p}];
          sh.phase = is.phases[p].name;
          sh.green += is.phases[p].green_time;
          sh.arrivals += is.phases[p].arrivals;
        }
  auto node_totals = [](const std::map<std::pair<NodeId, std::size_t>, Share>& m, NodeId node, double& g, double& a) {
    g = a = 0;
    for (const auto& [key, sh] : m)
      if (key.first == node) {
        g += sh.green;
        a += sh.arrivals;
      }
  };
  // Largest mismatch under the baseline.
  double worst = 0.0;
  std::pair<NodeId, std::size_t> worst_key{kInvalidNode, 0};
  for (const auto& [key, sh] : shares[0]) {
    double g, a;
    node_totals(shares[0], key.first, g, a);
    if (g <= 0 || a <= 0) continue;
    const double mismatch = sh.green / g - sh.arrivals / a;
    if (std::abs(mismatch) > std::abs(worst)) {
      worst = mismatch;
      worst_key = key;
    }
  }
  if (worst_key.first != kInvalidNode && std::abs(worst) > 0.05) {
    double g, a;
    node_totals(shares[0], worst_key.first, g, a);
    const auto& sh = shares[0][worst_key];
    std::snprintf(buf, sizeof buf,
                  "At node %d, %s gave phase \"%s\" %.0f%% of green time while it carried %.0f%% of arriving vehicles.",
                  worst_key.first, pretty(result.controllers[0]).c_str(), sh.phase.c_str(), 100.0 * sh.green / g,
                  100.0 * sh.arrivals / a);
    std::string text = buf;
    for (std::size_t c = 1; c < nc; ++c) {
      auto it = shares[c].find(worst_key);
      double gc, ac;
      node_totals(shares[c], worst_key.first, gc, ac);
      if (it == shares[c].end() || gc <= 0) continue;
      std::snprintf(buf, sizeof buf, " %s gave it %.0f%%.", pretty(result.controllers[c]).c_str(), 100.0 * it->second.green / gc);
      text += buf;
    }
    result.explanations.push_back(text);
  }
}

}  // namespace traffic
