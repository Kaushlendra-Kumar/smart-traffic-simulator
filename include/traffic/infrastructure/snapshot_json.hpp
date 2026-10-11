#pragma once
// Wire format (protocol version 1). See docs/api-specification.md.

#include <string>

#include <nlohmann/json.hpp>

#include "traffic/core/simulation_engine.hpp"
#include "traffic/core/snapshot.hpp"
#include "traffic/experiments/experiment_runner.hpp"

namespace traffic::io {

inline constexpr int kProtocolVersion = 1;

/// Static geometry: nodes, roads/lanes, movements with connector paths, crosswalks, phases.
nlohmann::json network_to_json(const SimulationEngine& engine);
/// Compact dynamic state. Vehicles are arrays [id, x, y, heading, speed, type, length]
/// with positions rounded to 1 cm to keep frames small.
std::string snapshot_to_json(const SimulationSnapshot& snap, std::uint64_t seq);
nlohmann::json metrics_to_json(const MetricsSummary& m);
nlohmann::json series_to_json(const std::vector<TimeSample>& series);
nlohmann::json intersections_to_json(const std::vector<IntersectionStats>& stats);

}  // namespace traffic::io
