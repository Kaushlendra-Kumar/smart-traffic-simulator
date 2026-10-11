#include "traffic/infrastructure/experiment_io.hpp"

#include <sstream>

#include "traffic/infrastructure/scenario_io.hpp"
#include "traffic/infrastructure/snapshot_json.hpp"

namespace traffic::io {

using nlohmann::json;

std::optional<ExperimentSpec> experiment_spec_from_json(const json& j, const ScenarioConfig& scenario,
                                                        std::vector<std::string>& errors) {
  if (!j.is_object()) {
    errors.push_back("experiment request must be an object");
    return std::nullopt;
  }
  static const std::vector<std::string> known{"scenario", "controllers", "seeds", "seed_count", "base_seed", "duration", "threads"};
  for (const auto& [k, _] : j.items())
    if (std::find(known.begin(), known.end(), k) == known.end()) errors.push_back(k + ": unknown key");

  ExperimentSpec spec;
  spec.scenario = scenario;
  spec.duration = scenario.duration;
  if (j.contains("duration")) {
    if (!j["duration"].is_number() || j["duration"].get<double>() < 60 || j["duration"].get<double>() > 14400)
      errors.push_back("duration: must be a number in [60, 14400] s");
    else spec.duration = j["duration"].get<double>();
  }
  if (spec.duration <= scenario.warmup) errors.push_back("duration must exceed the scenario warm-up (" + std::to_string(scenario.warmup) + " s)");
  if (j.contains("seeds")) {
    if (!j["seeds"].is_array() || j["seeds"].empty() || j["seeds"].size() > 50) errors.push_back("seeds: 1..50 non-negative integers");
    else
      for (const auto& s : j["seeds"]) {
        if (!s.is_number_integer() || s.get<long long>() < 0) {
          errors.push_back("seeds: entries must be non-negative integers");
          break;
        }
        spec.seeds.push_back(s.get<std::uint64_t>());
      }
  } else {
    int count = 5;
    std::uint64_t base = scenario.seed;
    if (j.contains("seed_count")) {
      if (!j["seed_count"].is_number_integer() || j["seed_count"].get<int>() < 1 || j["seed_count"].get<int>() > 50)
        errors.push_back("seed_count: must be an integer in [1, 50]");
      else count = j["seed_count"].get<int>();
    }
    if (j.contains("base_seed")) {
      if (!j["base_seed"].is_number_integer() || j["base_seed"].get<long long>() < 0) errors.push_back("base_seed: non-negative integer");
      else base = j["base_seed"].get<std::uint64_t>();
    }
    for (int i = 0; i < count; ++i) spec.seeds.push_back(base + static_cast<std::uint64_t>(i));
  }
  if (!j.contains("controllers") || !j["controllers"].is_array() || j["controllers"].size() < 1 || j["controllers"].size() > 6) {
    errors.push_back("controllers: provide 1..6 controller configs (the first is the baseline)");
  } else {
    for (std::size_t i = 0; i < j["controllers"].size(); ++i) {
      auto c = controller_from_json(j["controllers"][i], errors, "controllers[" + std::to_string(i) + "]");
      if (c) spec.controllers.push_back(*c);
    }
    // Labels must be unique so results can be told apart.
    for (std::size_t a = 0; a < spec.controllers.size(); ++a)
      for (std::size_t b = a + 1; b < spec.controllers.size(); ++b)
        if (spec.controllers[a].display_name() == spec.controllers[b].display_name())
          spec.controllers[b].label = spec.controllers[b].display_name() + " #" + std::to_string(b + 1);
  }
  if (j.contains("threads")) {
    if (!j["threads"].is_number_integer() || j["threads"].get<int>() < 0 || j["threads"].get<int>() > 64)
      errors.push_back("threads: integer in [0, 64]");
    else spec.threads = j["threads"].get<int>();
  }
  if (!errors.empty()) return std::nullopt;
  return spec;
}

json experiment_result_to_json(const ExperimentResult& r, bool include_series) {
  json runs = json::array();
  for (const auto& run : r.runs) {
    json rj{{"controller", run.controller},
            {"controller_index", run.controller_index},
            {"seed", run.seed},
            {"metrics", metrics_to_json(run.metrics)},
            {"intersections", intersections_to_json(run.intersections)},
            {"final_hash", std::to_string(run.final_hash)},
            {"wall_seconds", run.wall_seconds}};
    if (!run.error.empty()) rj["error"] = run.error;
    if (include_series) rj["series"] = series_to_json(run.series);
    runs.push_back(rj);
  }
  json metrics_meta = json::array();
  for (const auto& d : comparison_metrics())
    metrics_meta.push_back({{"key", d.key}, {"label", d.label}, {"unit", d.unit}, {"lower_is_better", d.lower_is_better}});
  json aggregates = json::array();
  for (const auto& a : r.aggregates) {
    json m = json::object();
    for (const auto& s : a.metrics) m[s.key] = {{"mean", s.mean}, {"ci95", s.ci95}, {"n", s.n}};
    aggregates.push_back({{"controller", a.controller}, {"metrics", m}});
  }
  json comparisons = json::array();
  for (const auto& c : r.comparisons) {
    json m = json::object();
    for (const auto& x : c.metrics)
      m[x.key] = {{"baseline_mean", x.baseline_mean}, {"candidate_mean", x.candidate_mean}, {"diff_mean", x.diff_mean},
                  {"diff_ci95", x.diff_ci95}, {"pct_change", x.pct_change ? json(*x.pct_change) : json(nullptr)},
                  {"candidate_better", x.candidate_better}, {"n", x.n}};
    comparisons.push_back({{"baseline", c.baseline}, {"candidate", c.candidate}, {"metrics", m}});
  }
  return json{{"id", r.id},
              {"scenario_id", r.scenario_id},
              {"duration", r.duration},
              {"seeds", r.seeds},
              {"controllers", r.controllers},
              {"metric_definitions", metrics_meta},
              {"aggregates", aggregates},
              {"comparisons", comparisons},
              {"explanations", r.explanations},
              {"runs", runs},
              {"wall_seconds", r.wall_seconds},
              {"cancelled", r.cancelled}};
}

namespace {
std::string csv_escape(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += "\"\"";
    else out += c;
  }
  return out + "\"";
}
}  // namespace

