#pragma once
// Fundamental identifiers and enums shared by every engine subsystem.
// Units: metres, seconds, metres/second. World frame: x = east, y = north.

#include <cstdint>
#include <string_view>

namespace traffic {

using NodeId = std::int32_t;
using RoadId = std::int32_t;
using MovementId = std::int32_t;
using VehicleId = std::uint64_t;
using PedestrianId = std::uint64_t;
using SlotIndex = std::uint32_t;  // index into the vehicle slot array (stable while alive)

inline constexpr NodeId kInvalidNode = -1;
inline constexpr RoadId kInvalidRoad = -1;
inline constexpr MovementId kInvalidMovement = -1;
inline constexpr SlotIndex kInvalidSlot = 0xFFFFFFFFu;

enum class VehicleType : std::uint8_t { Car = 0, Bus = 1, Emergency = 2 };
inline constexpr int kVehicleTypeCount = 3;

enum class TurnType : std::uint8_t { Left = 0, Straight = 1, Right = 2, UTurn = 3 };

enum class LightState : std::uint8_t { Red = 0, Yellow = 1, Green = 2 };

enum class WalkState : std::uint8_t { DontWalk = 0, Walk = 1, Clearance = 2 };

enum class SignalStage : std::uint8_t { Green = 0, Yellow = 1, AllRed = 2 };

constexpr std::string_view to_string(VehicleType t) {
  switch (t) {
    case VehicleType::Car: return "car";
    case VehicleType::Bus: return "bus";
    case VehicleType::Emergency: return "emergency";
  }
  return "?";
}

constexpr std::string_view to_string(TurnType t) {
  switch (t) {
    case TurnType::Left: return "L";
    case TurnType::Straight: return "S";
    case TurnType::Right: return "R";
    case TurnType::UTurn: return "U";
  }
  return "?";
}

constexpr std::string_view to_string(SignalStage s) {
  switch (s) {
    case SignalStage::Green: return "green";
    case SignalStage::Yellow: return "yellow";
    case SignalStage::AllRed: return "all_red";
  }
  return "?";
}

}  // namespace traffic
