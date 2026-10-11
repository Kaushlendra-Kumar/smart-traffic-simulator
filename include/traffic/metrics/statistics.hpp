#pragma once
#include <optional>
#include <vector>

namespace traffic::stats {

double mean(const std::vector<double>& xs);
/// Sample standard deviation (n-1). 0 for fewer than two samples.
double stddev(const std::vector<double>& xs);
/// Linear-interpolated percentile (q in [0,1]); nullopt for empty input.
std::optional<double> percentile(std::vector<double> xs, double q);
/// Two-sided Student-t critical value for 95% confidence with `dof` degrees of freedom.
double t_critical_95(int dof);
/// Half-width of the 95% confidence interval of the mean (0 for n < 2).
double ci95_half_width(const std::vector<double>& xs);

}  // namespace traffic::stats
