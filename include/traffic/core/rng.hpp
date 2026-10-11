#pragma once
// Portable, seedable PRNG. std::uniform_real_distribution is implementation-defined,
// so we derive doubles ourselves to get identical sequences on every platform.

#include <cmath>
#include <cstdint>

namespace traffic {

class Rng {
 public:
  explicit Rng(std::uint64_t seed = 1, std::uint64_t stream = 0) { reseed(seed, stream); }

  void reseed(std::uint64_t seed, std::uint64_t stream = 0) {
    std::uint64_t x = seed ^ (0x9E3779B97F4A7C15ULL * (stream + 1));
    for (auto& s : s_) s = splitmix(x);
  }

  std::uint64_t next() {  // xoshiro256**
    const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
  }

  /// Uniform in [0, 1).
  double uniform() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }
  double uniform(double lo, double hi) { return lo + (hi - lo) * uniform(); }

 private:
  static std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
  static std::uint64_t splitmix(std::uint64_t& x) {
    std::uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  std::uint64_t s_[4]{};
};

/// Independent RNG streams so that, e.g., controller behaviour can never change the
/// realised vehicle demand (which only consumes the Demand stream).
enum class RngStream : std::uint64_t { Demand = 1, VehicleChoice = 2, Pedestrians = 3 };

}  // namespace traffic
