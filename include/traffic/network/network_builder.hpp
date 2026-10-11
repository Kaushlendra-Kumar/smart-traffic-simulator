#pragma once
#include <string>
#include <vector>

#include "traffic/core/config.hpp"
#include "traffic/network/road_network.hpp"

namespace traffic {

struct BuildResult {
  RoadNetwork network;
  std::vector<std::string> errors;
  [[nodiscard]] bool ok() const { return errors.empty(); }
};

/// Expands a GridSpec into explicit nodes and two-way roads.
/// Interior intersection (r, c) gets id r*cols + c; boundary nodes follow in the
/// order: north edge (west→east), south edge, west edge (north→south), east edge.
NetworkSpec expand_grid(const GridSpec& grid);

/// Builds geometry, movements, connectors and crosswalks, then validates.
BuildResult build_network(const NetworkSpec& spec);

}  // namespace traffic
