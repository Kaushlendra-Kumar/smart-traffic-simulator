#include "api/scenario_manager.hpp"

#include <filesystem>
#include <fstream>

#include "traffic/core/simulation_engine.hpp"
#include "traffic/infrastructure/scenario_io.hpp"

namespace traffic::api {

namespace fs = std::filesystem;

std::vector<std::string> ScenarioManager::load_all() {
  std::vector<std::string> problems;
  std::error_code ec;
  if (!fs::is_directory(dir_, ec)) {
    problems.push_back("scenario directory not found: " + dir_);
    return problems;
  }
  std::map<std::string, std::shared_ptr<const ScenarioConfig>> loaded;
  for (const auto& entry : fs::directory_iterator(dir_, ec)) {
    if (entry.path().extension() != ".json") continue;
    auto parsed = io::scenario_from_file(entry.path().string());
    if (!parsed.scenario) {
      problems.push_back(entry.path().filename().string() + ": " + parsed.errors.front());
      continue;
    }
    auto created = SimulationEngine::create(*parsed.scenario);
    if (!created.engine) {
      problems.push_back(entry.path().filename().string() + ": " + created.errors.front());
      continue;
    }
    const std::string id = parsed.scenario->id;  // read before moving the scenario
    if (loaded.contains(id)) {
      problems.push_back(entry.path().filename().string() + ": duplicate scenario id '" + id + "'");
      continue;
    }
    loaded[id] = std::make_shared<const ScenarioConfig>(std::move(*parsed.scenario));
  }
  std::lock_guard lock(mu_);
  scenarios_ = std::move(loaded);
  return problems;
}

std::vector<ScenarioManager::Summary> ScenarioManager::list() const {
  std::lock_guard lock(mu_);
  std::vector<Summary> out;
  for (const auto& [id, s] : scenarios_) out.push_back({id, s->name, s->description, s->network.grid.has_value()});
  return out;
}

std::shared_ptr<const ScenarioConfig> ScenarioManager::get(const std::string& id) const {
  std::lock_guard lock(mu_);
  auto it = scenarios_.find(id);
  return it == scenarios_.end() ? nullptr : it->second;
}

ScenarioManager::SaveResult ScenarioManager::save(const ScenarioConfig& scenario, bool create,
                                                  std::vector<std::string>& errors) {
  auto created = SimulationEngine::create(scenario);
  if (!created.engine) {
    errors = created.errors;
    return SaveResult::Invalid;
  }
  std::lock_guard lock(mu_);
  const bool exists = scenarios_.contains(scenario.id);
  if (create && exists) return SaveResult::Conflict;
  if (!create && !exists) return SaveResult::NotFound;
  // Write to a temporary file and rename so a crash never leaves a half-written scenario.
  const fs::path final_path = fs::path(dir_) / (scenario.id + ".json");
  const fs::path tmp = fs::path(dir_) / (scenario.id + ".json.tmp");
  {
    std::ofstream out(tmp);
    if (!out) {
      errors.push_back("cannot write " + tmp.string());
      return SaveResult::IoError;
    }
    out << io::scenario_to_json(scenario).dump(2) << "\n";
  }
  std::error_code ec;
  fs::rename(tmp, final_path, ec);
  if (ec) {
    errors.push_back("cannot write " + final_path.string() + ": " + ec.message());
    return SaveResult::IoError;
  }
  scenarios_[scenario.id] = std::make_shared<const ScenarioConfig>(scenario);
  return exists ? SaveResult::Updated : SaveResult::Created;
}

}  // namespace traffic::api
