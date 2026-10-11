#pragma once
#include <memory>
#include <stdexcept>
#include <string>

#include "traffic/core/simulation_engine.hpp"

namespace traffic::test {

/// Four-arm junction: centre node 0; arms N=1 (0,arm), E=2, S=3, W=4.
inline ScenarioConfig single_intersection(double rate_vph = 300, int lanes = 2, double arm = 300) {
  ScenarioConfig c;
  c.id = "test-single";
  c.network.nodes = {{0, 0, 0, {}}, {1, 0, arm, {}}, {2, arm, 0, {}}, {3, 0, -arm, {}}, {4, -arm, 0, {}}};
  for (int i = 1; i <= 4; ++i) {
    RoadSpec r;
    r.from = i;
    r.to = 0;
    r.lanes = lanes;
    c.network.roads.push_back(r);
  }
  c.demand.default_rate_vph = rate_vph;
  c.warmup = 0;
  return c;
}

inline ScenarioConfig grid(int rows, int cols, double rate_vph = 200) {
  ScenarioConfig c;
  c.id = "test-grid";
  GridSpec g;
  g.rows = rows;
  g.cols = cols;
  c.network.grid = g;
  c.demand.default_rate_vph = rate_vph;
  c.warmup = 0;
  return c;
}

inline std::unique_ptr<SimulationEngine> make(const ScenarioConfig& c) {
  auto r = SimulationEngine::create(c);
  if (!r.engine) throw std::runtime_error(r.errors.empty() ? "create failed" : r.errors.front());
  return std::move(r.engine);
}

inline const Vehicle* find_vehicle(const SimulationEngine& e, VehicleId id) {
  const Vehicle* found = nullptr;
  e.for_each_vehicle([&](const Vehicle& v) {
    if (v.id == id) found = &v;
  });
  return found;
}

}  // namespace traffic::test
