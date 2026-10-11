#pragma once
// JSON <-> ScenarioConfig. Parsing is strict: wrong types, out-of-range values and
// unknown keys are reported with their JSON path (e.g. "demand.sources[2].rate_vph").

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "traffic/core/config.hpp"

namespace traffic::io {

struct ParseResult {
  std::optional<ScenarioConfig> scenario;
  std::vector<std::string> errors;
};

ParseResult scenario_from_json(const nlohmann::json& j);
ParseResult scenario_from_string(const std::string& text);
ParseResult scenario_from_file(const std::string& path);
nlohmann::json scenario_to_json(const ScenarioConfig& s);

std::optional<ControllerConfig> controller_from_json(const nlohmann::json& j, std::vector<std::string>& errors,
                                                     const std::string& path = "controller");
nlohmann::json controller_to_json(const ControllerConfig& c);

}  // namespace traffic::io
