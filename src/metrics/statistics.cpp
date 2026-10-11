#include "traffic/metrics/statistics.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace traffic::stats {

double mean(const std::vector<double>& xs) {
  if (xs.empty()) return 0.0;
  return std::accumulate(xs.begin(), xs.end(), 0.0) / static_cast<double>(xs.size());
}

double stddev(const std::vector<double>& xs) {
  if (xs.size() < 2) return 0.0;
  const double m = mean(xs);
  double ss = 0.0;
  for (double x : xs) ss += (x - m) * (x - m);
  return std::sqrt(ss / static_cast<double>(xs.size() - 1));
}

std::optional<double> percentile(std::vector<double> xs, double q) {
  if (xs.empty()) return std::nullopt;
  std::sort(xs.begin(), xs.end());
  q = std::clamp(q, 0.0, 1.0);
  const double pos = q * static_cast<double>(xs.size() - 1);
  const auto lo = static_cast<std::size_t>(std::floor(pos));
  const auto hi = static_cast<std::size_t>(std::ceil(pos));
  const double frac = pos - static_cast<double>(lo);
  return xs[lo] + (xs[hi] - xs[lo]) * frac;
}

double t_critical_95(int dof) {
  static const double table[] = {0,     12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
                                 2.201, 2.179,  2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086, 2.080,
                                 2.074, 2.069,  2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042};
  if (dof <= 0) return 0.0;
  if (dof <= 30) return table[dof];
  if (dof <= 60) return 2.000;
  if (dof <= 120) return 1.980;
  return 1.960;
}

double ci95_half_width(const std::vector<double>& xs) {
  if (xs.size() < 2) return 0.0;
  return t_critical_95(static_cast<int>(xs.size()) - 1) * stddev(xs) / std::sqrt(static_cast<double>(xs.size()));
}

}  // namespace traffic::stats
