#pragma once
#include <cstdint>

namespace traffic {

/// Fixed-timestep simulation clock. Time is derived from the integer step count
/// (t = step * dt) so it never accumulates floating-point drift and never moves
/// backwards. It is independent of wall-clock time.
class SimulationClock {
 public:
  explicit SimulationClock(double dt = 0.1) : dt_(dt) {}
  [[nodiscard]] double dt() const { return dt_; }
  [[nodiscard]] std::uint64_t step() const { return step_; }
  [[nodiscard]] double time() const { return static_cast<double>(step_) * dt_; }
  void advance() { ++step_; }

 private:
  double dt_;
  std::uint64_t step_ = 0;
};

}  // namespace traffic
