#include "traffic/infrastructure/command_parser.hpp"

#include <sstream>

#include "traffic/infrastructure/scenario_io.hpp"

namespace traffic {

using nlohmann::json;

namespace {

template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
// Explicit deduction guide: older compilers (e.g. Clang 15) lack aggregate CTAD.
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

std::string fmt(double x) {
  std::ostringstream os;
  os << x;
  return os.str();
}

struct Fields {
  const json& j;
  std::string error;

  bool number(const char* key, double& out, double lo, double hi, bool required = true) {
    if (!j.contains(key)) {
      if (required) error = std::string("missing '") + key + "'";
      return !required;
    }
    if (!j[key].is_number()) return fail(std::string("'") + key + "' must be a number");
    const double v = j[key].get<double>();
    if (!(v >= lo && v <= hi))
      return fail(std::string("'") + key + "' must be in [" + fmt(lo) + ", " + fmt(hi) + "]");
    out = v;
    return true;
  }
  bool integer(const char* key, int& out, int lo, int hi, bool required = true) {
    if (!j.contains(key)) {
      if (required) error = std::string("missing '") + key + "'";
      return !required;
    }
    if (!j[key].is_number_integer()) return fail(std::string("'") + key + "' must be an integer");
    const auto v = j[key].get<long long>();
    if (v < lo || v > hi) return fail(std::string("'") + key + "' out of range");
    out = static_cast<int>(v);
    return true;
  }
  bool vehicle_type(const char* key, VehicleType& out) {
    if (!j.contains(key) || !j[key].is_string()) return fail(std::string("'") + key + "' must be car|bus|emergency");
    const auto s = j[key].get<std::string>();
    if (s == "car") out = VehicleType::Car;
    else if (s == "bus") out = VehicleType::Bus;
    else if (s == "emergency") out = VehicleType::Emergency;
    else return fail(std::string("'") + key + "' must be car|bus|emergency");
    return true;
  }
  bool fail(std::string e) {
    error = std::move(e);
    return false;
  }
};

const char* type_name(VehicleType t) {
  switch (t) {
    case VehicleType::Car: return "car";
    case VehicleType::Bus: return "bus";
    case VehicleType::Emergency: return "emergency";
  }
  return "car";
}

}  // namespace

std::string control_command_name(ControlCommand::Kind k) {
  switch (k) {
    case ControlCommand::Kind::Start: return "start";
    case ControlCommand::Kind::Pause: return "pause";
    case ControlCommand::Kind::Resume: return "resume";
    case ControlCommand::Kind::Reset: return "reset";
    case ControlCommand::Kind::Step: return "step";
    case ControlCommand::Kind::SetSpeed: return "set_speed";
    case ControlCommand::Kind::Seek: return "seek";
    case ControlCommand::Kind::Load: return "load_scenario";
  }
  return "?";
}

ParseCommandResult parse_command(const json& j) {
  ParseCommandResult res;
  if (!j.is_object() || !j.contains("cmd") || !j["cmd"].is_string()) {
    res.error = "command must be an object with a string 'cmd'";
    return res;
  }
  const std::string cmd = j["cmd"].get<std::string>();
  Fields f{j, {}};
  auto control = [&](ControlCommand::Kind k) {
    ControlCommand c;
    c.kind = k;
    return c;
  };

  if (cmd == "start") res.command = control(ControlCommand::Kind::Start);
  else if (cmd == "pause") res.command = control(ControlCommand::Kind::Pause);
  else if (cmd == "resume") res.command = control(ControlCommand::Kind::Resume);
  else if (cmd == "reset") {
    auto c = control(ControlCommand::Kind::Reset);
    if (j.contains("seed")) {
      if (!j["seed"].is_number_integer() || j["seed"].get<long long>() < 0) f.fail("'seed' must be a non-negative integer");
      else c.seed = j["seed"].get<std::uint64_t>();
    }
    res.command = c;
  } else if (cmd == "step") {
    auto c = control(ControlCommand::Kind::Step);
    if (f.integer("count", c.steps, 1, 100000, false)) res.command = c;
  } else if (cmd == "set_speed") {
    auto c = control(ControlCommand::Kind::SetSpeed);
    if (f.number("speed", c.speed, 0.1, 50.0)) res.command = c;
  } else if (cmd == "seek") {
    auto c = control(ControlCommand::Kind::Seek);
    if (f.number("time", c.time, 0.0, 86400.0)) res.command = c;
  } else if (cmd == "load_scenario") {
    auto c = control(ControlCommand::Kind::Load);
    if (!j.contains("scenario") || !j["scenario"].is_string()) f.fail("'scenario' must be a scenario id");
    else {
      c.scenario_id = j["scenario"].get<std::string>();
      if (j.contains("seed")) {
        if (!j["seed"].is_number_integer() || j["seed"].get<long long>() < 0) f.fail("'seed' must be a non-negative integer");
        else c.seed = j["seed"].get<std::uint64_t>();
      }
      if (f.error.empty()) res.command = c;
    }
  } else if (cmd == "set_demand") {
    CmdSetDemand c;
    if (f.number("multiplier", c.multiplier, 0, 5)) res.command = EngineCommand{c};
  } else if (cmd == "set_pedestrian_demand") {
    CmdSetPedestrianDemand c;
    if (f.number("multiplier", c.multiplier, 0, 10)) res.command = EngineCommand{c};
  } else if (cmd == "set_vehicle_type") {
    CmdSetVehicleType c;
    if (f.vehicle_type("type", c.type)) {
      if (!j.contains("enabled") || !j["enabled"].is_boolean()) f.fail("'enabled' must be true/false");
      else {
        c.enabled = j["enabled"].get<bool>();
        res.command = EngineCommand{c};
      }
    }
  } else if (cmd == "close_road" || cmd == "open_road") {
    int road = -1;
    if (f.integer("road", road, 0, 1000000)) {
      if (cmd == "close_road") res.command = EngineCommand{CmdCloseRoad{road}};
      else res.command = EngineCommand{CmdOpenRoad{road}};
    }
  } else if (cmd == "block_lane") {
    CmdBlockLane c;
    if (f.integer("road", c.road, 0, 1000000) && f.integer("lane", c.lane, 0, 10) &&
        f.number("position", c.position, 0, 100000))
      res.command = EngineCommand{c};
  } else if (cmd == "clear_lane") {
    CmdClearLane c;
    if (f.integer("road", c.road, 0, 1000000) && f.integer("lane", c.lane, 0, 10)) res.command = EngineCommand{c};
  } else if (cmd == "set_controller") {
    if (!j.contains("controller")) f.fail("missing 'controller'");
    else {
      std::vector<std::string> errors;
      auto cfg = io::controller_from_json(j["controller"], errors);
      if (!cfg) f.fail(errors.empty() ? "invalid controller" : errors.front());
      else res.command = EngineCommand{CmdSetController{*cfg}};
    }
  } else if (cmd == "set_signal_plan") {
    CmdSetSignalPlan c;
    if (f.integer("node", c.node, 0, 1000000)) {
      if (!j.contains("greens") || !j["greens"].is_array() || j["greens"].size() > 16) f.fail("'greens' must be an array");
      else {
        for (const auto& g : j["greens"]) {
          if (!g.is_number()) {
            f.fail("'greens' must contain numbers");
            break;
          }
          c.greens.push_back(g.get<double>());
        }
        if (f.error.empty()) res.command = EngineCommand{c};
      }
    }
  } else if (cmd == "set_phase_timing") {
    CmdSetPhaseTiming c;
    if (f.integer("node", c.node, 0, 1000000) && f.integer("phase", c.phase, 0, 64) &&
        f.number("min_green", c.min_green, 1, 300) && f.number("max_green", c.max_green, 1, 600))
      res.command = EngineCommand{c};
  } else if (cmd == "set_speed_limit") {
    CmdSetSpeedLimit c;
    if (f.integer("road", c.road, 0, 1000000) && f.number("kmh", c.kmh, 10, 130)) res.command = EngineCommand{c};
  } else if (cmd == "spawn_vehicle") {
    CmdSpawnVehicle c;
    if (f.vehicle_type("type", c.type) && f.integer("origin", c.origin, 0, 1000000) &&
        f.integer("destination", c.destination, 0, 1000000))
      res.command = EngineCommand{c};
  } else {
    f.fail("unknown command '" + cmd + "'");
  }
  if (!f.error.empty()) {
    res.command.reset();
    res.error = f.error;
  } else if (!res.command) {
    res.error = "invalid command";
  }
  return res;
}

json engine_command_to_json(const EngineCommand& c) {
  return std::visit(
      overloaded{
          [](const CmdSetDemand& x) { return json{{"cmd", "set_demand"}, {"multiplier", x.multiplier}}; },
          [](const CmdSetPedestrianDemand& x) { return json{{"cmd", "set_pedestrian_demand"}, {"multiplier", x.multiplier}}; },
          [](const CmdSetVehicleType& x) { return json{{"cmd", "set_vehicle_type"}, {"type", type_name(x.type)}, {"enabled", x.enabled}}; },
          [](const CmdCloseRoad& x) { return json{{"cmd", "close_road"}, {"road", x.road}}; },
          [](const CmdOpenRoad& x) { return json{{"cmd", "open_road"}, {"road", x.road}}; },
          [](const CmdBlockLane& x) { return json{{"cmd", "block_lane"}, {"road", x.road}, {"lane", x.lane}, {"position", x.position}}; },
          [](const CmdClearLane& x) { return json{{"cmd", "clear_lane"}, {"road", x.road}, {"lane", x.lane}}; },
          [](const CmdSetController& x) { return json{{"cmd", "set_controller"}, {"controller", io::controller_to_json(x.config)}}; },
          [](const CmdSetSignalPlan& x) { return json{{"cmd", "set_signal_plan"}, {"node", x.node}, {"greens", x.greens}}; },
          [](const CmdSetPhaseTiming& x) {
            return json{{"cmd", "set_phase_timing"}, {"node", x.node}, {"phase", x.phase}, {"min_green", x.min_green}, {"max_green", x.max_green}};
          },
          [](const CmdSetSpeedLimit& x) { return json{{"cmd", "set_speed_limit"}, {"road", x.road}, {"kmh", x.kmh}}; },
          [](const CmdSpawnVehicle& x) {
            return json{{"cmd", "spawn_vehicle"}, {"type", type_name(x.type)}, {"origin", x.origin}, {"destination", x.destination}};
          },
      },
      c);
}

}  // namespace traffic