std::string experiment_result_to_csv(const ExperimentResult& r, const ExperimentSpec& spec) {
  std::ostringstream os;
  os << "experiment_id,scenario_id,controller,controller_config_json,seed,duration_s,warmup_s,dt_s";
  for (const auto& d : comparison_metrics()) {
    std::string unit = d.unit == "veh/h" ? "veh_per_h" : d.unit;
    os << "," << d.key << "_" << unit;
  }
  os << ",spawned_veh,unserved_veh,in_network_at_end_veh,backlog_at_end_veh,red_light_guard_events,following_violations,final_state_hash\n";
  for (const auto& run : r.runs) {
    const ControllerConfig& cfg = spec.controllers.at(run.controller_index);
    os << csv_escape(r.id) << "," << csv_escape(r.scenario_id) << "," << csv_escape(run.controller) << ","
       << csv_escape(controller_to_json(cfg).dump()) << "," << run.seed << "," << r.duration << "," << spec.scenario.warmup
       << "," << spec.scenario.dt;
    for (const auto& d : comparison_metrics()) {
      os << ",";
      if (auto v = metric_value(run.metrics, d.key)) os << *v;
    }
    os << "," << run.metrics.spawned << "," << run.metrics.unserved << "," << run.metrics.in_network << ","
       << run.metrics.backlog << "," << run.metrics.red_light_guard_events << "," << run.metrics.following_violations
       << "," << run.final_hash << "\n";
  }
  return os.str();
}

}  // namespace traffic::io
