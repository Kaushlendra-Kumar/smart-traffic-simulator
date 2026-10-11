#include <gtest/gtest.h>

#include <cmath>

#include "traffic/core/geometry.hpp"
#include "traffic/core/rng.hpp"
#include "traffic/core/simulation_clock.hpp"
#include "traffic/metrics/statistics.hpp"

using namespace traffic;

TEST(Geometry, SegmentIntersection) {
  EXPECT_TRUE(segments_intersect({0, 0}, {2, 2}, {0, 2}, {2, 0}));
  EXPECT_FALSE(segments_intersect({0, 0}, {1, 0}, {0, 1}, {1, 1}));
  EXPECT_FALSE(segments_intersect({0, 0}, {1, 1}, {1, 1}, {2, 0}));  // touching endpoints don't count
}

TEST(Geometry, PolylineArcLengthAndInterpolation) {
  Polyline p({{0, 0}, {3, 0}, {3, 4}});
  EXPECT_DOUBLE_EQ(p.length(), 7.0);
  EXPECT_NEAR(p.point_at(5.0).x, 3.0, 1e-12);
  EXPECT_NEAR(p.point_at(5.0).y, 2.0, 1e-12);
  EXPECT_NEAR(p.tangent_at(1.0).x, 1.0, 1e-12);
  EXPECT_NEAR(p.tangent_at(6.0).y, 1.0, 1e-12);
  EXPECT_EQ(p.point_at(-5), (Vec2{0, 0}));  // clamped
}

TEST(Geometry, BezierEndpoints) {
  auto b = quadratic_bezier({0, 0}, {10, 0}, {10, 10}, 16);
  EXPECT_EQ(b.points().front(), (Vec2{0, 0}));
  EXPECT_EQ(b.points().back(), (Vec2{10, 10}));
  EXPECT_GT(b.length(), std::hypot(10.0, 10.0));
  EXPECT_LT(b.length(), 20.0);
}

TEST(Rng, DeterministicAndStreamsIndependent) {
  Rng a(42, 1), b(42, 1), c(42, 2);
  for (int i = 0; i < 1000; ++i) {
    const double x = a.uniform();
    EXPECT_EQ(x, b.uniform());
    EXPECT_GE(x, 0.0);
    EXPECT_LT(x, 1.0);
  }
  Rng a2(42, 1);
  int same = 0;
  for (int i = 0; i < 100; ++i) same += a2.uniform() == c.uniform() ? 1 : 0;
  EXPECT_EQ(same, 0);
}

TEST(Rng, UniformMeanIsReasonable) {
  Rng r(7);
  double sum = 0;
  for (int i = 0; i < 100000; ++i) sum += r.uniform();
  EXPECT_NEAR(sum / 100000, 0.5, 0.01);
}

TEST(Clock, TimeDerivedFromStepsNeverDrifts) {
  SimulationClock c(0.1);
  for (int i = 0; i < 36000; ++i) c.advance();
  EXPECT_DOUBLE_EQ(c.time(), 3600.0);
}

TEST(Statistics, MeanStdPercentile) {
  std::vector<double> xs{1, 2, 3, 4, 5};
  EXPECT_DOUBLE_EQ(stats::mean(xs), 3.0);
  EXPECT_NEAR(stats::stddev(xs), std::sqrt(2.5), 1e-12);
  EXPECT_DOUBLE_EQ(*stats::percentile(xs, 0.5), 3.0);
  EXPECT_DOUBLE_EQ(*stats::percentile(xs, 0.0), 1.0);
  EXPECT_DOUBLE_EQ(*stats::percentile(xs, 1.0), 5.0);
  EXPECT_DOUBLE_EQ(*stats::percentile(xs, 0.25), 2.0);
  EXPECT_FALSE(stats::percentile({}, 0.5).has_value());
}

TEST(Statistics, ConfidenceInterval) {
  EXPECT_DOUBLE_EQ(stats::ci95_half_width({5.0}), 0.0);
  std::vector<double> xs{10, 12, 14, 16, 18};
  const double expected = 2.776 * stats::stddev(xs) / std::sqrt(5.0);
  EXPECT_NEAR(stats::ci95_half_width(xs), expected, 1e-9);
  EXPECT_DOUBLE_EQ(stats::t_critical_95(1000), 1.96);
}
