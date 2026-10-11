#include "traffic/core/commands.hpp"

namespace traffic {

namespace {
template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
}  // namespace

std::string command_name(const EngineCommand& c) {
  return std::visit(overloaded{
                        [](const CmdSetDemand&) { return std::string("set_demand"); },
                        [](const CmdSetPedestrianDemand&) { return std::string("set_pedestrian_demand"); },
                        [](const CmdSetVehicleType&) { return std::string("set_vehicle_type"); },
                        [](const CmdCloseRoad&) { return std::string("close_road"); },
                        [](const CmdOpenRoad&) { return std::string("open_road"); },
                        [](const CmdBlockLane&) { return std::string("block_lane"); },
                        [](const CmdClearLane&) { return std::string("clear_lane"); },
                        [](const CmdSetController&) { return std::string("set_controller"); },
                        [](const CmdSetSignalPlan&) { return std::string("set_signal_plan"); },
                        [](const CmdSetPhaseTiming&) { return std::string("set_phase_timing"); },
                        [](const CmdSetSpeedLimit&) { return std::string("set_speed_limit"); },
                        [](const CmdSpawnVehicle&) { return std::string("spawn_vehicle"); },
                    },
                    c);
}

}  // namespace traffic
