#pragma once
// Validates untrusted JSON commands (from REST or WebSocket) into typed commands.

#include <optional>
#include <string>
#include <variant>

#include <nlohmann/json.hpp>

#include "traffic/core/commands.hpp"

namespace traffic {

/// Commands handled by the coordinator itself (they change how/when time advances,
/// not what the simulation computes, except load/reset/seek which rebuild the engine).
struct ControlCommand {
  enum class Kind { Start, Pause, Resume, Reset, Step, SetSpeed, Seek, Load };
  Kind kind = Kind::Pause;
  int steps = 1;
  double speed = 1.0;
  double time = 0.0;
  std::string scenario_id;           // Load: resolved by the caller
  std::optional<std::uint64_t> seed;  // Load/Reset: optional new seed
};

using ParsedCommand = std::variant<ControlCommand, EngineCommand>;

struct ParseCommandResult {
  std::optional<ParsedCommand> command;
  std::string error;
};

ParseCommandResult parse_command(const nlohmann::json& j);
nlohmann::json engine_command_to_json(const EngineCommand& c);
std::string control_command_name(ControlCommand::Kind k);

}  // namespace traffic
