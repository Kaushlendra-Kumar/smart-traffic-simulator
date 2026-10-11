// Engine benchmarks. Vehicle counts are *measured* (reported as counters), not assumed:
// the network is filled by real demand until the target population is reached.
//
// Run:  ./build/release/benchmarks/traffic_benchmarks --benchmark_counters_tabular=true

#include <benchmark/benchmark.h>

#include <algorithm>
#include <map>
#include <memory>

#include "traffic/core/simulation_engine.hpp"
#include "traffic/infrastructure/snapshot_json.hpp"
#include "traffic/network/routing.hpp"

using namespace traffic;

namespace {

struct Loaded {
  std::unique_ptr<SimulationEngine> engine;
  int population = 0;
};

// Grid sizes chosen so the target population fits comfortably on the roads.
int grid_side(int target) {
  if (target <= 150) return 3;
  if (target <= 1200) return 6;
  if (target <= 5500) return 10;
  return 14;
}

Loaded& loaded(int target) {
  static std::map<int, Loaded> cache;  // build each population once, then copy-free reuse
  auto it = cache.find(target);
  if (it != cache.end()) return it->second;
  ScenarioConfig c;
  c.id = "bench";
  c.seed = 12345;
  GridSpec g;
  g.rows = g.cols = grid_side(target);
  g.block_length = 250;
  c.network.grid = g;
  c.demand.default_rate_vph = 1500;
  c.pedestrians.rate_per_hour = 60;
  c.controller.type = "adaptive";
  c.warmup = 0;
  auto created = SimulationEngine::create(c);
  Loaded l;
  l.engine = std::move(created.engine);
  // Fill with real traffic, then throttle demand to hold the population roughly steady.
  for (int i = 0; i < 200000 && l.engine->vehicle_count() < target; ++i) l.engine->step();
  l.engine->apply(CmdSetDemand{0.35});
  l.population = l.engine->vehicle_count();
  return cache.emplace(target, std::move(l)).first->second;
}

void BM_EngineStep(benchmark::State& state) {
  Loaded& l = loaded(static_cast<int>(state.range(0)));
  SimulationEngine& e = *l.engine;
  double vehicle_updates = 0;
  for (auto _ : state) {
    vehicle_updates += e.vehicle_count();
    e.step();
  }
  state.counters["vehicles"] = static_cast<double>(e.vehicle_count());
  state.counters["intersections"] = static_cast<double>(e.signals().size());
  state.counters["steps/s"] = benchmark::Counter(static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
  state.counters["sim_s/wall_s"] =
      benchmark::Counter(static_cast<double>(state.iterations()) * e.dt(), benchmark::Counter::kIsRate);
  state.counters["veh_updates/s"] = benchmark::Counter(vehicle_updates, benchmark::Counter::kIsRate);
}
BENCHMARK(BM_EngineStep)->Arg(100)->Arg(1000)->Arg(5000)->Arg(10000)->Unit(benchmark::kMicrosecond)->MinTime(2.0);

void BM_SnapshotBuildAndSerialize(benchmark::State& state) {
  Loaded& l = loaded(static_cast<int>(state.range(0)));
  std::size_t bytes = 0;
  std::uint64_t seq = 0;
  for (auto _ : state) {
    const auto snap = l.engine->snapshot();
    const std::string json = io::snapshot_to_json(snap, ++seq);
    bytes = json.size();
    benchmark::DoNotOptimize(json.data());
  }
  state.counters["vehicles"] = static_cast<double>(l.engine->vehicle_count());
  state.counters["bytes"] = static_cast<double>(bytes);
}
BENCHMARK(BM_SnapshotBuildAndSerialize)->Arg(100)->Arg(1000)->Arg(5000)->Arg(10000)->Unit(benchmark::kMicrosecond);

void BM_Routing(benchmark::State& state) {
  Loaded& l = loaded(static_cast<int>(state.range(0)));
  const auto& net = l.engine->network();
  const auto boundary = net.boundary_nodes();
  std::size_t i = 0;
  for (auto _ : state) {
    const NodeId o = boundary[i % boundary.size()];
    const NodeId d = boundary[(i * 7 + 3) % boundary.size()];
    ++i;
    if (o == d) continue;
    benchmark::DoNotOptimize(route_between(net, o, d, free_flow_cost));
  }
  state.counters["roads"] = static_cast<double>(net.roads.size());
}
BENCHMARK(BM_Routing)->Arg(100)->Arg(10000)->Unit(benchmark::kMicrosecond);

}  // namespace

BENCHMARK_MAIN();
