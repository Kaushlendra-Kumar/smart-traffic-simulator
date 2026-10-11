#pragma once
// Commands that change simulation semantics. They are applied by the simulation
// thread between steps, validated by the engine, and recorded in the command log
// (with the step index) so runs can be replayed deterministically.

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "traffic/core/config.hpp"
#include "traffic/core/types.hpp"

namespace traffic {

struct CmdSetDemand { double multiplier = 1.0; };
struct CmdSetPedestrianDemand { double multiplier = 1.0; };
struct CmdSetVehicleType { VehicleType type = VehicleType::Bus; bool enabled = true; };
struct CmdCloseRoad { RoadId road = kInvalidRoad; };
struct CmdOpenRoad { RoadId road = kInvalidRoad; };
struct CmdBlockLane { RoadId road = kInvalidRoad; int lane = 0; double position = 0.0; };
struct CmdClearLane { RoadId road = kInvalidRoad; int lane = 0; };
struct CmdSetController { ControllerConfig config; };
struct CmdSetSignalPlan { NodeId node = kInvalidNode; std::vector<double> greens; };
struct CmdSetPhaseTiming { NodeId node = kInvalidNode; int phase = 0; double min_green = 7; double max_green = 60; };
struct CmdSetSpeedLimit { RoadId road = kInvalidRoad; double kmh = 50.0; };
struct CmdSpawnVehicle { VehicleType type = VehicleType::Emergency; NodeId origin = kInvalidNode; NodeId destination = kInvalidNode; };

using EngineCommand =
    std::variant<CmdSetDemand, CmdSetPedestrianDemand, CmdSetVehicleType, CmdCloseRoad, CmdOpenRoad, CmdBlockLane,
                 CmdClearLane, CmdSetController, CmdSetSignalPlan, CmdSetPhaseTiming, CmdSetSpeedLimit, CmdSpawnVehicle>;

struct CommandResult {
  bool ok = true;
  std::string error;
  static CommandResult success() { return {}; }
  static CommandResult fail(std::string e) { return {false, std::move(e)}; }
};

struct LoggedCommand {
  std::uint64_t step = 0;  // applied immediately before this step executed
  EngineCommand command;
};

std::string command_name(const EngineCommand& c);

}  // namespace traffic
