#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "traffic/experiments/experiment_runner.hpp"

namespace traffic::io {

/// Parses {"controllers":[...], "seeds":[...] | "seed_count":N, "base_seed":S, "duration":D, "threads":T}.
/// The scenario is supplied by the caller (resolved from an id).
std::optional<ExperimentSpec> experiment_spec_from_json(const nlohmann::json& j, const ScenarioConfig& scenario,
                                                        std::vector<std::string>& errors);
nlohmann::json experiment_result_to_json(const ExperimentResult& r, bool include_series);
/// One row per run; column names carry units; includes scenario id, controller config and seed.
std::string experiment_result_to_csv(const ExperimentResult& r, const ExperimentSpec& spec);

}  // namespace traffic::io
