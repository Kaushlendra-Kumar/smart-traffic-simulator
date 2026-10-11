#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "traffic/core/config.hpp"

namespace traffic::api {

/// Thread-safe scenario library backed by a directory of JSON files. Every scenario
/// is validated by building an engine before it is accepted.
class ScenarioManager {
 public:
  explicit ScenarioManager(std::string directory) : dir_(std::move(directory)) {}

  /// Loads every *.json file; invalid files are skipped and reported.
  std::vector<std::string> load_all();

  struct Summary {
    std::string id, name, description;
    bool grid = false;
  };
  [[nodiscard]] std::vector<Summary> list() const;
  [[nodiscard]] std::shared_ptr<const ScenarioConfig> get(const std::string& id) const;

  enum class SaveResult { Created, Updated, Conflict, NotFound, Invalid, IoError };
  /// create=true rejects existing ids; create=false requires an existing id.
  SaveResult save(const ScenarioConfig& scenario, bool create, std::vector<std::string>& errors);

 private:
  std::string dir_;
  mutable std::mutex mu_;
  std::map<std::string, std::shared_ptr<const ScenarioConfig>> scenarios_;
};

}  // namespace traffic::api
